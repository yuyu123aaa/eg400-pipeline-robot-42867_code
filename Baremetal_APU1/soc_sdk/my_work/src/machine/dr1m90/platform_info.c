/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * platform_info.c —— remoteproc / rpmsg 平台适配（APU1 裸机侧）
 *
 * 作用：把 APU1 这一侧的"通知通道"和"共享内存"接进 OpenAMP/libmetal 框架。
 */

#include <openamp/remoteproc.h>
#include <openamp/rpmsg_virtio.h>
#include <metal/atomic.h>
#include <metal/device.h>
#include <metal/io.h>
#include <metal/irq.h>
#include <metal/sys.h>
#include "platform_info.h"
#include "rsc_table.h"
#include <errno.h>

/* ★ 2026-09-25 新增：给 platform_poll() 的等待加上界所需的两个接口
 *   （apu_time_ms 取时间、apu_wdt_feed 等待期间喂狗，避免看门狗误咬） */
#include "apu_time.h"
#include "apu_wdt.h"

/* 对端（跑 Linux 的 APU0）CPU ID = 0 */
#define MASTER_CPU_ID   0UL

/* ★ 2026-09-25 修正：原来这里是无条件 wfi。
 *
 *  为什么必须换掉 wfi（这是整机被狗咬的**最后一环**）：
 *      wfi = "睡到有中断来才醒"。如果 Linux 一直不发 SGI（核间中断），
 *      那么 CPU 会**永远睡在 wfi 里**，它后面的"超时判断"根本执行不到，
 *      连 apu_time_ms() 都不再前进 ——
 *      所以任何"等了多少秒就放弃"的逻辑都形同虚设。
 *      实测证据：加了超时+诊断后，日志里那行
 *      `[APU1][poll] waiting for APU0 kick` 依然**一行都没有**。
 *
 *  换成"睡一小段就醒"的定时延时：这样每 APU_WAIT_SLICE_MS 都会回到循环，
 *  既让超时判断真正生效，也让 apu_time_ms() 继续前进（诊断能打出来）。
 *
 *  ⚠️ 代价：不再 wfi 省电。但这段等待最多 APU_POLL_TIMEOUT_MS(900ms)，
 *     而且 APU1 本来就要持续跑主循环，这点功耗不值得用"可能永久睡死"去换。
 *     想恢复 wfi 语义可定义 APU_RPROC_WAIT_USE_WFI=1（不推荐）。 */
#ifndef APU_WAIT_SLICE_MS
#define APU_WAIT_SLICE_MS   1U
#endif

#if defined(APU_RPROC_WAIT_USE_WFI) && (APU_RPROC_WAIT_USE_WFI == 1)
#define _rproc_wait() asm volatile("wfi")
#else
#define _rproc_wait() AlSys_MDelay(APU_WAIT_SLICE_MS)
#endif

#define __section_t(S)          __attribute__((__section__(#S)))
#define __atomic_sec            __section_t(.atomic_sec)

extern const struct remoteproc_ops dr1m90_proc_ops;

/* GIC Distributor 作为 metal device 注册，用于产生 SGI */
static metal_phys_addr_t scugic_phys_addr = SCUGIC_DIST_BASE;
struct metal_device scugic_device = {
	.name = SCUGIC_DEV_NAME,
	.bus = NULL,
	.num_regions = 1,
	.regions = {
		{
			.virt = (void *)SCUGIC_DIST_BASE,
			.physmap = &scugic_phys_addr,
			.size = 0x1000,
			.page_shift = -1UL,
			.page_mask = -1UL,
			.mem_flags = DEVICE_MEMORY,
			.ops = {NULL},
		},
	},
	.node = {NULL},
	.irq_num = 0,
	.irq_info = NULL,
};

static struct remoteproc_priv __atomic_sec rproc_priv = {
	.gic_name = SCUGIC_DEV_NAME,
	.gic_bus_name = SCUGIC_BUS_NAME,
	.gic_dev = NULL,
	.gic_io = NULL,
	.irq_to_notify = SGI_TO_NOTIFY,
	.irq_notification = SGI_NOTIFICATION,
	.cpu_id = MASTER_CPU_ID,
};

static struct remoteproc __atomic_sec rproc_inst;

