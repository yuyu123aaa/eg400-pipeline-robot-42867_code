/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_front.h —— 前向超声波（直连 UART-D = AXI_UARTLite_3）
 * ===========================================================================
 *  【为什么单独一路】
 *      原来前/左/右三路都经 STM32 小板 -> I2C1。前向那一路的 32 板接口
 *      损坏后，前向探头改为直连 FPGA 的 UART-D（0x8070_0000，CEP pin17/18），
 *      左/右仍然走 32 板 I2C1。两者物理独立。
 *
 *  【协议】（与 apu_depth.c 的 L08 完全一致）
 *      115200 8N1，先发 0x01 触发，探头回 4 字节：
 *          [0]=0xFF  [1]=距离高  [2]=距离低  [3]=校验和
 *      校验：sum(buf[0..2]) & 0xFF == buf[3]
 *      距离：(buf[1] << 8) | buf[2] mm
 *
 *  【实时性约束 —— 本模块最重要的设计点】
 *      前向可以慢（默认 5Hz），但**绝不能阻塞主循环**：
 *        · 本模块是纯轮询状态机，每次 poll 最多读 4 个字节，立即返回；
 *        · 触发只写 1 字节，TX FIFO 满就跳过本次，不等待；
 *        · 左/右 I2C 仍由 apu_sonar_periodic() 按 10Hz 独立采集/上报；
 *        · 前向只把“最近一次有效原始值 + 新鲜标志”交给 apu_sonar.c，
 *          由 apu_sonar.c 用同一个 5 点中值滤波器滤波后一起打包。
 *
 *  【数据语义】
 *      apu_front_get() 返回最近一次合法原始距离（mm）和 fresh 标志。
 *      fresh=0 时 mm 仍是上一次的有效值，调用方应把 V 标志置 0，
 *      不要把旧值当新值。
 * ===========================================================================
 */

#ifndef APU_FRONT_H
#define APU_FRONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 触发周期：前向慢一点没关系，5Hz 足够；不影响 10Hz 的左/右 */
#define APU_FRONT_PERIOD_MS         200U
/* 触发后等整帧的上界；超时只影响本路，不阻塞主循环 */
#define APU_FRONT_TIMEOUT_MS        80U
/* 超过这么久没有新的合法帧，就认为前向“不新鲜”（V=0） */
#define APU_FRONT_STALE_MS          600U

#define APU_FRONT_TRIGGER_BYTE      0x01U
#define APU_FRONT_FRAME_LEN         4U
#define APU_FRONT_FRAME_HEADER      0xFFU

#define APU_FRONT_MIN_VALID_MM      20
#define APU_FRONT_MAX_VALID_MM      2000

/* 初始化：查 UART-D 基地址并清 RX FIFO。
 * 返回 0 成功；-1 = UART-D 实例不存在（bit 流不是最新含 4 路 UART 的版本） */
int apu_front_init(void);

/* 周期轮询：**非阻塞**，每圈调用一次即可。 */
void apu_front_poll(void);

/* 取最近一次结果。
 *   *mm    = 最近一次合法原始距离（mm）；无数据时为 0
 *   *valid = 1 表示该值来自最近 APU_FRONT_STALE_MS 内的合法帧
 * 返回 0 = 当前 fresh；-1 = 无数据/已过期（*mm 仍可能是上一次的值）
 * 指针可为 NULL。 */
int apu_front_get(uint16_t *mm, int *valid);

/* 统计（诊断用） */
uint32_t apu_front_ok_count(void);
uint32_t apu_front_err_count(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_FRONT_H */
