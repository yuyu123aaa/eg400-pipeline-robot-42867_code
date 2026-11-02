/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_sonar.c —— APU1 裸机侧超声波采集实现
 * ===========================================================================
 *  逐段对照传承项目 sonar.c:69-110 与 slaver_00_example.c:1005-1089 写成。
 * ===========================================================================
 */

#include "apu_sonar.h"

#include "al_core.h"

#include "apu_app.h"
#include "apu_front.h"      /* 前向直连 UART-D，取最近一次有效原始值 */
#include "apu_i2c.h"
#include "apu_msg_api.h"
#include "apu_msg.h"
#include "apu_sonar_filter.h"
#include "apu_time.h"
#include "apu_wdt.h"        /* apu_wdt_feed —— I2C 阻塞读之后补喂狗 */

/* ---------------------------------------------------------------------------
 * 常量
 * --------------------------------------------------------------------------- */
#define APU_SONAR_I2C_BUS        APU_I2C_BUS1
#define APU_SONAR_I2C_ADDR       APU_I2C_ADDR_STM32_SONAR   /* 0x40 */
#define APU_SONAR_REG_START      0x10U                      /* 原项目 sonar.c:76 */
#define APU_SONAR_BYTES          6U                         /* 左/前/右 各 2 字节，大端 */

/* 有效性范围用 apu_sonar_filter.h 的 APU_SONAR_MIN_VALID_MM / APU_SONAR_MAX_VALID_MM
 * （20..2000mm，与 navigation_safety.h 的 NAV_SONAR_MIN_MM/MAX_MM 一致）。
 * 不在这里重复 define，避免重定义告警。 */

/* 上报周期：传承项目靠 10 次 tick 触发（10ms tick -> 100ms = 10Hz） */
#define APU_SONAR_PERIOD_MS      100U

/* ---------------------------------------------------------------------------
 * 状态
 * --------------------------------------------------------------------------- */
static apu_sonar_filter_t g_filt_front;
static apu_sonar_filter_t g_filt_left;
static apu_sonar_filter_t g_filt_right;

static apu_sonar_raw_t g_filtered;      /* 最近一次滤波结果（给 P8 居中控制用） */

static uint32_t   g_last_period_ms;
static uint8_t    g_last_damage_mask;   /* 用于边沿触发上报 */
static int        g_damage_reported;    /* 是否已经报过当前这一轮故障 */
static uint32_t   g_ok_count;
static uint32_t   g_err_count;
static int        g_sonar_ready;

/* ===========================================================================
 * ★ 连续失败熔断
 * ===========================================================================
 *  【为什么需要它】
 *
 *  SDK 的 `AlIic_Dev_MasterRecvDataPolling()` 结尾是：
 *        while (AlIic_ll_IsMasterActivity(BaseAddr));     // al_iic_dev.c
 *  **这是一个没有超时的忙等**。如果 SCL/SDA 被拉死、总线卡住，
 *  这个循环会**永远转下去** —— 主循环被挂住，喂狗也停了。
 *
 *  【熔断能做什么 / 不能做什么（别夸大）】
 *     ✅ 能：连续读失败达到阈值后**跳过** I2C 读取，避免在"一个已经坏了的
 *            从机 / 一条已经烂了的总线"上反复浪费时间；串口会明确告诉你
 *            "已熔断，等 X 秒重试"，比主循环卡住好排查得多。
 *     ❌ 不能：如果**某一次**读本身就把总线卡死了，那一次还是会卡住
 *            （软件拦不住 SDK 内部的忙等）。
 *            那种情况下的兜底是**硬件看门狗** —— 前提是 `@wdt` 显示
 *            `hw_active=1`（见 P9 测试清单第 3 条）。
 *
 *  【恢复】每 APU_SONAR_BREAKER_RETRY_MS 允许再试一次，成功了就完全恢复。
 * =========================================================================== */
#define APU_SONAR_BREAKER_FAILS      5U       /* 连续失败几次就熔断 */
#define APU_SONAR_BREAKER_RETRY_MS   5000U    /* 熔断后多久允许再试 */

static uint32_t g_consecutive_fails;
static int      g_breaker_open;          /* 1 = 已熔断（在跳过读取） */
static uint32_t g_breaker_tripped_count; /* 熔断触发次数 */
static uint32_t g_breaker_last_try_ms;   /* 熔断后最后一次尝试的时刻 */

/* ---------------------------------------------------------------------------
 * 内部：有效性判定（对应 NavSafety_IsSonarValueValid） */