extern int init_system(void);
extern void cleanup_system(void);

static struct remoteproc *
platform_create_proc(int proc_index, int rsc_index)
{
	void *rsc_table;
	int rsc_size;
	int ret;
	metal_phys_addr_t pa;

	(void)proc_index;
	rsc_table = get_resource_table(rsc_index, &rsc_size);

	/* 注册 GIC 设备，供 dr1m90_proc_ops.notify 产生 SGI */
	(void)metal_register_generic_device(&scugic_device);

	if (!remoteproc_init(&rproc_inst, &dr1m90_proc_ops, &rproc_priv))
		return NULL;

	/* 映射资源表 */
	pa = (metal_phys_addr_t)rsc_table;
	(void *)remoteproc_mmap(&rproc_inst, &pa, NULL, rsc_size,
				NORM_NONCACHE | STRONG_ORDERED,
				&rproc_inst.rsc_io);

	/* 映射共享内存（vdev0buffer） */
	pa = SHARED_MEM_PA;
	(void *)remoteproc_mmap(&rproc_inst, &pa, NULL, SHARED_MEM_SIZE,
				NORM_NONCACHE | STRONG_ORDERED, NULL);

	/* 解析资源表 */
	ret = remoteproc_set_rsc_table(&rproc_inst, rsc_table, rsc_size);
	if (ret) {
		al_printf("Failed to initialize remoteproc\r\n");
		remoteproc_remove(&rproc_inst);
		return NULL;
	}
	al_printf("Initialize remoteproc successfully.\r\n");

	return &rproc_inst;
}

int platform_init(int argc, char *argv[], void **platform)
{
	unsigned long proc_id = 0;
	unsigned long rsc_id = 0;
	struct remoteproc *rproc;

	(void)argc;
	(void)argv;

	if (!platform) {
		al_printf("Failed to initialize platform, NULL pointer.\r\n");
		return -EINVAL;
	}

	/* 初始化 libmetal + GIC，并注册 SGI_NOTIFICATION 的处理入口 */
	init_system();

	rproc = platform_create_proc(proc_id, rsc_id);
	if (!rproc) {
		al_printf("Failed to create remoteproc device.\r\n");
		return -EINVAL;
	}
	*platform = rproc;
	return 0;
}

/* RPMsg virtio 共享缓冲池 */
static struct rpmsg_virtio_shm_pool shpool;

struct rpmsg_device *
platform_create_rpmsg_vdev(void *platform, unsigned int vdev_index,
			   unsigned int role,
			   void (*rst_cb)(struct virtio_device *vdev),
			   rpmsg_ns_bind_cb ns_bind_cb)
{
	struct remoteproc *rproc = platform;
	struct rpmsg_virtio_device *rpmsg_vdev;
	struct virtio_device *vdev;
	void *shbuf;
	struct metal_io_region *shbuf_io;
	int ret;

	rpmsg_vdev = metal_allocate_memory(sizeof(*rpmsg_vdev));
	if (!rpmsg_vdev)
		return NULL;

	shbuf_io = remoteproc_get_io_with_pa(rproc, SHARED_MEM_PA);
	if (!shbuf_io)
		goto err1;

	shbuf = metal_io_phys_to_virt(shbuf_io, SHARED_MEM_PA + SHARED_BUF_OFFSET);

	al_printf("creating remoteproc virtio\r\n");
	vdev = remoteproc_create_virtio(rproc, vdev_index, role, rst_cb);
	if (!vdev) {
		al_printf("failed remoteproc_create_virtio\r\n");
		goto err1;
	}

	al_printf("initializing rpmsg vdev\r\n");
	if (role == VIRTIO_DEV_DRIVER) {
		rpmsg_virtio_init_shm_pool(&shpool, shbuf,
					   (SHARED_MEM_SIZE - SHARED_BUF_OFFSET));
		ret = rpmsg_init_vdev(rpmsg_vdev, vdev, ns_bind_cb,
				      shbuf_io, &shpool);
	} else {
		ret = rpmsg_init_vdev(rpmsg_vdev, vdev, ns_bind_cb,
				      shbuf_io, NULL);
	}
	if (ret) {
		al_printf("failed rpmsg_init_vdev\r\n");
		goto err2;
	}
	al_printf("rpmsg vdev ready\r\n");
	return rpmsg_virtio_get_rpmsg_device(rpmsg_vdev);

err2:
	remoteproc_remove_virtio(rproc, vdev);
err1:
	metal_free_memory(rpmsg_vdev);
	return NULL;
}

