/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_gic.c —— APU1 裸机侧 GICv3 "本核" 拉起（SLAVE 语义）实现
 * ===========================================================================
 *  设计动因、与 SDK AlGic_Init() 的关系、以及"绝不做什么"，
 *  全部写在 apu_gic.h 顶部。动手改之前请先读那段。
 * ===========================================================================
 */

#include "apu_gic.h"

#include "al_core.h"
#include "al_chip.h"          /* AL_DR1M90_GICD_BASE / AL_DR1M90_GICR_BASE / AL_ARM_CORE_COUNT */
#include "al_gicv3.h"         /* CTLR_* / GICR_WAKER / WAKER_CA_BIT / Gicd_* */
#include "al_gicv3_private.h" /* extern const AL_GICV3_DrvDataStruct *Gicv3DrvData */
#include "al_systimer.h"      /* AlSys_UDelay */

/* al_gic.c 里的那个 static 表：这里只借它的 MPIDR->CorePos 映射 */
extern const AL_GICV3_DrvDataStruct GicData;

/* ---------------------------------------------------------------------------
 * 等待 apu0（Linux）把本核 Redistributor 唤醒。
 *
 * GICv3 规范：GICR_WAKER.ChildrenAsleep(bit2, CA) 会在本核把
 * ProcessorSleep(bit1, PS) 清 0、且完成唤醒流程后自动由硬件清 0。
 * Linux 侧 gic_cpu_init()->gic_enable_redist(true) 做的就是这件事。
 *
 * ⚠️ SDK 的 AlGicv3_Rdist_WaitMasterAwake() 是**没有超时**的
 *    while 循环（al_gicv3_private.c:50）。在"Linux 还没起来"或
 *    "唤醒流程本身出问题"时，它会永远转下去 —— 表现就是
 *    "调试器连得上、断点不响、串口一行都没有"。
 *    所以这里自己带超时地轮询，宁可报错继续跑，也不永久卡死。
 * --------------------------------------------------------------------------- */
#define APU_GIC_WAKE_TIMEOUT_US   2000000U   /* 2 秒：Linux 起来后才放核，正常是微秒级 */
#define APU_GIC_WAKE_POLL_US      100U

/* ---------------------------------------------------------------------------
 * GICD_CTLR 读写
 * ---------------------------------------------------------------------------
 *  ⚠️ **不要用 Gicd_ReadCtlr()/Gicd_SetCtlr()**：它们在 SDK 里是 static inline
 *    （只被 al_gicv3_main.c 用），**没有外部符号**，从别的 .c 调用会在链接期报
 *    `undefined reference to 'Gicd_ReadCtlr'`（本次实测踩过）。
 *    GICD_CTLR 就是 Distributor 的第一个 32 位寄存器，直接用 SDK 的
 *    AL_REG32_READ/WRITE 最稳，位定义仍用 al_gicv3.h 的 CTLR_*_BIT。
 * --------------------------------------------------------------------------- */
static AL_U32 apu_gicd_ctlr_read(void)
{
	return AL_REG32_READ(AL_DR1M90_GICD_BASE);
}

static int wait_rdist_awake(AL_UINTPTR gicr_base)
{
	AL_U32 waited = 0U;
	AL_U32 waker;

	if (gicr_base == 0U) {
		return APU_GIC_ERR_ARG;
	}

	waker = AL_REG32_READ(gicr_base + GICR_WAKER);
	if ((waker & WAKER_CA_BIT) == 0U) {
		return APU_GIC_OK;              /* 已经醒了，最常见的正常路径 */
	}

	while (waited < APU_GIC_WAKE_TIMEOUT_US) {
		AlSys_UDelay(APU_GIC_WAKE_POLL_US);
		waited += APU_GIC_WAKE_POLL_US;

		waker = AL_REG32_READ(gicr_base + GICR_WAKER);
		if ((waker & WAKER_CA_BIT) == 0U) {
			return APU_GIC_OK;
		}
	}

	return APU_GIC_ERR_MASTER;
}

