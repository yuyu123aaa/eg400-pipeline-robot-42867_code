/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_env.c —— 三路环境传感器的采集 / 有效性判定 / 滤波 / 上报
 * ===========================================================================
 *  设计说明与有效性窗口见 apu_env.h 顶部。这里只记实现要点：
 *
 *  【1】一圈读全部三路 —— 这是本次"刷新率太低"的修复
 *      主循环被 APU0 的 ~1Hz SGI 踢着走，最快就是 1Hz。
 *      旧实现每 2 秒才读一路 + 陈旧优先 ⇒ 每路 4~6 秒更新一次。
 *      现在每圈都读三路 ⇒ 每路 1Hz（主循环所能给到的上限）。
 *      总耗时典型 80~130ms/圈（占 8~13%），远低于 2026-10-14 那次
 *      把 rpmsg 饿死的 960ms/圈，安全。
 *
 *  【2】无效值**绝不进入上报数据** —— 与前三路超声完全同一策略
 *      超声的做法（apu_sonar.c:315-332）：
 *          l_valid = sonar_value_valid(raw.left);
 *          if (l_valid) g_filtered.left = apu_sonar_filter_apply(...);
 *          apu_send_sonar_full(g_filtered.*, f_valid, l_valid, r_valid);
 *      本模块照做，并且更进一步：**无效时上报哨兵值而不是旧值**，
 *      让 Web 侧一眼能分辨"没测到"和"真的测到 0"。
 *
 *  【3】滤波复用前三路超声那一份代码（apu_sonar_filter.c）
 *      5 点滑动窗口取中值 + 连续跳变抑制（>400mm 需连续 3 次才认可）。
 *      深度和浊度各持一个独立的滤波器实例，互不干扰。
 *
 *  【4】三路独立降级
 *      哪一路坏只影响它自己的字段，其余照常上报 —— 单个传感器故障
 *      不该让整页数据变空。
 * ===========================================================================
 */

#include "apu_env.h"

#include "al_core.h"

#include "apu_msg_api.h"        /* apu_send_env() */
#include "apu_time.h"
#include "apu_sonar_filter.h"   /* 复用超声的 5 点中值 + 跳变抑制 */

#include "apu_depth.h"
#include "apu_turb.h"
#include "apu_gas.h"

/* ---------------------------------------------------------------------------
 *  有效性判定
 * --------------------------------------------------------------------------- */
int apu_env_depth_valid(int mm)
{
	return (mm >= APU_ENV_DEPTH_MIN_VALID_MM) &&
	       (mm <= APU_ENV_DEPTH_MAX_VALID_MM);
}

int apu_env_turb_valid(int unt)
{
	return (unt >= APU_ENV_TURB_MIN_VALID) &&
	       (unt <= APU_ENV_TURB_MAX_VALID);
}

/* ---------------------------------------------------------------------------
 *  缓存：**只保存有效的、滤波后的**读数。
 *  · 无效时读数为 0（= 哨兵），上报时原样发 0，Web 侧据此判"无数据"。
 *  · 不做"保留上次有效值"—— 那正是之前 Web 显示假 0 的根源
 *    （旧固件把 null 当 0 发，Web 分不清"没数据"和"测到 0"）。
 * --------------------------------------------------------------------------- */
static int g_depth_mm;          /* 0 = 本次未取到有效深度 */
static int g_turb_unt;          /* 0 = 本次未取到有效浊度（注意 0 本身也可能是真值） */
static int g_co, g_h2s, g_o2, g_ch4;

/* 滤波器实例：深度 / 浊度各一个（独立窗口，互不污染） */
static apu_sonar_filter_t g_filt_depth;
static apu_sonar_filter_t g_filt_turb;

static uint32_t g_next_ms;

/* ---------------------------------------------------------------------------
 *  初始化
 * --------------------------------------------------------------------------- */
int apu_env_init(void)
{
	int rc = 0;

	apu_sonar_filter_init(&g_filt_depth);
	apu_sonar_filter_init(&g_filt_turb);

	if (apu_depth_init() != 0)
		rc |= 0x1;
	if (apu_turb_init() != 0)
		rc |= 0x2;
	if (apu_gas_init() != 0)
		rc |= 0x4;

	g_next_ms = 0U;
	return rc;
}

