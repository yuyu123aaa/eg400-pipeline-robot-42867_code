/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_i2c.h —— APU1 裸机侧 I2C 总线抽象（双总线）
 * ===========================================================================
 *  【为什么是"双总线"】
 *
 *  PCA9685（推进器 PWM）和 STM32 超声波小板**地址都是 0x40**。
 *  同一条总线上不能有两个 0x40 —— 所以用两条**独立的 PS I2C 控制器**：
 *
 *      BUS0 = PS I2C0 (DevId 0, 0xF8414000) -> PL IO -> CEP 3/4  (Y12/Y13) -> PCA9685
 *      BUS1 = PS I2C1 (DevId 1, 0xF8415000) -> PL IO -> CEP 5/6  (T9/U10)  -> STM32 超声板
 *
 *  两条总线各自独立，地址重叠**天然不冲突**，不用改 PCA9685 的跳线。
 *
 *  【分层】
 *  这是驱动层（dr1m90/），业务层和上层驱动（sonar/pca9685）只调本文件的
 *  apu_i2c_write_regs() / apu_i2c_read_regs()，**不直接碰 AL SDK 的 IIC API**。
 *
 *  【API 选择说明 —— 只用 Polling 版】
 *  SDK 的 AlIic_Hal_*Block(..., Timeout) 里的 **Timeout 在裸机下被丢弃**
 *  （al_osal.h 里用 AL_UNUSED），I2C 挂了可能**死等**。
 *  所以本文件只用 Polling 版，并自己用寄存器判 NACK。
 * ===========================================================================
 */

#ifndef APU_I2C_H
#define APU_I2C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 两条总线
 * --------------------------------------------------------------------------- */
typedef enum {
	APU_I2C_BUS0 = 0,   /* PS I2C0 -> PCA9685   (0x40) */
	APU_I2C_BUS1 = 1    /* PS I2C1 -> 超声小板  (0x40) */
} apu_i2c_bus_t;

#define APU_I2C_NUM_BUS         2

/* 总线上的设备地址（与传承项目一致） */
#define APU_I2C_ADDR_PCA9685    0x40U   /* 参考 pca9685.h:8  */
#define APU_I2C_ADDR_STM32_SONAR 0x40U  /* 参考 sonar.h:9    */

/* 标准模式 100kHz（PCA9685 标准模式；STM32 小板 iic_slave.h 也是 100000） */
#define APU_I2C_RATE_HZ         100000U

/* ---------------------------------------------------------------------------
 * 初始化 / 复位
 * --------------------------------------------------------------------------- */

/* 初始化两条 I2C 总线（主模式 / 7 位地址 / 100kHz）。
 * 返回：0 成功；<0 表示有几条总线初始化失败（返回 -失败条数）。
 *
 * 位置：apu_app_init() 里调用。 */
int apu_i2c_init(void);

/* 软复位一条总线并清错误状态。
 * 什么时候用：某次传输失败后，总线可能停在异常状态，需要重来。 */
int apu_i2c_bus_reset(apu_i2c_bus_t bus);

/* ---------------------------------------------------------------------------
 * 读写
 * ---------------------------------------------------------------------------
 *  寄存器语义（和传承项目一致）：
 *      写 = [reg][data0][data1]...  一次发出（STOP 结尾）
 *      读 = [reg]（RESTART，不发 STOP）然后读 len 字节（STOP 结尾）
 *
 *  返回：0 成功；-1 参数错；-2 总线没初始化；-3 设备无应答(NACK)；
 *        -4 传输被中止（其它原因）
 * --------------------------------------------------------------------------- */

int apu_i2c_write_regs(apu_i2c_bus_t bus, uint8_t addr, uint8_t reg,
		       const uint8_t *buf, uint32_t len);

int apu_i2c_read_regs(apu_i2c_bus_t bus, uint8_t addr, uint8_t reg,
		      uint8_t *buf, uint32_t len);

/* 便捷版：写/读单个 8 位寄存器 */
int apu_i2c_write_reg(apu_i2c_bus_t bus, uint8_t addr, uint8_t reg, uint8_t val);
int apu_i2c_read_reg(apu_i2c_bus_t bus, uint8_t addr, uint8_t reg, uint8_t *val);

/* ---------------------------------------------------------------------------
 * 总线扫描（★ 接任何 I2C 设备前的必过关卡）
 * ---------------------------------------------------------------------------
 *  遍历 0x08..0x77，打印有应答的地址。
 *  返回：找到的设备个数；<0 参数错。
 * --------------------------------------------------------------------------- */
int apu_i2c_scan(apu_i2c_bus_t bus);

/* ---------------------------------------------------------------------------
 * 诊断
 * --------------------------------------------------------------------------- */

/* 最近一次 NACK 时 IC_TX_ABRT_SOURCE 的原始值（排查接线问题时很有用） */
uint32_t apu_i2c_last_abort_source(apu_i2c_bus_t bus);

/* 打印总线上报的原始 abort 值（含每一位的含义） */
void apu_i2c_dump_last_abort(apu_i2c_bus_t bus);

#ifdef __cplusplus
}
#endif

#endif /* APU_I2C_H */
