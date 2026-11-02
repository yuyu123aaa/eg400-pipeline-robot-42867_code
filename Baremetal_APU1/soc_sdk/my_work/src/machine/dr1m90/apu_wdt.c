/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_wdt.c —— APU1 裸机侧硬件看门狗实现
 * ===========================================================================
 *  ⚠️ 本文件用了两个 SDK 里取值很关键的枚举，注释里写清为什么：
 *
 *      ResponseMode   = WDT_RESET_MODE               (0x0)  ← 不是默认值！
 *                       默认 WDT_INTR_MODE (0x1) 只中断不复位
 *      TimeOutValue   = WDT_TIMEOUT_PERIOD_64M_CLOCKS (0xa)  ← ≈4.03s
 * ===========================================================================
 */

#include "apu_wdt.h"

#include "al_core.h"
#include "al_wdt_hal.h"
/* ★ 2026-09-21：PMU 侧回读（判断"超时到底会不会复位芯片"）。
 *   al_pmu_ll.h 自己会 include al_pmu_hw.h，不用额外加。
 *   WDT_TOP_CFG_CTRL_ADDR 由 al_wdt_ll.h -> al_wdt_hw.h 链路带进来，直接用即可。 */
#include "al_pmu_ll.h"

#include "apu_time.h"

/* WDT 实例 0（WDT0 @0xF8410000）。参考项目用的是 WDT1（留给从核），
 * 我们这里用 WDT0 —— Linux 侧会把它让出来。若实测冲突，改成 1 即可。 */
#define APU_WDT_DEV_ID          0U

/* ---------------------------------------------------------------------------
 * ★ 2026-09-21 新增：RST_REASON / WDT_OUT_CTRL（CRP 块）
 * ---------------------------------------------------------------------------
 *  来源：安路官方寄存器手册（FD 安装目录自带）
 *      C:\Anlogic\FD_2025.1_SP1\data\addrmap\dr1m90\registers\top_cfg_apu.html
 *
 *  RST_REASON  @ 0xF88014B0 = TOP_CRP_BASE_ADDR(0xF8801000) + 0x4B0，类型 w1c
 *      bit0 srst_b / bit1 srst_soft / bit4 jtag_BSU / bit5 pmu_err
 *      bit6 swdt0_rst ★ / bit7 swdt1_rst / bit8 swdt2_rst
 *
 *  WDT_OUT_CTRL @ 0xF88014B4，rw，默认 0
 *      bit0/1/2 wdt0/1/2_rst_outen：1 = 把 wdtN_rst_n 引到 PS_IO/PL_IO（外部复位）
 * --------------------------------------------------------------------------- */
#define APU_WDT_RST_REASON_ADDR   (0xF88014B0UL)
#define APU_WDT_OUT_CTRL_ADDR     (0xF88014B4UL)

static AL_WDT_HalStruct *g_wdt;
static int      g_wdt_active;       /* WDT 寄存器块配置成功（Enable/超时/模式都写下去了）*/
static int      g_wdt_can_reset;    /* ★ PMU 回读：超时**真的会**复位芯片 */
static int      g_selftest_armed;   /* CMD:WDT_TEST 已 arm -> 停止喂狗 */
static uint32_t g_feed_count;

/* ★ 2026-09-21：RST_REASON（上次复位原因）的状态变量。
 * ⚠️ 必须声明在这里（文件顶部）—— apu_wdt_init() 比实现那里的函数**先用**它们。 */
static apu_wdt_rst_reason_t g_last_rst_reason;
static int g_rst_reason_valid;

/* 软看门狗：硬件狗没起来时的兜底（记录最后一次喂狗时间） */
static uint32_t g_soft_last_feed_tick;

