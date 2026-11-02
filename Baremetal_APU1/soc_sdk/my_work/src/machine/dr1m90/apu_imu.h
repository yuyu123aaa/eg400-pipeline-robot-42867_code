/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_imu.h —— APU1 裸机侧姿态接收（UART0 收行解析）
 * ===========================================================================
 *  【硬件】
*      STM32 姿态小板挂在 **PS UART0**（DevId 0，0xF8400000），**115200** 8N1
*      ★ 2026-09-20：与姿态板 USART1（Main.c:51 Usart1Init(115200)）对齐；
*        9600 是姿态板 USART2 接维特传感器那一侧的值，不是这一路。
 *      引脚：PL IO -> CEP pin7/pin8 (T5/U5)，**TX/RX 交叉接**
 *
 *  【协议】STM32 那侧每周期打一行：
 *          angle:<roll> <pitch> <yaw>\r\n
 *          例：angle:1.23 -0.45 178.90
 *      来源：传承项目 slaver_00_example.c:940-1002 的 Uart2_Proc()
 *
 *  【两个必须自己写的理由】
 *      ① SDK 的 UART **没有环形缓冲**（只有 16 字节硬件 FIFO）
 *         -> 会丢数据，必须自己写 ring buffer
 *      ② FRpmsgEchoApp 里没有现成的收行工具
 *
 *  ⚠️ 时间单位：陈旧检测用 **10ms tick**（NAV_IMU_TIMEOUT_TICKS=100 -> 1.0s），
 *     直接套 apu_tick_10ms()，常量不用改。
 * ===========================================================================
 */

#ifndef APU_IMU_H
#define APU_IMU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 接收环形缓冲大小（必须是 2 的幂，才能用 & 取模） */
#define APU_IMU_RB_SIZE         512U
/* 单行最大长度（"angle:1.23 -0.45 178.90" 才 23 字符，留足余量） */
#define APU_IMU_LINE_MAX        96U

/* ★ 2026-09-20：每收满这么多行**有效**数据上报一次
 *   —— 逐字对应传承 slaver_00_example.c:993 的 "if (uart_count < 10) continue;"。
 *   ⚠️ 上报率 = 行率 / 这个数。如果实测行率太低导致上报太慢，就把它调小。
 *   （从 apu_imu.c 挪到这里：它是个可调项，而且 @imu:loop 的提示要打出真实行数） */
#define APU_IMU_REPORT_DIV      10U

/* 初始化 UART0 + 中断接收。返回 0 成功，<0 失败。 */
int apu_imu_init(void);

/* 每轮主循环调一次：从环形缓冲取字节攒行，攒满一行就解析。
 * 返回：本次解析成功的行数（0 = 没有新行或都没解析成功）。 */
int apu_imu_poll(void);

/* ---------------------------------------------------------------------------
 * 查询
 * --------------------------------------------------------------------------- */

/* 最近一次有效的姿态角（度） */
void apu_imu_get_angles(float *roll, float *pitch, float *yaw);

/* 是否收到过合法数据 */
int apu_imu_seen(void);

/* 最近一次有效数据的 tick（10ms 单位），供 NavSafety 判陈旧 */
uint32_t apu_imu_last_tick(void);

/* 数据是否陈旧（超过 NAV_IMU_TIMEOUT_TICKS 没新数据） */
int apu_imu_is_stale(void);

/* 统计（排错用） */
uint32_t apu_imu_rx_bytes(void);
uint32_t apu_imu_lines_ok(void);
uint32_t apu_imu_lines_bad(void);
uint32_t apu_imu_overflow(void);

/* ---------------------------------------------------------------------------
 * ★ 2026-09-20 新增：**UART0 自检**接口
 *   起因：板子一直收不到 IMU 数据，必须先把**安路这一侧**单独摘出来验，
 *         否则"到底是安路 UART0 的问题，还是外设/接线的问题"分不清。
 *     · @imu:loop —— UART0 TX(Cep pin7/T5) 发一行 angle: 文本；
 *                    T5/U5 杜邦线短接即可"自收自发"，验证
 *                    收发 + 中断 + 环形缓冲 + 攒行解析 + 上报 整条链。
 *     · @imu:raw  —— 把线上**真实收到的原始字节**分类 + hex 打出来，
 *                    判断是噪声 / 二进制协议 / 波特率错配。
 * --------------------------------------------------------------------------- */

/* 直接在 UART0 TX 上发一串以 0 结尾的字节。返回发出的字节数，<0 = 失败 */
int apu_imu_tx_raw(const char *s);

/* ★ 2026-09-20：回环自检 —— 连发 APU_IMU_REPORT_DIV 行标准 angle: 文本，
 *   这样**一次命令就能凑满一个上报周期**，@rep:on 时能直接看到 SENSOR 帧。
 *   返回总发出字节数，<0 = 失败。 */
int apu_imu_selftest_loopback(void);

/* 收到字节的分类计数（可打印 ASCII / CR-LF / 其它）。在**所有过滤之前**统计 */
void apu_imu_byte_classes(uint32_t *printable, uint32_t *crlf, uint32_t *other);

/* 最近收到的原始字节快照（按到达顺序）。返回实际长度（<= max_len）*/
uint32_t apu_imu_raw_snapshot(uint8_t *buf, uint32_t max_len);

/* 复位（清环形缓冲和行缓冲） */
void apu_imu_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_IMU_H */
