/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_gic.h —— APU1 裸机侧 GICv3 "本核" 拉起（SLAVE 语义）
 * ===========================================================================
 *  【为什么需要这个文件】
 *
 *  SDK 里负责"把本核 GIC 弄起来"的函数是 al_gic.c 的 AlGic_Init()，但它
 *  **在本工程当前构建下从来没被执行过**：
 *
 *    al_aarch64_start.S:201-205
 *        #ifdef SUPPORT_NONSECURE
 *            bl clear_bss
 *            bl AlGic_Init          <-- 全 SDK 唯一的调用点
 *            bl AlSys_StartTimer
 *        #endif
 *
 *    dr1m90_fpsoc_cpu1/Makefile:85-89
 *        ifeq ($(ARMv8_EL), EL1)
 *          ifeq ($(ARMv8_SECURE), NONSECURE)
 *            CFLAGS += -DSUPPORT_NONSECURE
 *
 *    而 soc_base/tools/make/config.mk:23-24 给的是 SDK 默认值
 *        ARMv8_EL     ?= EL3
 *        ARMv8_SECURE ?= SECURE
 *    本工程没有覆盖它们 ⇒ SUPPORT_NONSECURE 未定义 ⇒ AlGic_Init() 不执行。
 *
 *    链接产物可自证：build/my_work.map 的交叉引用表里
 *    **没有任何一条 (AlGic_Init)** —— 符号在（--whole-archive 带进来的），
 *    但没人调。
 *
 *  ⇒ 后果：AlGicv3_RdistInit()（配本核 Redistributor）与
 *          AlGicv3_CpuIfEnable()（使能本核 CPU 接口）**都没跑**。
 *          SGI 14/15 这条 RPMsg 通知链因此不可靠。
 *
 *  【本文件做什么】
 *    只做 AlGic_Init() 里 **SLAVE 分支**该做的事，一个不多：
 *      ① AlGicv3_DriverInit()            —— 探测两个 Redistributor 基址
 *      ② 等 apu0（Linux）把本核 Rdist 唤醒（GICR_WAKER.ChildrenAsleep=0）
 *         ★ 带超时，**不裸用 SDK 那个无超时死循环**
 *      ③ AlGicv3_DisableOwnSpiInterrupt() —— 与 SDK 的 SLAVE 分支一致
 *      ④ AlGicv3_RdistInit(本核)
 *      ⑤ AlGicv3_CpuIfEnable(本核)
 *
 *  【本文件绝不做什么】
 *    ✗ 不调用 AlGicv3_DistInit()
 *      它会关掉 GICD_CTLR 的 G0/G1S/G1NS 使能位、重写 ARE 位、并把全部 SPI
 *      恢复默认属性（al_gicv3_main.c:59-90）。那个 Distributor 属于 Linux。
 *    ✗ 不写 GICD_CTLR
 *      Distributor 归 Linux 管；我们只在 GICD_CTLR.ARE_S 明显没置起来时补一下
 *      （见实现里的说明），其余一概不动。
 * ===========================================================================
 */

#ifndef APU_GIC_H
#define APU_GIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回值 */
#define APU_GIC_OK              0
#define APU_GIC_ERR_ARG        (-1)   /* 参数/驱动数据不合法 */
#define APU_GIC_ERR_MASTER     (-2)   /* 等 apu0 唤醒超时（ChildrenAsleep 一直为 1）*/

/*
 * apu_gic_up() —— 把 APU1 本核的 GIC 通路拉起来。幂等，可重复调用。
 *
 * 必须在"要用中断之前"调用，建议位置：main.c 的 openamp_sample() 最开头，
 * 早于 platform_init()（后者会注册 SGI 处理函数并开 IRQ）。
 *
 * 返回 APU_GIC_OK(0) 表示本核 CPU 接口已使能；
 * 返回负数表示没弄成，**不会卡住**，调用者可以继续跑（只是收不到中断）。
 */
int apu_gic_up(void);

/* 自检打印：把 GICD/GICR 的关键寄存器打一遍，排查 SGI 方向时看这个 */
void apu_gic_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_GIC_H */