int apu_wdt_init(void)
{
	AL_WDT_InitStruct cfg = {
		.ResetPuaseLength = (AL_U32)WDT_RPL_PCLK_CYCLES_8,
		/* ★★ 必须显式选 RESET_MODE，否则超时只中断、不复位芯片，
		 *    CMD:WDT_TEST 看起来就是"没反应"（这是已知陷阱之一） */
		.ResponseMode     = (AL_U32)WDT_RESET_MODE,
		/* ★ 只有 16 档枚举，4.03s 最接近参考项目要的 5s */
		.TimeOutValue     = (AL_U32)WDT_TIMEOUT_PERIOD_64M_CLOCKS,
	};
	AL_S32 ret;

	g_wdt = AL_NULL;
	/* ★★ 先读 RST_REASON：**上一次到底是什么把芯片复位的** ★★
	 *  这个寄存器跨复位保留，所以这是"看门狗到底咬没咬"的硬件铁证。
	 *  读后自动清除（w1c），保证每次开机报的是紧邻的那一次。 */
	apu_wdt_rst_reason_read(&g_last_rst_reason);
	g_rst_reason_valid = 1;

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][wdt] RST_REASON=0x%08X  SWDT0=%d SWDT1=%d SWDT2=%d "
	       "PMU_ERR=%d JTAG=%d SRST_soft=%d SRST_pin=%d",
	       (unsigned int)g_last_rst_reason.raw,
	       g_last_rst_reason.swdt0, g_last_rst_reason.swdt1,
	       g_last_rst_reason.swdt2, g_last_rst_reason.pmu_err,
	       g_last_rst_reason.jtag_bsu, g_last_rst_reason.srst_soft,
	       g_last_rst_reason.srst_b);
	
	if (g_last_rst_reason.swdt0 || g_last_rst_reason.swdt1 ||
	    g_last_rst_reason.swdt2) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][wdt] ★★ 上一次复位【是看门狗干的】-> 硬兜底确实咬过！");
	} else if (g_last_rst_reason.pmu_err) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][wdt] ★★ 上一次复位由 PMU 错误触发（可能就是 WDT 超时走硬件模式）。");
	} else if (g_last_rst_reason.raw == 0U) {
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][wdt] RST_REASON 读到 0 —— 可能是冷启动/上电复位，"
		       "也可能被 FSBL 提前读清过了（w1c）。");
	} else {
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][wdt] 上一次复位不是看门狗：JTAG/软件/复位引脚，属正常。");
	}

	g_wdt_active = 0;
	g_selftest_armed = 0;
	g_feed_count = 0U;
	g_soft_last_feed_tick = apu_tick_now();

	/* ⚠️ AlWdt_Hal_Init 内部会写 PMU 安全域寄存器（al_wdt_hal.c:131-133），
	 *    APU1 处于非安全世界时**可能写不进去** —— 必须判返回值。 */
	ret = AlWdt_Hal_Init(&g_wdt, APU_WDT_DEV_ID, &cfg, AL_NULL);
	if (ret != AL_OK) {
		g_wdt = AL_NULL;
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][wdt] AlWdt_Hal_Init(DevId=%u) failed, ret=0x%x",
		       (unsigned int)APU_WDT_DEV_ID, (unsigned int)ret);
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][wdt] -> hardware watchdog UNAVAILABLE. "
		       "Likely cause: PMU secure-domain write rejected from non-secure world. "
		       "Falling back to software watchdog.");
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][wdt] NOTE: software watchdog CANNOT reset the chip - "
		       "it can only detect a stall. Real hard-reset protection needs the HW WDT.");
		return -1;
	}

	/* Init 内部已经 Enable + CounterRestart，这里再显式 Start 一次更保险 */
	ret = AlWdt_Hal_Start(g_wdt);
	if (ret != AL_OK) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][wdt] AlWdt_Hal_Start failed, ret=0x%x",
		       (unsigned int)ret);
		return -1;
	}

	/* ---- WDT 寄存器块起来了 ---- */
	g_wdt_active = 1;

	/* ★★ 关键：回读 PMU，确认"超时真的会复位芯片" ★★
	 *  上面这三个 AlPmu_*() 是 void 函数、静默失败，只能靠读回来验证。
	 *  读不到 hw_en=1 就说明：狗会超时，但**芯片不会复位**。 */
	{
		/* ⚠️⚠️ 2026-09-21 修正：这里**绝对不能**调 apu_wdt_pmu_read()！
		 *   它会读 AlPmu_GetRawHistoryState()，而 SDK 在那个函数上明确标了
		 *   **"Read clear"**（al_pmu_ll.h:115）—— **读一下就清除**，
		 *   是个带副作用的读。而且刚复位完 raw_his 本来就是 0，读它毫无信息量。
		 *   启动路径上做带副作用的读，风险不值得。
		 *   → 这里只读两个"纯状态"位；raw_his 留给用户手动敲 @wdt / @wdt:regs 时再看。 */
		int hw_en  = AlPmu_GetHwEn(AL_PMU_ERR_WDT0_OT) ? 1 : 0;
		int int_en = AlPmu_GetIntEn(AL_PMU_ERR_WDT0_OT) ? 1 : 0;

		g_wdt_can_reset = hw_en;

		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][wdt] hardware WDT block up: WDT%u, RESET mode, ~%u ms timeout",
		       (unsigned int)APU_WDT_DEV_ID, (unsigned int)APU_WDT_TIMEOUT_MS_APPROX);
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][wdt] PMU readback: hw_en=%d int_en=%d (raw_his 留到 @wdt:regs 再看)",
		       hw_en, int_en);

		if (!g_wdt_can_reset) {
			AL_LOG(AL_LOG_LEVEL_ERROR,
			       "[APU1][wdt] ★★ hw_en=0 -> PMU 没有允许 'WDT0 超时 -> 复位芯片'。"
			       "狗会超时，但【不会复位芯片】—— 硬兜底当前无效！"
			       "（最可能原因：APU1 非安全世界写不进 PMU 安全域）");
			AL_LOG(AL_LOG_LEVEL_ERROR,
			       "[APU1][wdt] -> apu_wdt_is_active() 会返回 0；请用 @wdt / @wdt:regs 复核");
		} else {
			AL_LOG(AL_LOG_LEVEL_INFO,
			       "[APU1][wdt] CMD:WDT_TEST will stop feeding -> chip should reset in ~4s");
		}
	}
	return 0;
}

