/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_sonar_filter.h —— 超声波滑动窗口中值滤波 + 跳变抑制
 * ===========================================================================
 *  【来源】传承项目 slaver_00_example.c:183-290，**零改动照抄**（纯数学，无硬件依赖）
 *
 *  【它做三件事】
 *
 *   ① 异常值替换
 *        新值 == SONAR_ERROR_VALUE(65533) 或 > 2000  ->  用上一个有效值顶替
 *        新值 < 0                                   ->  归零
 *
 *   ② 跳变抑制（防止"某一次读数突然跳很远"带偏控制）
 *        与上一个有效值相差超过 SONAR_MAX_JUMP(400mm) 时：
 *            连续跳变 < 3 次  ->  丢弃本次，仍用上一个有效值
 *            连续跳变 >= 3 次 ->  认为是真实变化，接受（jump_count 归零）
 *
 *   ③ 5 点中值滤波
 *        窗口填满后，返回窗口内 5 个值的**中位数**（冒泡排序后取 [2]）
 *        —— 中值滤波对"偶发脉冲干扰"特别有效，比平均滤波更适合超声波
 *
 *  【为什么放在 common/】
 *      纯逻辑、不碰硬件，将来能单测、能换芯片。
 * ===========================================================================
 */

#ifndef APU_SONAR_FILTER_H
#define APU_SONAR_FILTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 常量（与传承项目逐字一致，别改） ---- */
#define APU_SONAR_FILTER_WINDOW     5U
#define APU_SONAR_ERROR_VALUE       65533U   /* STM32 小板用它表示"本次测失败" */
#define APU_SONAR_MAX_JUMP          400      /* mm，超过算"跳变" */
#define APU_SONAR_DAMAGE_VALUE      65535U   /* 探头损坏 */
#define APU_SONAR_MIN_VALID_MM      20       /* 有效范围下限（>这个值才算有效） */
#define APU_SONAR_MAX_VALID_MM      2000     /* 超过这个值当无效 */

/* 探头损坏位掩码（与 apu_msg.h 的 APU_SONAR_FAULT_* 数值一致） */
#define APU_SONAR_DAMAGE_FRONT_MASK 0x01U
#define APU_SONAR_DAMAGE_LEFT_MASK  0x02U
#define APU_SONAR_DAMAGE_RIGHT_MASK 0x04U

typedef struct {
	uint16_t buffer[APU_SONAR_FILTER_WINDOW];
	uint32_t index;
	int      is_filled;
	int      last_valid;
	int      jump_count;
} apu_sonar_filter_t;

/* 复位一个滤波器（上电 / 重新开始时调） */
void apu_sonar_filter_init(apu_sonar_filter_t *f);

/* 喂一个新值，返回滤波后的值。
 * ⚠️ 窗口没填满时返回输入值本身（与传承项目行为一致）。 */
int apu_sonar_filter_apply(apu_sonar_filter_t *f, int new_value);

/* 从一个"三路原始值"算出探头损坏位掩码（0 = 全部正常）。
 * 对应传承项目的 SonarDamage_Mask()。 */
uint8_t apu_sonar_damage_mask(uint16_t front, uint16_t left, uint16_t right);

#ifdef __cplusplus
}
#endif

#endif /* APU_SONAR_FILTER_H */
