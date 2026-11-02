/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_time.c —— APU1 裸机侧的统一时间基（实现）
 * ===========================================================================
 *  硬件来源：ARM 通用计时器计数寄存器 CNTPCT_EL0
 *            安路 SDK 把它包成 AlSys_GetTimerTickCount() / AlSys_GetTimerFreq()
 *
 *  换算：ms   = ticks * 1000 / freq
 *        tick_10ms 不自己算，而是**【只在 ms 跨过 10ms 边界时 +1】**
 *        —— 这样 tick_10ms 严格跟随 ms，不会因为两次取整各自累积误差而漂移。
 *
 *  ⚠️ 本文件是【全工程唯一】允许调用 AlSys_GetTimerTickCount() 的地方。
 * ===========================================================================
 */

#include "apu_time.h"

#include "al_core.h"          /* 已经 include 了 al_systimer.h 和 al_reg_io.h */
#include "al_systimer.h"

/* ---------------------------------------------------------------------------
 * 模块内部状态
 * --------------------------------------------------------------------------- */
static AL_U64   g_freq;             /* 计数器频率（Hz），0 = 时钟不可用 */
static uint64_t g_last_ticks;       /* 上一次换算用的原始计数 */
static uint32_t g_base_ms;          /* 上一次换算得到的毫秒值 */
static uint32_t g_ms;               /* 毫秒单调递增（我们自己维护，保证单调） */
static uint32_t g_tick_10ms;        /* 10ms tick */
static uint32_t g_last_tick_ms;  /* 上次推进 tick 时的毫秒值（用来保住不足 10ms 的余数） */
static int      g_time_ok;          /* 1 = 硬件计数器可用 */

/* 1ms 自检计数器：给"跑 10 分钟比对秒表"那条验收用（P9 会关掉打印）。
 * P1 阶段只在 apu_app.c 里用 apu_time_ms() 打印，所以这里先不占变量。 */

/* ---------------------------------------------------------------------------
 * 初始化
 * --------------------------------------------------------------------------- */
int apu_time_init(void)
{
	g_freq = AlSys_GetTimerFreq();

	if (g_freq == 0U) {
		/* 时钟没起来：退回软件计时，精度会差，但不会崩 */
		g_time_ok = 0;
		g_ms = 0U;
		g_tick_10ms = 0U;
		g_last_ticks = 0U;
		g_base_ms = 0U;
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][time] AlSys_GetTimerFreq() returned 0 - "
		       "falling back to software timing (imprecise)");
		return -1;
	}

	g_time_ok = 1;
	g_last_ticks = AlSys_GetTimerTickCount();
	/* 用硬件计数器当前值当作零点，后面只做差值 */
	g_base_ms = (uint32_t)(g_last_ticks * 1000ULL / g_freq);
	g_ms = 0U;
	g_tick_10ms = 0U;
	g_last_tick_ms = 0U;

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][time] counter freq = %u Hz (%u MHz), timebase ready",
	       (unsigned int)g_freq, (unsigned int)(g_freq / 1000000ULL));
	return 0;
}

/* ---------------------------------------------------------------------------
 * 周期刷新（每轮主循环调一次）
 * ---------------------------------------------------------------------------
 *  只在"毫秒值真的前进了"时才更新 g_ms / g_tick_10ms，
 *  这样即使主循环一轮跑得比 1ms 还快，也不会出现 tick 跳变或倒退。
 * --------------------------------------------------------------------------- */
