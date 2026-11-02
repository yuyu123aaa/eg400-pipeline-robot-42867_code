/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_platform.c —— APU1 平台级一次性初始化
 * ===========================================================================
 *  【本文件解决的第一个问题：共享内存两端缓存属性不一致】
 *
 *  现状（以代码为准）：
 *    · APU1 侧：soc_base 的静态页表 al_aarch64_pagetables.S:78-106（DDR_2M_MAPPING
 *      分支）把 0x00000000-0x63FFFFFF 与 0x64000000-0x7FFFFFFF **整段**映射成
 *      2MB 块 + NORM_CACHE。而 vdev0buffer(0x09000000)、vring0(0x0F030000)、
 *      vring1(0x0F034000) 全都落在这个区间里 ⇒ **APU1 看到的是 Cacheable**。
 *    · platform_info.c 确实往 libmetal 传了 NORM_NONCACHE 属性，但
 *      dr1m90_rproc.c:147 的注释"本平台 va == pa（裸机无 MMU 重映射，页表里
 *      配成非缓存）"**是错的** —— libmetal 在裸机上不会去改页表，页表里配的是
 *      cacheable。那段属性只影响 libmetal 自己的 bookkeeping。
 *    · Linux 侧那三块是 reserved-memory + no-map，驱动按 non-cacheable/Device 映射。
 *
 *  两端属性不一致的后果不是"性能差一点"，而是**可能读到旧值或半帧**
 *  —— 表现为偶发丢包、偶发握手失败、偶发死机，正是最难查的那类。
 *
 *  【做法】只改这三块，粒度按"共享缓冲基址 2MB 对齐"来：
 *    界1 = 0x09000000 ...... 0x0A000000   vdev0buffer 头部（RPMsg 报文就住这）
 *    界2 = 0x0F000000 ...... 0x10000000   两条 vring 所在块
 *  两个界都是 2MB 对齐，所以可以**直接用 SDK 现成的 mmu_settlb()**：
 *  它就是"取 2MB 项的地址域 + 换属性位 + 刷 D-cache + 失效 TLB"，
 *  我们只需要把一个正确的属性值传进去，不必自己拼 AArch64 描述符。
 *
 *  NORM_NONCACHE = 0x701（al_mmu.h:50）就是页表项本身用的那个值，
 *  与 SDK 静态表里 NORM_CACHE=0x705 的位布局完全同源，只差 attr 位。
 *
 *  ⚠️ 为什么不像 SDK 的 AlCache_SetMemoryAttr() 那样传 Al_MEM_DMA：
 *    那个枚举被硬编码成 DEVICE_MEM(0x409)，即 Device-nGnRnE ——
 *    对内存来说过强：禁 unaligned，且每次访问都按严格顺序去总线，
 *    vring 操作性能会很难看。这里要的是 Normal-Non-Cacheable。
 *  ⚠️ 为什么不用 4KB 粒度页表：
 *    要改静态 4KB 描述符就得动 soc_base 的 al_aarch64_pagetables.S（SDK 文件），
 *    或者自己拼描述符 —— 两者都比"用 SDK 既有机制 + 2MB 对齐"风险高。
 *    实际被一起设成 non-cacheable 的额外范围只有 0x09000000..0x09020000 与
 *    0x0F038000..0x10000000，都在 16MB 固件保留区内，本核不使用，无副作用。
 *
 *  【第二个问题：镜像是否真住在 0x08000000】
 *  print_config() 只打印 RSC_TABLE_PA 这个**宏**，证明不了资源表真在那个物理
 *  地址上。这里读 &resources 的实际地址来证。
 * ===========================================================================
 */

#include "apu_platform.h"

#include "al_core.h"
#include "al_cache.h"       /* AlCache_FlushDcacheAll / AlCache_InvalidateDcacheAll */
#include "al_mmu.h"         /* mmu_settlb / NORM_NONCACHE */

#include "apu1_openamp_config.h"
#include "rsc_table.h"

/* 声明一下 SDK 里那个符号（al_aarch64_mmu.S:197，声明在 al_mmu.h 未导出，
 *  所以这里按 al_mmu.h 的签名自己 extern 一遍，避免依赖头文件是否带进来） */
extern AL_U64 mmu_settlb(AL_UINTPTR Addr, AL_U64 Attr);

/* 链接脚本符号 */
extern char _end[];
extern char stack_start[];

