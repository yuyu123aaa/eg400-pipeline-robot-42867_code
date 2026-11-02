/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_nav_safety.c —— 导航安全评估实现
 * ===========================================================================
 *  逐行对照传承项目 navigation_safety.c（93 行）写成，**逻辑零改动**。
 * ===========================================================================
 */

#include <math.h>

#include "apu_nav_safety.h"

int apu_nav_safety_sonar_value_valid(uint16_t distance_mm)
{
	/* 原项目 NavSafety_IsSonarValueValid() */
	return distance_mm >= (uint16_t)NAV_SONAR_MIN_MM &&
	       distance_mm <= (uint16_t)NAV_SONAR_MAX_MM;
}

int apu_nav_safety_imu_angles_valid(float roll_deg, float pitch_deg, float yaw_deg)
{
	/* 原项目 NavSafety_IsImuAnglesValid() */
	return isfinite(roll_deg) &&
	       isfinite(pitch_deg) &&
	       isfinite(yaw_deg) &&
	       roll_deg >= -180.0f && roll_deg <= 180.0f &&
	       pitch_deg >= -180.0f && pitch_deg <= 180.0f &&
	       yaw_deg >= -180.0f && yaw_deg <= 180.0f;
}

apu_nav_safety_fault_t apu_nav_safety_evaluate(const apu_nav_safety_input_t *input)
{
	/* 原项目 NavSafety_Evaluate()，顺序完全一致 */
	if (input == 0)
		return NAV_SAFETY_BAD_INPUT;

	if (!input->imu_seen)
		return NAV_SAFETY_IMU_NOT_READY;

	if ((uint32_t)(input->now_tick_10ms - input->imu_last_tick_10ms) >=
	    NAV_IMU_TIMEOUT_TICKS)
		return NAV_SAFETY_IMU_STALE;

	if (!input->sonar_seen)
		return NAV_SAFETY_SONAR_NOT_READY;

	if ((uint32_t)(input->now_tick_10ms - input->sonar_last_tick_10ms) >=
	    NAV_SONAR_TIMEOUT_TICKS)
		return NAV_SAFETY_SONAR_STALE;

	if (!input->left_valid || !input->front_valid || !input->right_valid)
		return NAV_SAFETY_SONAR_INVALID;

	if (input->front_mm <= (uint16_t)NAV_FRONT_STOP_MM)
		return NAV_SAFETY_FRONT_TOO_CLOSE;

	return NAV_SAFETY_OK;
}

const char *apu_nav_safety_fault_name(apu_nav_safety_fault_t fault)
{
	/* 原项目 NavSafety_FaultName()，字符串**逐字一致**
	 * （Linux 侧 comm.py 会拿这个字符串显示） */
	switch (fault) {
	case NAV_SAFETY_OK:                 return "OK";
	case NAV_SAFETY_BAD_INPUT:          return "BAD_INPUT";
	case NAV_SAFETY_IMU_NOT_READY:      return "IMU_NOT_READY";
	case NAV_SAFETY_IMU_STALE:          return "IMU_STALE";
	case NAV_SAFETY_SONAR_NOT_READY:    return "SONAR_NOT_READY";
	case NAV_SAFETY_SONAR_STALE:        return "SONAR_STALE";
	case NAV_SAFETY_SONAR_INVALID:      return "SONAR_INVALID";
	case NAV_SAFETY_FRONT_TOO_CLOSE:    return "FRONT_TOO_CLOSE";
	default:                            return "UNKNOWN";
	}
}
