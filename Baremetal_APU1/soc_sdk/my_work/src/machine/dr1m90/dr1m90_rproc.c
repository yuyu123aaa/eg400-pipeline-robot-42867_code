/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * dr1m90_rproc.c —— 安路 dr1m90 平台 remoteproc 实现（APU1 裸机侧）
 *
 * 三件核心事：
 *   1. 把"收到对端通知"的 SGI 注册成中断处理函数；
 *   2. 把共享内存/资源表映射成非缓存（NORM_NONCACHE）区域；
 *   3. notify()：通过 GIC 向 APU0 发一条 SGI，告诉它"我这边有新消息了"。
 */

#include <openamp/remoteproc.h>
#include <metal/atomic.h>
#include <metal/device.h>
#include <metal/irq.h>
#include "platform_info.h"
/* SDK 的 GICv3 接口：AL_GICV3_IrqGroupEnum / AlGicv3_RaiseSgi 都在这里，
 * 必须显式包含。**不要**在本文件里重复定义那个枚举，否则编译会报重定义错误。 */
#include "al_gicv3.h"

extern struct metal_io_ops io_ops;

/* ★ 2026-09-25 新增：SGI 收到的累计次数。
 *  用途：platform_poll() 的诊断行要区分两种情况 ——
 *    · 计数一直是 0  -> Linux 压根没发出 kick（去查内核侧）
 *    · 计数在涨      -> kick 到了，但 nokick 标志没被清（查中断处理/优先级）
 *  只在这个中断处理里 +1，读侧用下面的 getter。 */
static volatile uint32_t g_sgi_rx_count;

uint32_t dr1m90_sgi_rx_count(void)
{
	return g_sgi_rx_count;
}

/* APU0 发来 SGI 时进这里：只清一个标志，真正的处理在 platform_poll() 里做 */
static int dr1m90_proc_irq_handler(int vect_id, void *data)
{
	struct remoteproc *rproc = data;
	struct remoteproc_priv *prproc;

	if (!rproc)
		return METAL_IRQ_NOT_HANDLED;
	prproc = rproc->priv;

	g_sgi_rx_count++;

	/* ------------------------------------------------------------------
	 * 【联调关键打印】判断 SGI 方向对不对，就靠这一行。
	 *
	 *   echo start 之后，裸机串口上应该出现 SGI_NUM = 当前配置的监听号
	 *   （默认应该是 14）。
	 *
	 *   - 看到 14  -> 配置正确，继续往下调
	 *   - 看到 15  -> 方向反了：把 apu1_openamp_config.h 里
	 *                 APU1_SGI_SWAP 改成 1 重新编译
	 *   - 一条都没有 -> 问题不在 SGI 号，去查 GIC 初始化 / 内核 vring_sgis
	 *
	 * 调通之后可以把下面这两行注释掉，免得刷屏（每来一包都会打）。
	 * ------------------------------------------------------------------ */
	al_printf("[APU1] SGI %d received from APU0 (expect %d)\r\n",
		  vect_id, SGI_NOTIFICATION);

	atomic_flag_clear(&prproc->nokick);
	return METAL_IRQ_HANDLED;
}

static struct remoteproc *
dr1m90_proc_init(struct remoteproc *rproc,
		 const struct remoteproc_ops *ops, void *arg)
{
	struct remoteproc_priv *prproc = arg;
	struct metal_device *dev;
	unsigned int irq_vect;
	int ret;

	if (!rproc || !prproc || !ops)
		return NULL;

	ret = metal_device_open(prproc->gic_bus_name, prproc->gic_name, &dev);
	if (ret) {
		al_printf("failed to open GIC device: %d.\r\n", ret);
		return NULL;
	}
	rproc->priv = prproc;
	prproc->gic_dev = dev;
	prproc->gic_io = metal_device_io_region(dev, 0);
	if (!prproc->gic_io)
		goto err1;
	rproc->ops = ops;
	atomic_flag_test_and_set(&prproc->nokick);

	/* 注册并打开"接收对端通知"的 SGI */
	irq_vect = prproc->irq_notification;
	metal_irq_register(irq_vect, dr1m90_proc_irq_handler, rproc);
	metal_irq_enable(irq_vect);
	al_printf("Successfully initialize remoteproc.\r\n");
	return rproc;
err1:
	metal_device_close(dev);
	return NULL;
}

