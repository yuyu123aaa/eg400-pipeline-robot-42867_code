/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_manual.c —— 手动遥控混控实现
 * ===========================================================================
 *  逐行对照传承项目 manual_control.c（50 行）写成，**逻辑零改动**。
 *
 *  ⚠️ 注意那个**故意的不对称**（原项目就是这么写的，别"修正"）：
 *      前进时：turn_adjust = +turn * FORWARD_TURN_ADJUST_MAX / 100
 *      后退时：turn_adjust = -turn * REVERSE_TURN_ADJUST_MAX / 100
 *                          ^^^ 多了个负号
 *    倒车时方向盘反打才符合直觉，这是有意的。
 * ===========================================================================
 */

#include "apu_manual.h"

static int clamp_percent(int value)
{
	if (value > 100)
		return 100;
	if (value < -100)
		return -100;
	return value;
}

static int clamp_motor_output(int value)
{
	if (value > 100)
		return 100;
	if (value < -MANUAL_REVERSE_SPEED_MAX)
		return -MANUAL_REVERSE_SPEED_MAX;
	return value;
}

void apu_manual_mix(int throttle_percent, int turn_percent,
		    apu_manual_output_t *output)
{
	int base_speed;
	int turn_adjust;

	if (output == 0)
		return;

	throttle_percent = clamp_percent(throttle_percent);
	turn_percent = clamp_percent(turn_percent);

	if (throttle_percent >= 0) {
		base_speed = throttle_percent * MANUAL_FORWARD_SPEED_MAX / 100;
		turn_adjust = turn_percent * MANUAL_FORWARD_TURN_ADJUST_MAX / 100;
	} else {
		base_speed = throttle_percent * MANUAL_REVERSE_SPEED_MAX / 100;
		/* ★ 原项目这里的负号是**故意的**（倒车反打），照抄 */
		turn_adjust = -turn_percent * MANUAL_REVERSE_TURN_ADJUST_MAX / 100;
	}

	output->left_rear = clamp_motor_output(base_speed + turn_adjust);
	output->right_rear = clamp_motor_output(base_speed - turn_adjust);
}