void apu_time_poll(void)
{
	uint32_t now_ms;
	uint32_t elapsed;

	if (!g_time_ok) {
		/* 硬件计数器不可用：至少让 ms/tick 每轮 +1，保证"时间在走"，
		 * 不会因为恒为 0 让心跳超时判断永远不触发（旧版本就踩过这个坑）。 */
		g_ms++;
		if ((g_ms % 10U) == 0U)
			g_tick_10ms++;
		return;
	}

	now_ms = (uint32_t)(AlSys_GetTimerTickCount() * 1000ULL / g_freq) - g_base_ms;

	if ((uint32_t)(now_ms - g_ms) == 0U)
		return;                 /* 还没到下一个毫秒，什么都不做 */

	g_ms = now_ms;

	/* ★★ 10ms tick 的正确算法（2026-09-18 晚修正）★★
	 * ---------------------------------------------------------------------------
	 *  旧写法是错的：
	 *      new_ticks = delta_ms / 10U;                    // ← 余数 delta_ms % 10 被丢掉
	 *      if (new_ticks != 0U) g_tick_10ms += new_ticks;
	 *  只要主循环一轮只走 1~9ms，delta_ms/10 就是 0 → **这一段计数永久丢失、补不回来**。
	 *
	 *  上板实测（中断修好后主循环变快，问题被放大）：
	 *      ms=10305 时 tick_10ms 只有 180（应约 1030）；
	 *      ms 从 10305 走到 14745（4.4 秒），tick 只 +1；偏差越拉越大。
	 *
	 *  后果很严重：tick_10ms 是**心跳门的计时单位**
	 *      （APU_HB_TIMEOUT_TICKS=250 号称 2.5s、启动宽限 750 号称 7.5s）
	 *      tick 走不动 → 门限被无限拉长 → **心跳保护实际上永远不会触发**。
	 *
	 *  正确做法：把"上次推进 tick 时的毫秒值"单独记在 g_last_tick_ms，
	 *  只推进整数个 10ms，**余数留在里面**给下一轮继续用。
	 *  这样 tick 严格等于 ms/10，既不丢也不漂。
	 * --------------------------------------------------------------------------- */
	elapsed = (uint32_t)(g_ms - g_last_tick_ms);
	if (elapsed >= 10U) {
		uint32_t n = elapsed / 10U;

		g_last_tick_ms += n * 10U;      /* ★ 只推进整数倍，余数保留 */
		g_tick_10ms += n;
	}
}

/* ---------------------------------------------------------------------------
 * 取时间
 * --------------------------------------------------------------------------- */
uint32_t apu_time_ms(void)
{
	return g_ms;
}

uint32_t apu_tick_10ms(void)
{
	return g_tick_10ms;
}

/* ---------------------------------------------------------------------------
 * ★★ 2026-09-25 新增：实时毫秒（见 apu_time.h 的详细说明）★★
 * ---------------------------------------------------------------------------
 *  g_ms 是**缓存**，只有 apu_time_poll() 会刷新它；而 apu_time_poll() 只在
 *  主循环里调。所以"主循环之外"的代码读 apu_time_ms() 会看到时间冻住 ——
 *  上板后果：platform_poll() 的"等够 3 秒就放弃"永不成立 → 永久卡死，
 *  但看门狗一直被喂 → 板子不复位、串口也一行不打（活锁，极难查）。
 *
 *  本函数直接读硬件计数器，**不依赖任何缓存**，主循环之外也能正确前进。
 *  换算方式与 apu_time_poll() 完全一致（同一个 g_base_ms 零点），
 *  所以两者单调一致、可以安全混用。
 *
 *  ⚠️ 沿用文件头的约定：全工程只有本文件允许碰 AlSys_GetTimerTickCount()，
 *     所以"实时读"这件事必须由本模块提供，不允许调用方自己读。
 * --------------------------------------------------------------------------- */
uint32_t apu_time_ms_live(void)
{
	if (!g_time_ok) {
		/* 硬件计数器不可用：只能退回缓存值。
		 * 此时代码本来就跑得不准，但至少不会因为读不到而卡死。 */
		return g_ms;
	}

	return (uint32_t)(AlSys_GetTimerTickCount() * 1000ULL / g_freq) - g_base_ms;
}

/* ===========================================================================
 * 说明：本工程当前【没有】任何地方使用 TC0 定时器。
 *
 * 如果将来实测发现主循环周期抖动太大（比如某个外设轮询阻塞 >10ms），
 * 可以按下面方式加 1ms 中断，接口不用改：
 *      AlTc_Hal_Init(&tc, 0, &tcfg, cb);
 *      AlTc_Hal_EnableIntervalMode(tc);
 *      AlTc_Hal_SetIntervalMaxVal(tc, 62494);   // PrescaleVal=4 -> 1.0000ms
 *      AlTc_Hal_EnableIntr(tc, AL_TC_IntrInterval, AL_TRUE);
 *      AlTc_Hal_EnableCounter(tc, AL_TRUE);      // ★ Init 里计数器是停的
 *  在 cb 里调 apu_time_poll() 即可。
 * =========================================================================== */