/* ===========================================================================
 * ★★ 2026-09-25 重点修正：platform_poll() 原来是**无界等待**，导致整机被狗咬 ★★
 * ===========================================================================
 *  【原来的代码】
 *      while (1) {
 *          if (!(atomic_flag_test_and_set(&prproc->nokick))) {
 *              remoteproc_get_notification(...); break;
 *          }
 *          _rproc_wait();              // wfi，等 Linux 的 SGI
 *      }
 *   只要 Linux 不踢这一脚，这个 while **永远不退出**。
 *
 *  【为什么这是致命的】
 *   主循环（main.c:215-222）是：
 *          while (1) {
 *              platform_poll(priv);     // ← 第一件事就无限等
 *              apu_time_poll();
 *              apu_periodic();
 *              apu_app_periodic();      // ← 唯一常规喂狗点在这里面
 *              apu_testlink_poll();
 *          }
 *   ⇒ Linux 不踢 → platform_poll 不返回 → apu_app_periodic() 永远跑不到
 *     → 没人喂狗 → 4.03 秒后硬件狗咬 → **整板复位**。
 *
 *  【上板实证】putty_s20_14400.log（2026-09-25 19:58）
 *   - 日志里 `[APU1][tick]`（本该每秒一行）**一行都没有** → 主循环从未进入
 *   - `dr1m90_rproc.c:46` 的 "SGI %d received from APU0" **一次都没出现**
 *     （按 apu1_openamp_config.h:139 的判据：一条都没有 = SGI 根本没送到）
 *   - 最后一行为 `business layer init done`，紧接着就是 FSBL 输出（= 复位重来）
 *
 *  【9-25 那次的临时做法 —— 已被 2026-10-17 方案 A 取代，见下方函数头】
 *    ⚠️ 下面三条是"有界等待"时期的做法，保留作历史记录：
 *    ① 有界等待：最多等 APU_POLL_TIMEOUT_MS 就返回，让主循环继续往下跑
 *    ② 等待期间喂狗：用 apu_wdt_feed()，硬狗不会因为这一段被误咬
 *    ③ 每秒一行诊断：打印已等秒数与累计收到的 SGI 数，用来判方向
 *
 *    ❌ 但这三条仍然解决不了**速率**问题：主循环依旧被 500ms 心跳节流成 2Hz，
 *       导致设计 10Hz 的 PID 实际只跑 2Hz。所以 10-17 改成"不等待、直接返回"。
 *
 *  ⚠️ 注意：本修正**不会**让 RPMsg 变通。没有 Linux 的 kick，依然收不到消息。
 *     它做的是把"整机反复复位"变成"能跑但收不到消息"——可观测、可继续排查；
 *     对水下机器人而言，"控制核不被通信问题拖死"本来也是更安全的行为。
 *
 *  ⚠️ 也没有把 kick 丢掉：一旦 Linux 真的踢了，nokick 被中断处理清掉，
 *     下一轮进来立刻走 get_notification 分支，语义与原版一致。
 *
 *  【诊断怎么读】
 *    `[APU1][poll] waiting for APU0 kick %us (SGI rx=%u)`
 *      · SGI rx 一直是 0            -> Linux 压根没发出 kick（去查内核侧）
 *      · SGI rx 在涨、但仍在 waiting -> kick 到了但 nokick 没被清（查中断处理/优先级）
 *      · 打印几行后彻底停住          -> 卡在 _rproc_wait()（查 GIC 唤醒路径）
 * =========================================================================== */
#ifndef APU_POLL_TIMEOUT_MS
/* 取 3000ms，**必须大于诊断周期 1000ms**，否则那行诊断永远来不及打
 * （等 1000ms 才打，可 900ms 就 return 了 —— 那样等于没有诊断）。
 * 3000ms 仍然 < 硬件狗超时 4030ms，且循环里每 1ms 都 apu_wdt_feed()，
 * 所以不会因为这一段被狗咬。 */
#define APU_POLL_TIMEOUT_MS   3000U
#endif