static int sonar_value_valid(uint16_t mm)
{
	return (mm >= (uint16_t)APU_SONAR_MIN_VALID_MM) &&
	       (mm <= (uint16_t)APU_SONAR_MAX_VALID_MM);
}

/* ---------------------------------------------------------------------------
 * 初始化
 * --------------------------------------------------------------------------- */
int apu_sonar_init(void)
{
	apu_sonar_reset_filters();

	g_last_period_ms = 0U;
	g_last_damage_mask = 0U;
	g_damage_reported = 0;
	g_ok_count = 0U;
	g_err_count = 0U;
	g_consecutive_fails = 0U;
	g_breaker_open = 0;
	g_breaker_tripped_count = 0U;
	g_breaker_last_try_ms = 0U;

	/* 总线本身由 apu_i2c_init() 建好；这里只贴一个"存在性"标记，
	 * 真正的连通性用读一次来验证 */
	g_sonar_ready = 1;

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][sonar] ready: bus%d addr=0x%02X reg=0x%02X %u bytes (大端 左/前/右, mm)",
	       (int)APU_SONAR_I2C_BUS, (unsigned int)APU_SONAR_I2C_ADDR,
	       (unsigned int)APU_SONAR_REG_START, (unsigned int)APU_SONAR_BYTES);

	/* 开机先试读一次，把"到底通不通"立刻暴露出来（失败不算致命） */
	{
		apu_sonar_raw_t probe;

		if (apu_sonar_read_raw(&probe) == 0) {
			AL_LOG(AL_LOG_LEVEL_INFO,
			       "[APU1][sonar] first read OK: L=%u F=%u R=%u (mm, 交换前)",
			       (unsigned int)probe.left, (unsigned int)probe.front,
			       (unsigned int)probe.right);
		} else {
			AL_LOG(AL_LOG_LEVEL_ERROR,
			       "[APU1][sonar] first read FAILED - check I2C1 wiring "
			       "(CEP 5/6 = T9/U10), 0x40 address, pull-ups, common GND");
			apu_i2c_dump_last_abort(APU_SONAR_I2C_BUS);
		}
	}

	return 0;
}

void apu_sonar_reset_filters(void)
{
	apu_sonar_filter_init(&g_filt_front);
	apu_sonar_filter_init(&g_filt_left);
	apu_sonar_filter_init(&g_filt_right);
	g_filtered.left = 0U;
	g_filtered.front = 0U;
	g_filtered.right = 0U;
}

/* ---------------------------------------------------------------------------
 * 原始读取（对应传承项目 sonar.c:69-110 的 Sonar_ReadDistances）
 * --------------------------------------------------------------------------- */
int apu_sonar_read_raw(apu_sonar_raw_t *out)
{
	uint8_t rx[APU_SONAR_BYTES];
	int ret;

	if (out == AL_NULL)
		return -1;

	/* ---- ★ 熔断检查：连续失败太多次就先跳过，别反复戳坏总线 ---- */
	if (g_breaker_open) {
		uint32_t now = apu_time_ms();

		if ((uint32_t)(now - g_breaker_last_try_ms) <
		    APU_SONAR_BREAKER_RETRY_MS) {
			g_err_count++;
			return -5;      /* -5 = 熔断中，本次跳过 */
		}
		/* 到了重试时刻，放行一次 */
		g_breaker_last_try_ms = now;
	}

	/* ★ 复合读：写寄存器地址(RESTART) -> 读 6 字节(STOP)
	 *   —— apu_i2c_read_regs 内部就是这么做的 */
	ret = apu_i2c_read_regs(APU_SONAR_I2C_BUS, (uint8_t)APU_SONAR_I2C_ADDR,
				(uint8_t)APU_SONAR_REG_START, rx, APU_SONAR_BYTES);
	if (ret != 0) {
		g_err_count++;
		g_consecutive_fails++;

		if (!g_breaker_open &&
		    g_consecutive_fails >= APU_SONAR_BREAKER_FAILS) {
			g_breaker_open = 1;
			g_breaker_tripped_count++;
			g_breaker_last_try_ms = apu_time_ms();
			AL_LOG(AL_LOG_LEVEL_ERROR,
			       "[APU1][sonar] CIRCUIT BREAKER OPEN after %u consecutive "
			       "failures (last ret=%d). Skipping reads for %u ms, then retrying. "
			       "Check I2C1 wiring / 0x40 device / pull-ups.",
			       (unsigned int)g_consecutive_fails, ret,
			       (unsigned int)APU_SONAR_BREAKER_RETRY_MS);
		}
		return ret;
	}

	/* 读成功 -> 清零失败计数，熔断自动恢复 */
	if (g_breaker_open) {
		g_breaker_open = 0;
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][sonar] circuit breaker CLOSED (bus recovered)");
	}
	g_consecutive_fails = 0;

	/* 大端拼接（原项目 sonar.c:100-102 原文） */
	out->left  = (uint16_t)(((uint16_t)rx[0] << 8) | (uint16_t)rx[1]);
	out->front = (uint16_t)(((uint16_t)rx[2] << 8) | (uint16_t)rx[3]);
	out->right = (uint16_t)(((uint16_t)rx[4] << 8) | (uint16_t)rx[5]);

	g_ok_count++;
	return 0;
}

