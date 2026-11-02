/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_sonar.h —— APU1 裸机侧超声波采集（I2C1 读 STM32 小板）
 * ===========================================================================
 *  【硬件】
 *      STM32 超声波小板挂在 **I2C1**（apu_i2c 的 BUS1），从机地址 **0x40**
 *      一次读 6 字节（寄存器 0x10 起），**大端**，顺序：左 / 前 / 右（单位 mm）
 *
 *  【参考来源】传承项目 sonar.c（103 行）—— 逻辑照抄，只把 I2C 调用换成 apu_i2c_*
 *
 *  【为什么不改 STM32 小板固件】（已定决策 2）
 *      实物那块板**不会发 65535**（探头损坏值），所以 FAULT:SONAR 暂时
 *      不会被真实硬件触发。测试模式可以用 `@sonar:65535,0,0` 造出来验。
 *      —— 靠"读到 0mm -> 前方过近"作为兜底安全网。
 * ===========================================================================
 */

#ifndef APU_SONAR_H
#define APU_SONAR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 原始三路读数（对应传承项目 SonarData_t） */
typedef struct {
	uint16_t left;
	uint16_t front;
	uint16_t right;
} apu_sonar_raw_t;

/* 一次采集的结果 */
typedef struct {
	apu_sonar_raw_t raw;        /* 硬件原始值（已做 front/right 交换） */
	apu_sonar_raw_t filtered;   /* 滤波后的值（对外上报用这个） */
	uint8_t  damage_mask;       /* 探头损坏位掩码，0 = 正常 */
	int      left_valid;        /* 各路是否在有效范围内 */
	int      front_valid;
	int      right_valid;
	int      read_ok;           /* I2C 读成功 = 1 */
} apu_sonar_sample_t;

/* 初始化（其实就是确保 I2C1 已就绪；总线由 apu_i2c_init() 建好） */
int apu_sonar_init(void);

/* 读一次原始三路距离。
 * 返回：0 成功（raw 已填）；<0 读失败（I2C 错误码，见 apu_i2c.h） */
int apu_sonar_read_raw(apu_sonar_raw_t *out);

/* ---------------------------------------------------------------------------
 * ★ 采集 + 滤波 + 上报（业务层周期调用，10Hz）
 * ---------------------------------------------------------------------------
 *  这是"超声波这一路"的完整处理，包含传承项目里的：
 *      · front/right 交换（硬件安装方向导致，**故意的**，照抄）
 *      · 损坏判定（放在滤波**之前** —— 原项目 :1023 就是这个顺序）
 *      · 有效性判定 NavSafety_IsSonarValueValid（20..2000mm）
 *      · 三路各自中值滤波
 *      · 边沿触发的 FAULT:SONAR / RECOVER:SONAR 上报
 *      · 10Hz 的 "SONAR:前,左,右,V:前左右" 上报
 *
 *  返回：1 = 本轮做了采集；0 = 还没到周期（10Hz）或读失败
 * --------------------------------------------------------------------------- */
int apu_sonar_periodic(void);

/* 复位滤波器（重新标定/上电时调） */
void apu_sonar_reset_filters(void);

/* 最近一次滤波后的三路值（供居中控制 P8 使用） */
void apu_sonar_get_filtered(apu_sonar_raw_t *out);

/* 统计 */
uint32_t apu_sonar_read_ok_count(void);
uint32_t apu_sonar_read_err_count(void);

/* ---------------------------------------------------------------------------
 * ★ 连续失败熔断状态
 * ---------------------------------------------------------------------------
 *  为什么需要：SDK 的 I2C 轮询读结尾是一个**没有超时的忙等**
 *      while (AlIic_ll_IsMasterActivity(BaseAddr));
 *  总线卡死时它会永远转下去。熔断能在"连续失败"后跳过 I2C 读取，
 *  避免在坏总线/坏从机上反复消耗时间。
 *  ⚠️ 它**拦不住"某一次读本身就把总线卡死"** —— 那种情况靠硬件看门狗兜底。
 * --------------------------------------------------------------------------- */

/* 1 = 已熔断（正在跳过读取）。熔断后每 5 秒自动重试一次，成功即恢复。 */
int      apu_sonar_breaker_open(void);

/* 熔断触发过几次 */
uint32_t apu_sonar_breaker_tripped_count(void);

/* 当前连续失败次数 */
uint32_t apu_sonar_consecutive_fails(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_SONAR_H */