void apu_wdt_feed(void)
{
	/* ★ arm 之后**故意不喂** —— 让狗咬，验证它真的活着 */
	if (g_selftest_armed)
		return;

	g_soft_last_feed_tick = apu_tick_now();

	if (g_wdt_active && g_wdt != AL_NULL) {
		(void)AlWdt_Hal_Feed(g_wdt);
		g_feed_count++;
	}
}

/* ---------------------------------------------------------------------------
 * ★ 2026-09-25 新增：喂狗安全的延时（见 apu_wdt.h 的说明）
 *
 *  把一次长延时切成片，每片之前喂一次狗。
 *  切片长度取超时的 1/4，留 4 倍余量。
 *
 *  ⚠️ 时间基准必须用 apu_time_ms_live()（**毫秒**，且**实时读硬件**）。
 *     两个坑都踩过，并列在这里：
 *
 *     坑 1（单位）：不要用 apu_tick_now() —— 那个是 10ms 单位
 *        （apu_time.h），按毫秒用会慢 10 倍，正好把狗喂成复位。
 *
 *     坑 2（时效，2026-09-25 修）：不要用 apu_time_ms()！
 *        它是**软件缓存**，只在 apu_time_poll() 里刷新，而 apu_time_poll()
 *        只在**主循环**里调。本函数唯一的调用点 apu_pca9685_init() 跑在
 *        apu_app_init() 里 —— **那时候主循环还没开始**，所以
 *        apu_time_ms() 会一直返回同一个值，`elapsed >= ms` 永不成立
 *        ⇒ 本函数永远不返回 ⇒ 整机卡死在初始化里（活锁）。
 *        （上板暂未暴露，只因为 PCA9685 当前不应答、走不到这行；
 *          一旦器件接上就会真实卡死。）
 *        改用 apu_time_ms_live()（详见 apu_time.h）即可。
 *
 *  写成 `(uint32_t)(now - start)` 的形式，32 位回绕也正确。
 * --------------------------------------------------------------------------- */
