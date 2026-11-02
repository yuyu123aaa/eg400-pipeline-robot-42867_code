/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_wdt.h —— APU1 裸机侧硬件看门狗
 * ===========================================================================
 *  【为什么必须有它】
 *  交接文档里有句话：**"唯一的硬兜底是裸机自己的看门狗"**。
 *  Linux 侧的心跳线程丢包只打印、不动作；所以万一裸机挂死，
 *  能把它拉回来的只有硬件看门狗。
 *
 *  【SDK 的三个坑（实读源码确证）】
 *
 *   ① `ResponseMode` 默认是 `WDT_INTR_MODE`（**只中断不复位**）
 *      -> 想让 CMD:WDT_TEST 真的复位芯片，**必须显式选 `WDT_RESET_MODE`**
 *
 *   ② 超时**只有 16 档枚举**（64K..2G 个 WDT 时钟），没有"任意毫秒"：
 *      按 WDT_CLOCK = 16665000 换算：
 *        3.93ms / 7.87ms / 15.7ms / 31.5ms / 62.9ms / 126ms / 252ms / 504ms
 *        1.01s / 2.01s / **4.03s** / 8.05s / 16.1s / 32.2s / 64.4s / 128.8s
 *      -> 参考项目要 5s，**4.03s 最接近**，就用它
 *
 *   ③ `AlWdt_Hal_Init` 会写 **PMU 安全域寄存器**（al_wdt_hal.c:131-133）
 *      -> **APU1 非安全世界能否写，必须实测**
 *         若返回错误，本模块会打印告警并进入"软看门狗"降级模式
 *
 *  【WDT 不需要 FD/平台配置】
 *      `al_wdt_cfg.c` 是**无条件编译**的（没有 #ifdef WDTPS_ENABLE 包着），
 *      所以不用勾 WDTPS_ENABLE 也能用。
 * ===========================================================================
 */

#ifndef APU_WDT_H
#define APU_WDT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 建议超时档：64M 个 WDT 时钟 ≈ 4.03s（最接近参考项目要的 5s） */
#define APU_WDT_TIMEOUT_MS_APPROX   4030U

/* 初始化并启动看门狗。
 * 返回：0 成功；<0 失败（会打印原因，并降级为"软看门狗"）。 */
int apu_wdt_init(void);

/* 喂狗（每轮主循环调一次）。
 * ⚠️ 如果 CMD:WDT_TEST 已经 arm，本函数**故意什么都不做** —— 让狗咬。
 * ⚠️ 硬件狗没起来时本函数**只更新软看门狗时间戳**，不会崩，可放心调用。 */
void apu_wdt_feed(void);

/* ★ 2026-09-25 新增：喂狗安全的延时。
 *
 *  【解决什么问题】
 *    硬件狗超时 = APU_WDT_TIMEOUT_MS_APPROX (4030ms)。
 *    任何**单次** ≥ 4s 的阻塞，都会让狗咬、整板复位。
 *    典型受害者是"长延时"：PCA9685 的 ESC 解锁 3000ms 就在 4.03s 的红线上，
 *    只要再叠加前面几笔 I2C 通信耗时（现场首次通信会偏慢），就擦边复位。
 *
 *  【做法】把一次长延时切成若干片，**每片之前喂一次狗**，
 *    这样无论 ms 多大，喂狗间隔都不超过 APU_WDT_TIMEOUT_MS_APPROX/4 ≈ 1s。
 *
 *  【边界】ms = 0 时只喂一次狗、不延时。
 *    ms 必须 >= 0；传负值属于编码错误（会因无符号回绕而变成超大延时）。
 *
 *  【与 apu_wdt_feed() 的关系】本函数内部就是反复调 apu_wdt_feed()。
 *    所以 CMD:WDT_TEST arm 自检之后，本函数里的 feed 同样会被跳过 ——
 *    自检语义不变（仍然会咬）。 */
void apu_wdt_delay(uint32_t ms);

/* CMD:WDT_TEST 用：arm 之后停止喂狗，等狗复位芯片 */
void apu_wdt_arm_selftest(void);
int  apu_wdt_selftest_armed(void);

/* 看门狗是否真的起来了 */
int apu_wdt_is_active(void);

/* 如果硬件看门狗没起来，用软件超时兜底：
 * 返回 1 = 软件判定的"看门狗应当咬"（主循环卡死检测）。
 * 这个函数由业务层在无法喂狗时用来判定，正常情况永远返回 0。 */
int apu_wdt_soft_bark_needed(void);

/* 统计 */
uint32_t apu_wdt_feed_count(void);

