/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_nav_safety.h —— 导航安全评估
 * ===========================================================================
 *  【来源】传承项目 navigation_safety.h/c，**零改动照抄**（纯逻辑）。
 *
 *  它回答一个问题：**"现在能不能让车动？"**
 *  按顺序检查，返回第一个不满足的条件：
 *
 *      ① IMU 有没有数据？          没有 -> IMU_NOT_READY
 *      ② IMU 数据是不是太旧？      超过 1.0s -> IMU_STALE
 *      ③ 超声波有没有数据？        没有 -> SONAR_NOT_READY
 *      ④ 超声波是不是太旧？        超过 1.0s -> SONAR_STALE
 *      ⑤ 三路超声波值有效吗？      20..2000mm 外 -> SONAR_INVALID
 *      ⑥ 前方是不是太近？          <= 50mm -> FRONT_TOO_CLOSE
 *      否则 -> OK
 *
 *  ⚠️ 时间单位是 **10ms tick**（对应我们 apu_time 的 apu_tick_10ms()），
 *     所以下面这些常量【一个都不用改】就能直接用。
 * ===========================================================================
 */

#ifndef APU_NAV_SAFETY_H
#define APU_NAV_SAFETY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 门限（与传承项目逐字一致）---- */
#define NAV_IMU_TIMEOUT_TICKS   100U    /* 100 × 10ms = 1.0 s 无新数据 -> 陈旧 */
#define NAV_SONAR_TIMEOUT_TICKS 100U    /* 100 × 10ms = 1.0 s */
#define NAV_FRONT_STOP_MM       50U     /* 前方 <= 50mm -> 太近 */
#define NAV_SONAR_MIN_MM        20U     /* 有效范围 */
#define NAV_SONAR_MAX_MM        2000U

/* ===========================================================================
 * 🔴🔴 调试开关：比赛前必须改回 0 ！！！
 * ===========================================================================
 *  1 = 旁路安全门（永远返回 OK）。原项目为了**调参方便**故意留的。
 *      开着它，安全门形同虚设 —— 撞墙不会自动停。
 *  0 = 正常生效。
 *
 *  ★ 这是 P9 收尾检查清单里的**必查项**。
 * =========================================================================== */
#define NAV_SAFETY_BYPASS_FOR_TUNING    1U

typedef enum {
	NAV_SAFETY_OK = 0,
	NAV_SAFETY_BAD_INPUT,
	NAV_SAFETY_IMU_NOT_READY,
	NAV_SAFETY_IMU_STALE,
	NAV_SAFETY_SONAR_NOT_READY,
	NAV_SAFETY_SONAR_STALE,
	NAV_SAFETY_SONAR_INVALID,
	NAV_SAFETY_FRONT_TOO_CLOSE
} apu_nav_safety_fault_t;

typedef struct {
	uint32_t now_tick_10ms;
	uint32_t imu_last_tick_10ms;
	uint32_t sonar_last_tick_10ms;
	uint16_t front_mm;
	int      imu_seen;
	int      sonar_seen;
	int      left_valid;
	int      front_valid;
	int      right_valid;
} apu_nav_safety_input_t;

/* 单路超声波值是否在有效范围内 */
int apu_nav_safety_sonar_value_valid(uint16_t distance_mm);

/* 三个姿态角是否合法（有限 + 在 ±180° 内） */
int apu_nav_safety_imu_angles_valid(float roll_deg, float pitch_deg, float yaw_deg);

/* 总评估：返回第一个不满足的条件；全满足返回 NAV_SAFETY_OK */
apu_nav_safety_fault_t apu_nav_safety_evaluate(const apu_nav_safety_input_t *input);

/* 故障名（用于 "ACK: REJECTED, NAV SAFETY <名字>" 和 FAULT:NAV 上报） */
const char *apu_nav_safety_fault_name(apu_nav_safety_fault_t fault);

#ifdef __cplusplus
}
#endif

#endif /* APU_NAV_SAFETY_H */
