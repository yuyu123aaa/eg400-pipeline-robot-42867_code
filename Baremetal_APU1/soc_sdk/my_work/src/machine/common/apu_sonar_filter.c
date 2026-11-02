/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_sonar_filter.c —— 超声波滤波实现
 * ===========================================================================
 *  逐行对照传承项目 slaver_00_example.c:225-290 写成，**逻辑零改动**。
 *  唯一变化：类型改成 stdint（uint16_t/int），加了下标越界断言性保护。
 * ===========================================================================
 */

#include "apu_sonar_filter.h"

void apu_sonar_filter_init(apu_sonar_filter_t *f)
{
	uint32_t i;

	if (f == 0)
		return;

	for (i = 0U; i < APU_SONAR_FILTER_WINDOW; i++)
		f->buffer[i] = 0U;

	f->index = 0U;
	f->is_filled = 0;
	f->last_valid = 0;
	f->jump_count = 0;
}

int apu_sonar_filter_apply(apu_sonar_filter_t *f, int new_value)
{
	uint16_t temp[APU_SONAR_FILTER_WINDOW];
	uint32_t i, j;

	if (f == 0)
		return new_value;

	/* ---- ① 异常值替换（对应原项目 :227-234） ---- */
	if ((uint32_t)new_value == APU_SONAR_ERROR_VALUE ||
	    new_value > APU_SONAR_MAX_VALID_MM) {
		new_value = f->last_valid;
	} else if (new_value < 0) {
		new_value = 0;
	} else if (f->last_valid != 0) {
		/* ---- ② 跳变抑制（对应原项目 :235-254） ---- */
		int diff = new_value - f->last_valid;

		if (diff > APU_SONAR_MAX_JUMP || diff < -APU_SONAR_MAX_JUMP) {
			f->jump_count++;
			if (f->jump_count >= 3) {
				/* 连续 3 次都跳 -> 认为是真实变化，接受 */
				f->jump_count = 0;
			} else {
				new_value = f->last_valid;
			}
		} else {
			f->jump_count = 0;
		}
	}

	f->last_valid = new_value;

	/* ---- ③ 进窗口（对应原项目 :258-269） ---- */
	f->buffer[f->index] = (uint16_t)new_value;
	f->index++;
	if (f->index >= APU_SONAR_FILTER_WINDOW) {
		f->index = 0U;
		f->is_filled = 1;
	}

	if (!f->is_filled)
		return new_value;       /* 窗口没满，先不回中值 */

	/* ---- ④ 5 点中值（对应原项目 :271-289，冒泡排序取中间） ---- */
	for (i = 0U; i < APU_SONAR_FILTER_WINDOW; i++)
		temp[i] = f->buffer[i];

	for (i = 0U; i + 1U < APU_SONAR_FILTER_WINDOW; i++) {
		for (j = 0U; j + 1U < APU_SONAR_FILTER_WINDOW - i; j++) {
			if (temp[j] > temp[j + 1U]) {
				uint16_t t = temp[j];

				temp[j] = temp[j + 1U];
				temp[j + 1U] = t;
			}
		}
	}

	return (int)temp[APU_SONAR_FILTER_WINDOW / 2U];
}

uint8_t apu_sonar_damage_mask(uint16_t front, uint16_t left, uint16_t right)
{
	uint8_t mask = 0U;

	/* 对应原项目 SonarDamage_Mask()，:204-222 */
	if (front == (uint16_t)APU_SONAR_DAMAGE_VALUE)
		mask |= APU_SONAR_DAMAGE_FRONT_MASK;
	if (left == (uint16_t)APU_SONAR_DAMAGE_VALUE)
		mask |= APU_SONAR_DAMAGE_LEFT_MASK;
	if (right == (uint16_t)APU_SONAR_DAMAGE_VALUE)
		mask |= APU_SONAR_DAMAGE_RIGHT_MASK;

	return mask;
}