void apu_wdt_delay(uint32_t ms)
{
	/* 每片最多 1000ms（超时 4030ms 的 1/4，留足余量） */
	const uint32_t slice_ms = APU_WDT_TIMEOUT_MS_APPROX / 4U;
	uint32_t start;

	/* ms=0 时也喂一次：调用点把它当"顺手喂狗"用是常见写法 */
	apu_wdt_feed();
	if (ms == 0U)
		return;

	start = apu_time_ms_live();
	for (;;) {
		uint32_t elapsed = (uint32_t)(apu_time_ms_live() - start);
		uint32_t remain;

		if (elapsed >= ms)
			break;

		remain = ms - elapsed;
		AlSys_MDelay(remain < slice_ms ? remain : slice_ms);

		/* 下一片之前喂狗 —— 保证喂狗间隔 <= slice_ms */
		apu_wdt_feed();
	}
}

void apu_wdt_arm_selftest(void)
{
	g_selftest_armed = 1;
	AL_LOG(AL_LOG_LEVEL_ERROR,
	       "[APU1][wdt] WDT SELF-TEST ARMED: stop feeding the watchdog. "
	       "Expected: chip resets in ~4s and prints the startup banner again.");
}

int apu_wdt_selftest_armed(void)
{
	return g_selftest_armed;
}

int apu_wdt_is_active(void)
{
	/* ★ 2026-09-21：语义改成"**真的能兜底**才算 active"。
	 *  WDT 寄存器块起来了（g_wdt_active）**不代表**超时会复位芯片 ——
	 *  PMU 那一步是 void 函数、静默失败（详见 apu_wdt.h 的说明）。
	 *  所以要求两个都成立。想看细节用 @wdt / @wdt:regs。 */
	return (g_wdt_active && g_wdt_can_reset) ? 1 : 0;
}

int apu_wdt_soft_bark_needed(void)
{
	/* 硬件狗没起来时，软件只能"报告"，不能复位。
	 * 判据：超过 2 × 喂狗周期（按 4.03s 算）没有喂狗。
	 * 正常情况主循环会一直喂，所以永远返回 0。 */
	if (g_wdt_can_reset)
		return 0;
	if (!g_selftest_armed)
		return 0;

	return (uint32_t)(apu_tick_now() - g_soft_last_feed_tick) >=
	       (APU_WDT_TIMEOUT_MS_APPROX / 10U);
}

uint32_t apu_wdt_feed_count(void)
{
	return g_feed_count;
}

/* ★ 2026-09-21 新增（见 apu_wdt.h 的说明） */

int apu_wdt_block_up(void)
{
	return g_wdt_active;
}

int apu_wdt_can_reset(void)
{
	return g_wdt_can_reset;
}

void apu_wdt_pmu_read(apu_wdt_pmu_t *out)
{
	if (out == AL_NULL)
		return;

	out->hw_en     = AlPmu_GetHwEn(AL_PMU_ERR_WDT0_OT) ? 1 : 0;
	out->int_en    = AlPmu_GetIntEn(AL_PMU_ERR_WDT0_OT) ? 1 : 0;
	out->int_state = AlPmu_GetIntState(AL_PMU_ERR_WDT0_OT) ? 1 : 0;
	out->raw_his   = AlPmu_GetRawHistoryState(AL_PMU_ERR_WDT0_OT) ? 1 : 0;
}

/* ---- RST_REASON（上次复位原因）-------------------------------------------
 * ⚠️ 状态变量 g_last_rst_reason / g_rst_reason_valid 声明在**本文件顶部** ——
 *    apu_wdt_init() 比这里的函数先用它们，C 要求先声明。 */