/* ===========================================================================
 * ★ 2026-09-21 新增：PMU 侧回读 + WDT 寄存器 dump
 * ===========================================================================
 *  【为什么必须加】
 *  SDK 的 `AlWdt_Hal_Init()` 里，把 "WDT0 超时" 接到 PMU "复位源" 上的三个调用是：
 *        AlPmu_SetHwEn(AL_PMU_ERR_WDT0_OT);       // al_wdt_hal.c:131  ← ★ 决定"超时会不会复位芯片"
 *        AlPmu_SetIntEn(AL_PMU_ERR_WDT0_OT);      // :132
 *        AlPmu_Shack(AL_PMU_SHACK_HDRST_CNT_CLR); // :133
 *  它们全是 `static inline AL_VOID`（al_pmu_ll.h:49/67）—— **返回 void，
 *  也不计入 AlWdt_Hal_Init() 的返回值**（那个返回值只来自最后的 AlIntr_RegHandler）。
 *
 *  ⇒ 所以 `hw_active=1` **只证明"寄存器写下去了、中断注册成功了"**，
 *    **完全不证明"狗超时能把芯片复位"**。
 *    2026-09-21 上板实测就撞到了：CMD:WDT_TEST 之后 feeds 冻结、LED 灭火，
 *    但芯片**没有复位** —— 典型的"PMU 那一步静默失败"。
 *
 *  `al_pmu_ll.h` 里有对应的**回读**函数，正好用来验证。
 *
 *  📌 判读（发完 CMD:WDT_TEST 等 5 秒再读）：
 *        raw_his = 1 -> 狗**确实超时过**（计数在走）⇒ 是 PMU 没放行复位
 *        raw_his = 0 -> 狗**压根没超时** ⇒ WDT 计数器没在跑
 * =========================================================================== */

typedef struct {
	int hw_en;       /* 1 = PMU 已允许 "WDT0 超时 -> 复位芯片"（0 = 超时也不复位！）*/
	int int_en;      /* 1 = PMU 中断使能 */
	int int_state;   /* PMU 中断状态（不清）*/
	int raw_his;     /* PMU 原始历史位（**读清除**）*/
} apu_wdt_pmu_t;

/* WDT 寄存器块是否配置成功（原来的 hw_active 语义）*/
int apu_wdt_block_up(void);

/* ★ 超时是否**真的会**复位芯片（PMU 回读 hw_en）—— 这才是"硬兜底是否有效" */
int apu_wdt_can_reset(void);

/* 读 PMU 侧状态 */
void apu_wdt_pmu_read(apu_wdt_pmu_t *out);

/* dump WDT + PMU 寄存器块（CCVR 连读两次判断计数器在不在走）。
 * 由控制台 `@wdt:regs` 调用。 */
void apu_wdt_dump_regs(void);

/* ===========================================================================
 * ★ 2026-09-21 新增：RST_REASON —— **上次是什么把芯片复位的**
 * ===========================================================================
 *  寄存器：RST_REASON @ 0xF88014B0（CRP 块），类型 **w1c**
 *  官方寄存器手册就在 FD 安装目录里：
 *      C:\Anlogic\FD_2025.1_SP1\data\addrmap\dr1m90\registers\top_cfg_apu.html
 *
 *  ⭐ 它**跨复位保留**（这正是它的用途）—— 复位后固件重新跑起来就能读到
 *     "上一次的复位原因"。这是**硬件层面的铁证**：
 *        · 不用 BOOT.bin
 *        · 不用调试器
 *        · 不用往 DDR 塞魔数
 *     判据：bit6 `rreason_swdt0_rst` = 1 ⇒ 上一次就是看门狗把芯片复位了。
 *
 *  ⚠️ 我们**读后清除**（w1c 寄存器的标准用法），这样每次开机报的是
 *     "紧邻的这一次"复位；清掉的值会存下来，用 @wdt 还能看。
 * =========================================================================== */
typedef struct {
	uint32_t raw;
	int srst_b;      /* bit0 PS_SRST_N 引脚复位（复位键/外部拉低）*/
	int srst_soft;   /* bit1 软件全局复位 */
	int jtag_bsu;    /* bit4 JTAG BSU 复位（下载/调试会话）*/
	int pmu_err;     /* bit5 PMU 错误触发的复位 */
	int swdt0;       /* bit6 ★ SWDT0 复位 —— 我们的看门狗 */
	int swdt1;       /* bit7 SWDT1 */
	int swdt2;       /* bit8 SWDT2 */
} apu_wdt_rst_reason_t;

/* 读 RST_REASON（**读后自动清除**，因为它是 w1c）。 */
void apu_wdt_rst_reason_read(apu_wdt_rst_reason_t *out);

/* 本次开机时读到的 RST_REASON（init 里存下来的那份）。
 * 返回 1 = 有效；0 = init 还没读过。 */
int apu_wdt_last_rst_reason(apu_wdt_rst_reason_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APU_WDT_H */