/* ---------------------------------------------------------------------------
 * 把一段 [Start, End) 设成 Normal-Non-Cacheable。
 * 要求 Start/End 都 2MB 对齐（mmu_settlb 的粒度就是 2MB 块）。
 *
 * ★★ 2026-09-25 修正（实验 A 的"只清自己那点范围"版本）★★
 *
 *  为什么这里要自己清一次、而不能靠 mmu_settlb 内部的全 cache flush：
 *
 *    DR1M90 的两颗 A35 **共享同一个 L2**（设备树 /cpus/l2-cache 的 phandle 被
 *    cpu@0 和 cpu@1 同时引用，逐字节核对过）。而 mmu_settlb() 内部走的是
 *    __asm_flush_dcache_all()，它按 CLIDR_EL1 遍历**所有 cache 层级**、用
 *    set/way 方式发 `dc cisw` —— 在共享 L2 上，这会把**另一个核（Linux）
 *    刚写入、尚未写回 DDR 的 dirty 行一起处理掉**，是随机数据损坏的根源。
 *
 *  正确做法：只对"我要改属性的这段地址"做 VA 方式的 civac（clean +
 *  invalidate），既保证没有残留的 cacheable 副本，又完全不碰 Linux 的行。
 *
 *  顺序很关键：
 *    ① 先按范围 civac  —— 把本核可能还挂在 cache 里的旧属性副本写回并作废，
 *       此时还是 Normal-WB，civac 能正常命中；
 *    ② 再 mmu_settlb 改属性 —— 之后新属性生效；
 *    ③ mmu_settlb 内部会 tlbi，保证旧 TLB 项不残留。
 * --------------------------------------------------------------------------- */
#define APU_PLAT_2M_MASK    (0x200000UL - 1UL)

static int force_noncacheable(AL_UINTPTR start, AL_UINTPTR end, const char *what)
{
	AL_UINTPTR addr;

	if ((start & APU_PLAT_2M_MASK) != 0UL || (end & APU_PLAT_2M_MASK) != 0UL) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][plat] %s: range 0x%lX..0x%lX is NOT 2MB aligned - "
		       "cannot use mmu_settlb()",
		       what, (unsigned long)start, (unsigned long)end);
		return -1;
	}

	/* ① 只清这一段（VA 方式 civac），不碰共享 L2 上属于 Linux 的行 */
	AlCache_FlushAndInvalidateSameDcacheRange(start, end);

	/* ② 逐 2MB 块改属性（mmu_settlb 内部会 tlbi） */
	for (addr = start; addr < end; addr += 0x200000UL) {
		(void)mmu_settlb(addr, (AL_U64)NORM_NONCACHE);
	}

	return 0;
}