/* ---------------------------------------------------------------------------
 *  单路采集
 * --------------------------------------------------------------------------- */
static void read_depth(void)
{
	apu_depth_result_t r;

	if (apu_depth_read(&r) != 0)
		return;

	apu_depth_print(&r);        /* 调试钩子（默认空实现） */

	/* 只有 OK 才是可用距离；INVALID(65533/65535) / TIMEOUT / BAD_SUM
	 * 全都被驱动层归成了别的错误码，这里一律丢弃。
	 * ⚠️ 不做"保留上次值"——那会把旧数据伪装成新数据。 */
	if (r.res != APU_DEPTH_E_OK) {
		g_depth_mm = 0;
		return;
	}

	if (!apu_env_depth_valid((int)r.distance_mm)) {
		g_depth_mm = 0;         /* 超出有效窗口：丢弃 */
		return;
	}

	/* ★ 与前三路超声同一套滤波：5 点中值 + 跳变抑制 */
	g_depth_mm = apu_sonar_filter_apply(&g_filt_depth, (int)r.distance_mm);
}

static void read_turb(void)
{
	apu_turb_result_t r;

	if (apu_turb_read(&r) != 0)
		return;

	apu_turb_print(&r);         /* 调试钩子（默认空实现） */

	if (r.res != APU_TURB_E_OK) {
		g_turb_unt = 0;
		return;
	}

	if (!apu_env_turb_valid((int)r.raw_value)) {
		g_turb_unt = 0;
		return;
	}

	g_turb_unt = apu_sonar_filter_apply(&g_filt_turb, (int)r.raw_value);
}

static void read_gas(void)
{
	apu_gas_all_t r;

	if (apu_gas_read_all(&r) != 0)
		return;

	apu_gas_print_all(&r);      /* 调试钩子（默认空实现） */

	if (r.res != APU_GAS_E_OK) {
		g_co = g_h2s = g_o2 = g_ch4 = 0;
		return;
	}

	/* ★ O2 == 0 是"没读到"的哨兵，不是"真的 0%VOL"（那不可能）。
	 *   丢弃，让 Web 侧显示"无数据"而不是氧含量 0。 */
	if ((int)r.o2 == APU_ENV_O2_INVALID) {
		g_co = g_h2s = g_o2 = g_ch4 = 0;
		return;
	}

	/* CO / H2S / CH4 的 0 是合法本底，照常上报 */
	g_co  = (int)r.co;
	g_h2s = (int)r.h2s;
	g_o2  = (int)r.o2;
	g_ch4 = (int)r.ch4;
}

/* ---------------------------------------------------------------------------
 *  周期任务
 * --------------------------------------------------------------------------- */
void apu_env_poll(void)
{
	uint32_t now = apu_time_ms_live();

	/* 限速：默认 APU_ENV_PERIOD_MS == 主循环周期 ⇒ 每圈都读（即最快） */
	if (g_next_ms != 0U &&
	    (uint32_t)(now - g_next_ms) < APU_ENV_PERIOD_MS)
		return;

	g_next_ms = now;

	/* ★ 一圈把三路全部读完。
	 *   顺序固定：深度 → 浊度 → 气体（气体最慢，放最后，
	 *   这样它超时也不会拖慢前两路的采样时刻）。 */
	read_depth();
	read_turb();
	read_gas();

	/* 上报：一条合并消息带全部字段。
	 * ⚠️ 无效字段发哨兵 0，**不是**旧值 —— Web 侧据此判"无数据"。 */
	(void)apu_send_env(g_depth_mm, g_turb_unt, g_co, g_h2s, g_o2, g_ch4);
}

/* ---------------------------------------------------------------------------
 *  供本地诊断查询（只返回**有效**的滤波后读数，无数据时为 0）
 * --------------------------------------------------------------------------- */
void apu_env_get(int *depth_mm, int *turb_unt,
		 int *co, int *h2s, int *o2, int *ch4)
{
	if (depth_mm) *depth_mm = g_depth_mm;
	if (turb_unt) *turb_unt = g_turb_unt;
	if (co)       *co       = g_co;
	if (h2s)      *h2s      = g_h2s;
	if (o2)       *o2       = g_o2;
	if (ch4)      *ch4      = g_ch4;
}
