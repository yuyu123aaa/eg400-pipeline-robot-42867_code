/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_manual.h —— 手动遥控混控
 * ===========================================================================
 *  【来源】传承项目 manual_control.h/c，**零改动照抄**（纯逻辑）。
 *
 *  把"油门 + 转向"两个百分比，混成左右两个后推进器的输出百分比。
 *  差速转向：一边加速、另一边减速。
 *
 *  【调参只改这几个宏】
 * ===========================================================================
 */

#ifndef APU_MANUAL_H
#define APU_MANUAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* 手柄遥控参数（与传承项目逐字一致，后续调参只改这里）
 *   FORWARD_SPEED_MAX  : 前进满扳机对应的直行基础速度
 *   REVERSE_SPEED_MAX  : 后退满扳机对应的直行基础速度
 *   FORWARD_TURN_ADJUST_MAX / REVERSE_TURN_ADJUST_MAX : 满转向时的单侧修正量
 */
#define MANUAL_FORWARD_SPEED_MAX         20
#define MANUAL_REVERSE_SPEED_MAX         15
#define MANUAL_FORWARD_TURN_ADJUST_MAX   10
#define MANUAL_REVERSE_TURN_ADJUST_MAX   8

typedef struct {
	int left_rear;
	int right_rear;
} apu_manual_output_t;

/* 混控：throttle_percent / turn_percent 都在 -100..+100
 * 结果写进 *output（不会超出 ±MANUAL_REVERSE_SPEED_MAX..100 的范围） */
void apu_manual_mix(int throttle_percent, int turn_percent,
		    apu_manual_output_t *output);

#ifdef __cplusplus
}
#endif

#endif /* APU_MANUAL_H */
