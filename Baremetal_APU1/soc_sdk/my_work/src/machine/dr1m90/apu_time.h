/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_time.h —— APU1 裸机侧的统一时间基
 * ===========================================================================
 *  【为什么必须有这一层】
 *
 *  传承项目（飞腾平台）的所有时间常量都是基于 **10ms tick** 的：
 *      HOST_HEARTBEAT_TIMEOUT_TICKS          250    (= 2.5 s)
 *      HOST_HEARTBEAT_STARTUP_TIMEOUT_TICKS  750    (= 7.5 s)
 *      NAV_IMU_TIMEOUT_TICKS                 100    (= 1.0 s)
 *      NAV_SONAR_TIMEOUT_TICKS               100    (= 1.0 s)
 *      PIPE_CENTER_CONTROL_PERIOD_TICKS       10    (= 100ms = 10Hz)
 *
 *  安路 SDK 只给"硬件计数器 tick"，单位是 1/25MHz 之类的碎数。
 *  如果每个模块各自换算，**迟早会有人把 ms 当 tick 用** → 刹车时间差 10 倍。
 *
 *  所以：**全工程只通过本模块取时间**，其它任何地方都不许直接调
 *        AlSys_GetTimerTickCount() / AlSys_GetTimerFreq()。
 *
 *  【本模块对外只给两种单位，别自创第三种】
 *      apu_time_ms()        毫秒单调递增（心跳超时、打印用）
 *      apu_tick_10ms()      10ms tick，**与传承项目单位完全一致**
 *                           → 移植过来的常量【一个都不用改】
 * ===========================================================================
 */

#ifndef APU_TIME_H
#define APU_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 一、初始化 / 周期调用
 * =========================================================================== */

/* 初始化时间基。必须在任何 apu_time_* / apu_tick_10ms 之前调用一次。
 *
 * 位置：main.c 的 openamp_sample() 里，platform_init() 之后、app() 之前。
 * 返回：0 成功；-1 说明硬件计数器读不出来（freq==0），此时退回软件计时，
 *       时间精度会变差，串口会打印告警。 */
int apu_time_init(void);

/* 每轮主循环调一次。
 *
 * ★ 为什么不用 TC 定时器中断：
 *   本芯片 TC 是 16 位计数器 + 最大 16 分频，TC_CLOCK = 249.975MHz，
 *   最长单次周期 = 16×65535/249.975e6 = 4.196ms —— 【单个 TC 做不出 10ms】。
 *   而主循环周期只有 ~20µs（接上 UART0/I2C1 后 ~230µs），
 *   每轮 poll 一次足够保证 10ms 分辨率，还省掉一整套 TC 中断接线。
 *
 * ★ 将来若要提高精度：让 TC0 跑 1ms 中断，在中断里调 apu_time_poll() 即可，
 *   本模块的对外接口不用改。 */
void apu_time_poll(void);

/* ===========================================================================
 * 二、取时间
 * =========================================================================== */

/* 毫秒单调递增（uint32 回绕，约 49.7 天一次）
 * ⚠️ 只能用来做【差】：(uint32_t)(now - then)，这样回绕也是对的。 */
uint32_t apu_time_ms(void);

/* 10ms tick 单调递增（uint32 回绕，约 497 天一次）
 * ★ 与传承项目 system_tick_10ms 单位一致，可直接套用原项目常量。 */
uint32_t apu_tick_10ms(void);

/* ★★ 2026-09-25 新增：**实时**毫秒（直接读硬件计数器，不依赖 apu_time_poll）★★
 *
 *  【为什么必须有这个接口 —— 上板踩过的大坑】
 *    apu_time_ms() 返回的是软件缓存的 g_ms，而 g_ms **只在 apu_time_poll()
 *    里刷新**（apu_time.c 的 apu_time_poll），apu_time_poll() 又只在**主循环**
 *    里被调用（main.c：排在 platform_poll() 后面）。
 *
 *    所以：**任何在"主循环之外"跑、且需要计时的代码，用 apu_time_ms() 都会
 *    看到时间冻在 0 不动。** 一旦这个时间的用途是"等够 N 毫秒就放弃"，
 *    那个放弃条件就永远不成立 → **永久卡死**（活锁）。而上板表现极具迷惑性：
 *    看门狗一直被喂（所以不复位）、串口一行不打（所以像死机）。
 *
 *    上板实证 putty_s1_14400.log（2026-09-25 20:18）：
 *      最后一行是 main.c 的 `[APU1][loop] entering main loop (t=0 ms)`，
 *      之后**连一行 `[APU1][poll] waiting for APU0 kick` 都没有** ——
 *      因为 platform_poll() 里的 now 恒为 0，诊断和超时两个判断同时失效。
 *
 *  【什么时候该用它】
 *      在**主循环之外**、或**主循环之前**的任何计时（初始化里的延时、
 *      等待类函数内部的超时），一律用它。
 *      主循环内部（能保证每轮都跑 apu_time_poll()）继续用 apu_time_ms() 即可。
 *
 *  ⚠️ 它和 apu_time_ms() 同源（同一个硬件计数器、同一个 g_base_ms 零点），
 *     所以两者**单调一致**，混用只会在"从没 poll 过"的期间有个阶梯式跳变
 *     （live 的值 >= 缓存的值），做差 `(uint32_t)(now - then)` 永远安全。 */
uint32_t apu_time_ms_live(void);

/* ===========================================================================
 * 三、基于 tick 的便捷判断（避免各模块重复写减法）
 * =========================================================================== */

/* 从 base_tick 到现在是否已经过去了 ticks 个 10ms。
 * 用法：if (apu_tick_elapsed(last_tick, 250U)) { 已经 2.5 秒没动静了 } */
static inline int apu_tick_elapsed(uint32_t base_tick, uint32_t ticks)
{
	return (uint32_t)(apu_tick_10ms() - base_tick) >= ticks;
}

/* 取"现在"这个 tick（等价于 apu_tick_10ms()，纯为可读性存在） */
static inline uint32_t apu_tick_now(void)
{
	return apu_tick_10ms();
}

#ifdef __cplusplus
}
#endif

#endif /* APU_TIME_H */