static void dr1m90_proc_remove(struct remoteproc *rproc)
{
	struct remoteproc_priv *prproc;
	struct metal_device *dev;

	if (!rproc)
		return;
	prproc = rproc->priv;

	/* ------------------------------------------------------------------
	 * 注意：dr1m90_proc_init() 里注册的是 irq_notification（"接收"号），
	 *       所以这里必须注销同一个号，才是对称的。
	 *
	 *       创龙官方原版这里写的是 irq_to_notify（"发送"号），
	 *       与 init 不对称 —— 那个号从来没被 metal_irq_register 过。
	 *       因为 remove 通常只在卸载时走一次，原版没暴露问题，
	 *       但如果将来做"重新加载固件"，就会留下一个没释放的中断。
	 *       这里改成对称的写法。
	 * ------------------------------------------------------------------ */
	metal_irq_disable(prproc->irq_notification);
	metal_irq_unregister(prproc->irq_notification);

	dev = prproc->gic_dev;
	if (dev)
		metal_device_close(dev);
}

static void *
dr1m90_proc_mmap(struct remoteproc *rproc, metal_phys_addr_t *pa,
		 metal_phys_addr_t *da, size_t size,
		 unsigned int attribute, struct metal_io_region **io)
{
	struct remoteproc_mem *mem;
	metal_phys_addr_t lpa, lda;
	struct metal_io_region *tmpio;

	lpa = *pa;
	lda = *da;

	if (lpa == METAL_BAD_PHYS && lda == METAL_BAD_PHYS)
		return NULL;
	if (lpa == METAL_BAD_PHYS)
		lpa = lda;
	if (lda == METAL_BAD_PHYS)
		lda = lpa;

	if (!attribute)
		attribute = NORM_NONCACHE | STRONG_ORDERED;

	mem = metal_allocate_memory(sizeof(*mem));
	if (!mem)
		return NULL;
	tmpio = metal_allocate_memory(sizeof(*tmpio));
	if (!tmpio) {
		metal_free_memory(mem);
		return NULL;
	}
	remoteproc_init_mem(mem, NULL, lpa, lda, size, tmpio);
	/* 本平台 va == pa（裸机无 MMU 重映射，页表里配成非缓存） */
	metal_io_init(tmpio, (void *)lpa, &mem->pa, size,
		      sizeof(metal_phys_addr_t) << 3, attribute, &io_ops);
	remoteproc_add_mem(rproc, mem);
	*pa = lpa;
	*da = lda;
	if (io)
		*io = tmpio;
	return metal_io_phys_to_virt(tmpio, mem->pa);
}

/* ---------------------------------------------------------------------------
 * 注意：这里**不要**再定义 AL_GICV3_IrqGroupEnum！
 *
 * 它已经由 SDK 头文件 al_gicv3.h（第 315-319 行）定义：
 *     typedef enum { AL_GICV3_G1S, AL_GICV3_G1NS, AL_GICV3_G0 } AL_GICV3_IrqGroupEnum;
 *
 * 我们再定义一遍会报：
 *     error: redeclaration of enumerator 'AL_GICV3_G1S'
 * 本文件已经在开头 #include "al_gicv3.h"，直接用即可。
 * --------------------------------------------------------------------------- */

/* APU1 -> APU0：发一条 SGI 通知 Linux 侧"去取消息" */
static int dr1m90_proc_notify(struct remoteproc *rproc, uint32_t id)
{
	struct remoteproc_priv *prproc;

	(void)id;
	if (!rproc)
		return -1;
	prproc = rproc->priv;

	AlGicv3_RaiseSgi(SGI_TO_NOTIFY, AL_GICV3_G1NS, prproc->cpu_id);

	return 0;
}

const struct remoteproc_ops dr1m90_proc_ops = {
	.init = dr1m90_proc_init,
	.remove = dr1m90_proc_remove,
	.mmap = dr1m90_proc_mmap,
	.notify = dr1m90_proc_notify,
	.start = NULL,
	.stop = NULL,
	.shutdown = NULL,
};