int apu_platform_init(void)
{
	static int done;
	AL_UINTPTR buf_end;

	if (done) {
		return 0;
	}
	done = 1;

	/* =====================================================================
	 * ① 共享区改成 non-cacheable
	 * ===================================================================== */
	AL_LOG(AL_LOG_LEVEL_INFO,
	       "----- 共享内存缓存属性对齐（与 Linux 侧一致）-----");

	/* vdev0buffer 起始 2MB 块：APU1_SHARED_MEM_PA 必须 2MB 对齐 */
	if ((APU1_SHARED_MEM_PA & APU_PLAT_2M_MASK) != 0UL) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][plat] APU1_SHARED_MEM_PA=0x%lX is not 2MB aligned - "
		       "shared buffer stays CACHEABLE. Fix apu1_openamp_config.h.",
		       (unsigned long)APU1_SHARED_MEM_PA);
	} else {
		/*
		 * ★★ 2026-09-25 修正：整个 32MB 都要设，不是只设头部 2MB ★★
		 *
		 *  契约是 32MB（apu1_openamp_config.h:37 APU1_SHARED_MEM_SIZE
		 *  = 0x02000000，DTB vdev0buffer@9000000 reg 也是 32MB）。
		 *
		 *  而且内核确实把**整块 32MB** 挂成了 vdev 子设备的 coherent 池：
		 *      drivers/remoteproc/remoteproc_virtio.c:406  pa = mem->dma (0x09000000)
		 *      drivers/remoteproc/remoteproc_virtio.c:410  dma_declare_coherent_memory(dev, pa, mem->da, mem->len)
		 *  于是 RPMsg 的报文缓冲（dma_alloc_coherent）就落在 32MB 里的**任意位置**。
		 *
		 *  原来只设前 2MB 的后果：一旦 Linux 把某个报文缓冲分到 0x09200000
		 *  以上，APU1 按 Normal-WB Cacheable 读、Linux 按 Normal-NonCacheable
		 *  写 ⇒ 两核看到的不是同一份数据（偶发丢包 / 半帧 / 偶发死机）。
		 *  循环 16 次 mmu_settlb，代价可忽略。
		 */
		buf_end = APU1_SHARED_MEM_PA + APU1_SHARED_MEM_SIZE;
		(void)force_noncacheable(APU1_SHARED_MEM_PA, buf_end,
					 "vdev0buffer");
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "vdev0buffer : 0x%08lX-0x%08lX  -> NORM_NONCACHE (全 32MB)",
		       (unsigned long)APU1_SHARED_MEM_PA, (unsigned long)buf_end);
	}

	/* 两条 vring 一起处理：它们同住在 0x0F000000 这个 2MB 块里 */
	{
		AL_UINTPTR vstart = APU1_VRING0_PA & ~APU_PLAT_2M_MASK;
		AL_UINTPTR vend   = vstart + 0x200000UL;

		(void)force_noncacheable(vstart, vend, "vring0+vring1");
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "vring0/1    : 0x%08lX-0x%08lX  -> NORM_NONCACHE "
		       "(含 vring0=0x%08lX vring1=0x%08lX)",
		       (unsigned long)vstart, (unsigned long)vend,
		       (unsigned long)APU1_VRING0_PA, (unsigned long)APU1_VRING1_PA);
	}

	/* =====================================================================
	 * ★★ 2026-09-25：这里原来有两行全 cache 维护，已删除 ★★
	 *
	 *      AlCache_FlushDcacheAll();        // 删
	 *      AlCache_InvalidateDcacheAll();   // 删
	 *
	 *  删除理由（两条，任一条都足以删）：
	 *
	 *  ① 多余：上面 force_noncacheable() 里已经对**同一段地址**做过
	 *     VA 方式的 civac，属性切换所需的 cache 维护职责已尽。
	 *
	 *  ② 有害：这两行走的是 __asm_dcache_all()（al_aarch64_mmu.S:294-333），
	 *     按 CLIDR_EL1 遍历**所有 cache 层级**、用 **set/way** 方式操作。
	 *     DR1M90 的两颗 A35 共享 L2（设备树已确证），所以在共享 L2 上：
	 *       · FlushDcacheAll  = `dc cisw` —— 把 Linux 尚未写回 DDR 的 dirty
	 *         行提前写回，破坏 Linux 的写缓冲语义；
	 *       · InvalidateDcacheAll = `dc isw`（al_aarch64_mmu.S:277）
	 *         —— **只作废、不写回**，Linux 那些 dirty 行**永久丢失**，
	 *         对应物理地址回读 DDR 旧值。
	 *     结果：APU0(Linux) 内核随机数据"被回滚"，数秒后静默死亡
	 *     （坏点随机，因为丢哪一行取决于作废瞬间 Linux 正在写哪里）。
	 *
	 *  验证：删掉后若 Linux 不再随机死机 → 实锤。
	 * ===================================================================== */

	/* =====================================================================
	 * ② 地址自检：证明镜像真的住在 0x08000000 且没踩到共享缓冲
	 * ===================================================================== */
	{
		extern struct remote_resource_table resources;
		AL_UINTPTR rsc = (AL_UINTPTR)&resources;

		AL_LOG(AL_LOG_LEVEL_INFO,
		       "----- APU1 固件/共享区地址自检 -----");
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "资源表实际地址 : 0x%08lX  (期望 0x%08lX)",
		       (unsigned long)rsc, (unsigned long)APU1_FW_BASE);
		if (rsc != (AL_UINTPTR)APU1_FW_BASE) {
			AL_LOG(AL_LOG_LEVEL_ERROR,
			       "[APU1][plat] 资源表不在固件基址上！Linux 的驱动会在 "
			       "0x%08lX 处找它 -> remoteproc 解析必然失败。检查链接脚本。",
			       (unsigned long)APU1_FW_BASE);
		}

		AL_LOG(AL_LOG_LEVEL_INFO,
		       "镜像末端(_end)  : 0x%08lX   栈底 0x%08lX",
		       (unsigned long)(AL_UINTPTR)_end,
		       (unsigned long)(AL_UINTPTR)stack_start);
		if ((AL_UINTPTR)_end > (AL_UINTPTR)APU1_SHARED_MEM_PA) {
			AL_LOG(AL_LOG_LEVEL_ERROR,
			       "[APU1][plat] 镜像踩进了共享缓冲区 (0x%08lX > 0x%08lX)!",
			       (unsigned long)(AL_UINTPTR)_end,
			       (unsigned long)APU1_SHARED_MEM_PA);
		}
		AL_LOG(AL_LOG_LEVEL_INFO, "------------------------------------");
	}

	return 0;
}