static void rst_reason_decode(uint32_t raw, apu_wdt_rst_reason_t *out)
{
	out->raw       = raw;
	out->srst_b    = (raw & (1UL << 0)) ? 1 : 0;
	out->srst_soft = (raw & (1UL << 1)) ? 1 : 0;
	out->jtag_bsu  = (raw & (1UL << 4)) ? 1 : 0;
	out->pmu_err   = (raw & (1UL << 5)) ? 1 : 0;
	out->swdt0     = (raw & (1UL << 6)) ? 1 : 0;
	out->swdt1     = (raw & (1UL << 7)) ? 1 : 0;
	out->swdt2     = (raw & (1UL << 8)) ? 1 : 0;
}

void apu_wdt_rst_reason_read(apu_wdt_rst_reason_t *out)
{
	uint32_t raw;

	if (out == AL_NULL)
		return;

	raw = AL_REG32_READ((AL_REG)APU_WDT_RST_REASON_ADDR);
	rst_reason_decode(raw, out);

	/* w1c：把读到的位写 1 回去清除。
	 * 清掉之后，下一次开机读到的就只反映"紧邻的这一次"复位。 */
	if (raw != 0U)
		AL_REG32_WRITE((AL_REG)APU_WDT_RST_REASON_ADDR, raw);
}

int apu_wdt_last_rst_reason(apu_wdt_rst_reason_t *out)
{
	if (!g_rst_reason_valid)
		return 0;
	if (out != AL_NULL)
		*out = g_last_rst_reason;
	return 1;
}