int apu_gic_up(void)
{
	AL_U32 core;
	AL_UINTPTR gicr_base;
	AL_U32 ctlr;

	/* ---- ① 驱动数据：探测 RdistBaseAddrs[0..1]（顺带读 GICD_PIDR2 验版本） ---- */
	if (AlGicv3_DriverInit(&GicData) != AL_OK) {
		AL_LOG(AL_LOG_LEVEL_ERROR, "[APU1][gic] AlGicv3_DriverInit failed");
		return APU_GIC_ERR_ARG;
	}

	core = AlGic_CorePos();
	if (core >= (AL_U32)AL_ARM_CORE_COUNT) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][gic] AlGic_CorePos()=%u out of range (max %u)",
		       (unsigned int)core, (unsigned int)(AL_ARM_CORE_COUNT - 1));
		return APU_GIC_ERR_ARG;
	}

	gicr_base = GicData.RdistBaseAddrs[core];
	if (gicr_base == 0U || gicr_base < 0x1000U) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][gic] RdistBaseAddrs[%u]=0x%lX looks invalid "
		       "(GICR_BASE=0x%lX) - GIC not usable",
		       (unsigned int)core, (unsigned long)gicr_base,
		       (unsigned long)AL_DR1M90_GICR_BASE);
		return APU_GIC_ERR_ARG;
	}

	/* ---- ② ARE_S 兜底 ----------------------------------------------------
	 * AlGicv3_RdistInit() 第 134 行有个断言：
	 *     AL_ASSERT((Gicd_ReadCtlr(GicdBase) & CTLR_ARE_S_BIT) != 0, AL_ERR_ILLEGAL_PARAM);
	 * 因为 SDK 正常流程里 ARE_S 是 AlGicv3_DistInit() 设的 —— 而我们刻意不调它
	 * （Distributor 归 Linux）。所以这里只做**兜底检查**：
	 *   · GICv3 规范要求 ARE 必须为 1（单个 Security state 时 ARE_S 只读为 1），
	 *     正常情况 Linux 早就让它为 1，这里读到就是 1，**什么都不会写**；
	 *   · 万一读到 0（GIC 还在 legacy 模式），说明 GIC 根本没按 GICv3 配起来，
	 *     这时候只补 ARE_S 一位（不碰 G0/G1S/G1NS 使能位、不碰 SPI 属性），
	 *     并明确打一行 ERROR 让现场知道"这里不该发生"。
	 * -------------------------------------------------------------------- */
	ctlr = apu_gicd_ctlr_read();
	if ((ctlr & CTLR_ARE_S_BIT) == 0U) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][gic] GICD_CTLR=0x%08X has ARE_S=0 (legacy mode). "
		       "Setting ARE_S only - this is a fallback, please check who "
		       "initialised the Distributor.",
		       (unsigned int)ctlr);
		AL_REG32_WRITE(AL_DR1M90_GICD_BASE, ctlr | CTLR_ARE_S_BIT);
	}

	/* ---- ③ 等 apu0 把本核唤醒（带超时，绝不裸等） ---- */
	{
		int ret = wait_rdist_awake(gicr_base);

		if (ret != APU_GIC_OK) {
			AL_LOG(AL_LOG_LEVEL_ERROR,
			       "[APU1][gic] GICR_WAKER@0x%lX = 0x%08X: ChildrenAsleep still set "
			       "after %u us. apu0(Linux) has not enabled this core's redistributor. "
			       "Continuing WITHOUT GIC - SGI notifications will not arrive.",
			       (unsigned long)gicr_base,
			       (unsigned int)AL_REG32_READ(gicr_base + GICR_WAKER),
			       (unsigned int)APU_GIC_WAKE_TIMEOUT_US);
			return APU_GIC_ERR_MASTER;
		}
	}

	/* ---- ④ 与 SDK 的 SLAVE 分支保持一致：别去抢属于 Linux 的 SPI ---- */
	(void)AlGicv3_DisableOwnSpiInterrupt();

	/* ---- ⑤ 配本核 Redistributor（SGI/PPI 默认属性） ---- */
	(void)AlGicv3_RdistInit(core);

	/* ---- ⑥ 使能本核 CPU 接口（这一步做完才真正能收中断） ---- */
	(void)AlGicv3_CpuIfEnable(core);

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][gic] core%u up: GICD@0x%lX ctlr=0x%08X GICR@0x%lX WAKER=0x%08X",
	       (unsigned int)core, (unsigned long)AL_DR1M90_GICD_BASE,
	       (unsigned int)apu_gicd_ctlr_read(),
	       (unsigned long)gicr_base,
	       (unsigned int)AL_REG32_READ(gicr_base + GICR_WAKER));

	return APU_GIC_OK;
}

void apu_gic_dump(void)
{
	AL_U32 core = AlGic_CorePos();
	AL_UINTPTR gicr_base = 0U;
	AL_U32 ctlr;

	if (core < (AL_U32)AL_ARM_CORE_COUNT) {
		gicr_base = GicData.RdistBaseAddrs[core];
	}
	ctlr = apu_gicd_ctlr_read();

	AL_LOG(AL_LOG_LEVEL_INFO, "----- GIC 状态（SGI 不通时先看这一段）-----");
	AL_LOG(AL_LOG_LEVEL_INFO, "本核 CorePos        : %u", (unsigned int)core);
	AL_LOG(AL_LOG_LEVEL_INFO, "GICD_CTLR           : 0x%08X (ARE_S=%u ARE_NS=%u G1S=%u G1NS=%u)",
	       (unsigned int)ctlr,
	       (unsigned int)((ctlr >> CTLR_ARE_S_SHIFT) & 1U),
	       (unsigned int)((ctlr >> CTLR_ARE_NS_SHIFT) & 1U),
	       (unsigned int)((ctlr >> CTLR_ENABLE_G1S_SHIFT) & 1U),
	       (unsigned int)((ctlr >> CTLR_ENABLE_G1NS_SHIFT) & 1U));
	AL_LOG(AL_LOG_LEVEL_INFO, "GICR_BASE(探测结果) : core0=0x%lX core1=0x%lX",
	       (unsigned long)GicData.RdistBaseAddrs[0],
	       (unsigned long)GicData.RdistBaseAddrs[1]);
	AL_LOG(AL_LOG_LEVEL_INFO, "本核 GICR_WAKER     : 0x%08X (PS=%u CA=%u  期望 CA=0)",
	       (unsigned int)AL_REG32_READ(gicr_base + GICR_WAKER),
	       (unsigned int)((AL_REG32_READ(gicr_base + GICR_WAKER) >> 1) & 1U),
	       (unsigned int)((AL_REG32_READ(gicr_base + GICR_WAKER) >> 2) & 1U));
	AL_LOG(AL_LOG_LEVEL_INFO,
	       "提示: SGI 方向若反了，改 apu1_openamp_config.h 的 APU1_SGI_SWAP 重编");
	AL_LOG(AL_LOG_LEVEL_INFO, "------------------------------------------");
}