/* SGI 接收计数（定义在 dr1m90_rproc.c），只用于诊断打印 */
extern uint32_t dr1m90_sgi_rx_count(void);

/* 等待 Linux 通过 SGI 通知我们。
 * 返回 0 表示"这一轮结束"，调用方继续跑主循环。
 *
 * ★★ 2026-10-17 改动（方案 A）：**不再阻塞等 kick，改成立即返回** ★★
 * -----------------------------------------------------------------
 *  【改之前的问题】
 *      本函数在收到 APU0 的 SGI 之前会一直 while(1) 里等（每 1ms 醒一次），
 *      而 APU0 的 PING 心跳是 APU_HEARTBEAT_PERIOD_MS = 500ms。
 *      ⇒ **主循环速率被心跳节流成约 2Hz（504ms/圈）**。
 *
 *      而控制流水线 apu_control_pipeline() 是**在主循环里**被调的：
 *          main.c while(1) → apu_app_periodic() → apu_control_pipeline()
 *              → apu_pipe_center_schedule_is_due()（门限 100ms = 10Hz）
 *                  → apu_pipe_center_update()   ← PID 在这里
 *      ⇒ 设计 10Hz 的 PID，实际只跑到 **2Hz** —— 慢了 5 倍，
 *        管道居中/航向纠偏严重滞后，弯道表现就差。
 *      （实测证据：日志 PING:1@4949ms → PING:100@54829ms，
 *        99 次 49880ms = 1.98Hz；`[APU1][loop] iter` 也是同样节奏。）
 *
 *  【改之后】
 *      没有 kick 就**立刻返回**（只做 1ms 让步避免空转烧 CPU）。
 *      ⇒ 主循环不再等心跳，速率提升到数百 Hz，
 *        100ms 的 PID 门限能真正按 10Hz 触发。
 *
 *  【为什么这样是安全的】
 *      · **rpmsg 不会丢**：kick 到达时中断处理把 nokick 清零，
 *        下一次调用（≤1~3ms 后）就会看到并走 get_notification。
 *        延迟反而比原来更小、更确定。
 *      · **看门狗照喂**：主循环每次进入都 apu_wdt_feed()（main.c 第一件事），
 *        本函数里也补喂一次。
 *      · **各周期任务各自有限速门限**，不会因为循环变快而变快：
 *            IMU / 超声        10Hz   （apu_sonar_periodic 内部门限）
 *            控制流水线/PID    100ms  （PIPE_CENTER_CONTROL_PERIOD_TICKS）
 *            三路环境传感器    1000ms （APU_ENV_PERIOD_MS）
 *      · 原来的 3s 超时保护保留为"兜底无意义"——因为根本不再等待了。
 * ========================================================================= */
int platform_poll(void *priv)
{
	struct remoteproc *rproc = priv;
	struct remoteproc_priv *prproc;
	unsigned int flags;

	if (!rproc)
		return 0;

	prproc = rproc->priv;

	/* ---- ① Linux 踢过了？处理完通知立刻返回（与原版语义一致）---- */
	flags = metal_irq_save_disable();
	if (!(atomic_flag_test_and_set(&prproc->nokick))) {
		metal_irq_restore_enable(flags);
		remoteproc_get_notification(rproc, RSC_NOTIFY_ID_ANY);
		return 0;
	}
	metal_irq_restore_enable(flags);

	/* ---- ② 没有 kick：补喂狗 + 1ms 让步，然后立即返回 ---- */
	apu_wdt_feed();
	_rproc_wait();          /* = AlSys_MDelay(1)，避免 100% CPU 空转 */
	return 0;
}

void platform_release_rpmsg_vdev(struct rpmsg_device *rpdev, void *platform)
{
	struct rpmsg_virtio_device *rpvdev;
	struct remoteproc *rproc;

	rpvdev = metal_container_of(rpdev, struct rpmsg_virtio_device, rdev);
	rproc = platform;

	rpmsg_deinit_vdev(rpvdev);
	remoteproc_remove_virtio(rproc, rpvdev->vdev);
}

void platform_cleanup(void *platform)
{
	struct remoteproc *rproc = platform;

	if (rproc)
		remoteproc_remove(rproc);
	cleanup_system();
}