void apu_wdt_dump_regs(void)
{
	uint32_t base, ccvr1, ccvr2;
	apu_wdt_pmu_t pmu;

	/* ★★ 2026-09-21 改：**读一个打印一个** ★★
	 *  原来是把所有寄存器先读完、最后才打印 —— 只要中间任何一次访问异常，
	 *  就会出现"敲了 @wdt:regs 完全没反应"（一行都不打），而且查不出卡在哪。
	 *  现在每步都立刻打印：**最后打出来的那一行，就是卡住的位置**。 */
	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][wdt] @wdt:regs BEGIN (逐条打印；此行之后没输出 = 卡在那里)");

	if (g_wdt == AL_NULL) {
		AL_LOG(AL_LOG_LEVEL_ERROR, "[APU1][wdt] no handle - WDT not initialised");
		return;
	}

	base = (uint32_t)g_wdt->BaseAddr;
	AL_LOG(AL_LOG_LEVEL_INFO, "[APU1][wdt] base=0x%08X", (unsigned int)base);

	{
		uint32_t cr = AL_REG32_READ((AL_REG)(base + (uint32_t)WDT__WDT_CR__ADDR__OFFSET));

		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][wdt] CR   =0x%08X  WDT_EN=%u RMOD=%u(%s) RPL=%u",
		       (unsigned int)cr,
		       (unsigned int)(cr & 1U),
		       (unsigned int)((cr >> 1) & 1U),
		       ((cr >> 1) & 1U) ? "INTR 只中断!" : "RESET 复位",
		       (unsigned int)((cr >> 2) & 7U));
	}
	{
		uint32_t torr = AL_REG32_READ((AL_REG)(base + (uint32_t)WDT__WDT_TORR__ADDR__OFFSET));

		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][wdt] TORR =0x%08X  TOP=0x%X (0xa=64M clocks ~3.84s @16.665MHz)",
		       (unsigned int)torr, (unsigned int)(torr & 0xFU));
	}
	ccvr1 = AL_REG32_READ((AL_REG)(base + (uint32_t)WDT__WDT_CCVR__ADDR__OFFSET));
	AL_LOG(AL_LOG_LEVEL_INFO, "[APU1][wdt] CCVR1=0x%08X", (unsigned int)ccvr1);

	{
		uint32_t stat = AL_REG32_READ((AL_REG)(base + (uint32_t)WDT__WDT_STAT__ADDR__OFFSET));

		AL_LOG(AL_LOG_LEVEL_INFO, "[APU1][wdt] STAT =0x%08X", (unsigned int)stat);
	}
	{
		uint32_t eoi = AL_REG32_READ((AL_REG)(base + (uint32_t)WDT__WDT_EOI__ADDR__OFFSET));

		AL_LOG(AL_LOG_LEVEL_INFO, "[APU1][wdt] EOI  =0x%08X", (unsigned int)eoi);
	}

	AL_LOG(AL_LOG_LEVEL_INFO, "[APU1][wdt] ... 等 200ms 再读一次 CCVR ...");
	AlSys_MDelay(200U);
	ccvr2 = AL_REG32_READ((AL_REG)(base + (uint32_t)WDT__WDT_CCVR__ADDR__OFFSET));
	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][wdt] CCVR2=0x%08X   %s",
	       (unsigned int)ccvr2,
	       (ccvr1 == ccvr2) ? "★ 两次一样 -> 计数器【没在走】!"
				: "两次不同 -> 计数器在走 OK");

	/* ⬇ 这一句是**分界线**：看到它、却没有下一行，就是这次读出了问题 */
	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][wdt] 下面读 TOP_NS 区 0x%08X (WDT_TOP_CFG_CTRL_ADDR) ...",
	       (unsigned int)WDT_TOP_CFG_CTRL_ADDR);
	{
		uint32_t top = AL_REG32_READ((AL_REG)WDT_TOP_CFG_CTRL_ADDR);

		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][wdt] TOP_CFG=0x%08X  WDT0_PAUSE=%u",
		       (unsigned int)top, (unsigned int)(top & 1U));
	}

	AL_LOG(AL_LOG_LEVEL_INFO, "[APU1][wdt] ... 读 PMU ...");
	apu_wdt_pmu_read(&pmu);
	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][wdt] PMU  hw_en=%d int_en=%d int_state=%d raw_his=%d",
	       pmu.hw_en, pmu.int_en, pmu.int_state, pmu.raw_his);

	/* RST_REASON 已经被 init 读过并清掉了，这里打的是存下来的那份 */
	if (g_rst_reason_valid) {
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][wdt] RST_REASON(本次开机读到, 已清) =0x%08X  "
		       "SWDT0=%d SWDT1=%d SWDT2=%d PMU_ERR=%d JTAG=%d SRST_soft=%d SRST_pin=%d",
		       (unsigned int)g_last_rst_reason.raw,
		       g_last_rst_reason.swdt0, g_last_rst_reason.swdt1,
		       g_last_rst_reason.swdt2, g_last_rst_reason.pmu_err,
		       g_last_rst_reason.jtag_bsu, g_last_rst_reason.srst_soft,
		       g_last_rst_reason.srst_b);
	}
	{
		uint32_t oc = AL_REG32_READ((AL_REG)APU_WDT_OUT_CTRL_ADDR);
	
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][wdt] WDT_OUT_CTRL=0x%08X (wdt0/1/2_rst_outen=%u/%u/%u) "
		       "1=把 wdtN_rst_n 引到 PS_IO/PL_IO",
		       (unsigned int)oc, (unsigned int)(oc & 1U),
		       (unsigned int)((oc >> 1) & 1U), (unsigned int)((oc >> 2) & 1U));
	}

	if (!pmu.hw_en) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][wdt] ★ hw_en=0 -> PMU 没允许 'WDT0 超时->复位芯片'。"
		       "狗会超时但【不会复位芯片】。");
	}
	if (pmu.raw_his) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][wdt] ★ raw_his=1 -> 狗【确实超时过】(计数在走)，"
		       "问题在 PMU 复位通路，不在 WDT 本身。");
	} else {
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][wdt] raw_his=0 -> 复位后还没超时过（正常）。"
		       "发完 CMD:WDT_TEST 等 5s 再打一次，变 1 就说明超时确实发生了。");
	}
}