/* ---------------------------------------------------------------------------
 * 采集 + 滤波 + 上报（对应 slaver_00_example.c:1005-1089 的 Sonar_Proc）
 * ---------------------------------------------------------------------------
 *  ⚠️ 顺序很重要，别调换：
 *      ① 先交换 front/right（硬件安装方向）
 *      ② **再**判损坏（原项目 :1023 就是放在滤波之前）
 *      ③ 判有效性
 *      ④ 最后才滤波
 * =========================================================================== */
int apu_sonar_periodic(void)
{
	apu_sonar_raw_t raw;
	uint32_t now_ms;
	uint8_t damage;
	int l_valid, f_valid, r_valid;
	int front_fresh = 0;

	if (!g_sonar_ready)
		return 0;

	/* ---- 10Hz 节流 ---- */
	now_ms = apu_time_ms();
	if (g_last_period_ms != 0U &&
	    (uint32_t)(now_ms - g_last_period_ms) < APU_SONAR_PERIOD_MS)
		return 0;
	g_last_period_ms = now_ms;

	/* ---- ① 读 + 交换 front/right（原项目 :1020-1021） ---- */
	if (apu_sonar_read_raw(&raw) != 0)
		return 0;

	/* ★ 2026-09-25：读完立刻补喂一次狗。
	 *
	 *  为什么这里必须喂：主循环里唯一的常规喂狗点是
	 *  apu_app_periodic() 的**最后一行**（apu_app.c:1279），
	 *  而这一步在前面。apu_sonar_read_raw() 走的是 SDK 的 I2C 阻塞版 API，
	 *  它内部有两个**无超时忙等**（见本文件上方 :58 的注释，
	 *  al_iic_hal.c 的 IsTxFifoCompleteEmpty / IsMasterActivity）。
	 *  一旦总线异常（例如 SCL 被拉低）这两个 while 会长时间不返回，
	 *  主循环就卡在喂狗点之前 → 狗咬 → 整板复位。
	 *
	 *  补喂之后：即使本次读被拖长，也只是"这一轮变慢"，
	 *  不会再演变成复位。上板日志的 reset reason bit6(SWDT0) 就是这类复位。 */
	apu_wdt_feed();

	{
		/* ⚠️ 原项目**故意**把 front 和 right 换位（硬件装反了）：
		 *        temp = raw.front;  raw.front = raw.right;  raw.right = temp;
		 *    照抄，不要"修正"它，否则车会往偏。 */
		uint16_t temp = raw.front;

		raw.front = raw.right;
		raw.right = temp;
	}

	/* ★ 2026-10-30：前向已改为直连 UART-D，不再用 32 板 I2C 的 front 字段。
	 *   左/右仍来自 I2C；这里只覆盖 raw.front。
	 *   apu_front_poll() 是非阻塞的，左/右 10Hz 采集不会被前向拖慢。 */
	{
		uint16_t front_mm = 0U;

		(void)apu_front_get(&front_mm, &front_fresh);
		raw.front = front_mm;
	}

	/* ---- ② 损坏判定（放在滤波之前！原项目 :1023） ---- */
	damage = apu_sonar_damage_mask(raw.front, raw.left, raw.right);

	if (damage != 0U) {
		/* ★ 2026-09-21（以传承为准）：拆成**两个独立条件**（slaver:1024-1040）：
		 *     ① "之前是好的"（掩码还是 0）才做故障停车；
		 *     ② 掩码**变了**才发 FAULT 帧。
		 *   以前我们把两条合并成"掩码变了"，于是掩码从 1 直接变 3 时会**多停一次车**。
		 *   停车是幂等的、多停无害，但传承是这么写的 —— 以传承为准。 */
		if (g_last_damage_mask == 0U)
			apu_app_safe_stop("sonar damaged (65535)");

		if (damage != g_last_damage_mask) {
			g_last_damage_mask = damage;
			g_damage_reported = 1;
			AL_LOG(AL_LOG_LEVEL_ERROR,
			       "[APU1][sonar] DAMAGE mask=0x%X (L=%u F=%u R=%u) -> safe stop",
			       (unsigned int)damage, (unsigned int)raw.left,
			       (unsigned int)raw.front, (unsigned int)raw.right);
			apu_send_sonar_fault((unsigned int)damage);
		}

		/* 只把"损坏"这件事交给业务层（安全门要据此拒绝 CMD:MOTOR_START）。
		 * 距离/时间戳**一律不更新** —— 与传承一致（它判到损坏就 return，
		 * 后面的 sonar_seen / sonar_last_update_tick 都不执行 => 安全门判 STALE）。 */
		{
			apu_sonar_update_t su;

			su.front_raw = 0;
			su.left_raw = 0;
			su.right_raw = 0;
			su.front_valid = 0;
			su.left_valid = 0;
			su.right_valid = 0;
			su.ctrl_left = 0;
			su.ctrl_right = 0;
			su.damage_mask = damage;

			apu_app_update_sonar(&su);
		}
		return 1;
	}

	/* 从故障恢复：也走边沿触发 */
	if (g_last_damage_mask != 0U) {
		g_last_damage_mask = 0U;
		g_damage_reported = 0;
		AL_LOG(AL_LOG_LEVEL_INFO, "[APU1][sonar] recovered");
		apu_send_sonar_recover();
	}

	/* ---- ③ 有效性（原项目 :1051-1056） ----
	 *  前向额外要求 front_fresh：UART-D 超过 600ms 没有新合法帧就置 V=0，
	 *  避免把旧值当新值上报。左/右仍按 I2C 的原始值判。 */
	l_valid = sonar_value_valid(raw.left);
	f_valid = front_fresh && sonar_value_valid(raw.front);
	r_valid = sonar_value_valid(raw.right);

	/* ---- ④ 滤波（原项目 :1062-1076） ---- */
	if (l_valid)
		g_filtered.left = (uint16_t)apu_sonar_filter_apply(&g_filt_left, (int)raw.left);
	if (f_valid)
		g_filtered.front = (uint16_t)apu_sonar_filter_apply(&g_filt_front, (int)raw.front);
	if (r_valid)
		g_filtered.right = (uint16_t)apu_sonar_filter_apply(&g_filt_right, (int)raw.right);

	/* ---- ⑤ 上报（原项目 :1078-1087 的完整格式，含有效性后缀） ----
	 *   "SONAR:%d,%d,%d,V:%d%d%d"  顺序 = 前,左,右 */
	if (apu_msg_periodic_report_enabled())
		apu_send_sonar_full((int)g_filtered.front, (int)g_filtered.left,
			    (int)g_filtered.right, f_valid, l_valid, r_valid);

	/* ★★ 2026-09-21（以传承为准）：交给业务层 —— **分两路** ★★
	 *   安全门  ：**原始**前向距离 + 从原始值算的三路有效性
	 *             （传承 NavSafety_BuildInput().front_mm = sonar_raw_front_mm；
	 *               有效性在 slaver:1051-1056 从 raw_sonar 算）
	 *   居中控制：**滤波**左右距离
	 *             （传承 ESC_Proc_Pipeline_Auto_Nav():1134-1135 用 sonar_data.left/right）
	 *   ⚠️ 别把这两路合并：安全门等不起滤波的滞后，控制又需要滤波的平滑。 */
	{
		apu_sonar_update_t su;

		su.front_raw   = (int)raw.front;
		su.left_raw    = (int)raw.left;
		su.right_raw   = (int)raw.right;
		su.front_valid = f_valid;
		su.left_valid  = l_valid;
		su.right_valid = r_valid;
		su.ctrl_left   = (int)g_filtered.left;
		su.ctrl_right  = (int)g_filtered.right;
		su.damage_mask = 0U;

		apu_app_update_sonar(&su);
	}

	return 1;
}

/* ---------------------------------------------------------------------------
 * 查询
 * --------------------------------------------------------------------------- */
void apu_sonar_get_filtered(apu_sonar_raw_t *out)
{
	if (out == AL_NULL)
		return;
	*out = g_filtered;
}

uint32_t apu_sonar_read_ok_count(void)
{
	return g_ok_count;
}

uint32_t apu_sonar_read_err_count(void)
{
	return g_err_count;
}

/* ★ 熔断状态查询（测试协议里"拔线"用例会看这个） */
int apu_sonar_breaker_open(void)
{
	return g_breaker_open;
}

uint32_t apu_sonar_breaker_tripped_count(void)
{
	return g_breaker_tripped_count;
}

uint32_t apu_sonar_consecutive_fails(void)
{
	return g_consecutive_fails;
}
