/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_msg.h —— APU0(Linux) 与 APU1(裸机) 之间的 RPMsg 消息格式
 * ===========================================================================
 *  ⚠️ 本文件定义的协议**沿用你们上一届（飞腾平台管道巡检机器人）的格式**，
 *     以保证 Linux 侧上位机 / C 代理程序不用改一行代码就能对接。
 *
 *  协议来源（原项目）：
 *      openamp_for_linux/rpmsg-demo-single.c        （Linux 侧 C 代理）
 *      openamp_for_linux/src/slaver_00_example.c    （裸机侧从核实现）
 *  两边用的是**逐字节相同**的 data_packet 结构体。
 *
 *  数据流（三跳，和原项目一致）：
 *
 *     ┌──────────────┐  RPMsg   ┌──────────────────┐  UDP    ┌──────────┐
 *     │ APU1 裸机    │ <======> │ APU0 Linux C 代理│ <=====> │ Python   │
 *     │ （本工程）   │ data_pkt │ rpmsg-demo-single│  JSON   │ 上位机   │
 *     └──────────────┘          └──────────────────┘         └──────────┘
 *              ▲                        ▲                        ▲
 *         本文件定义              原项目已实现             原项目已实现
 *
 *  所以：**只有最左边这一跳需要你们重新实现**（换成安路 DR1M90）。
 *        中间和右边那两跳，原代码可以直接复用。
 * ===========================================================================
 */

#ifndef APU_MSG_H
#define APU_MSG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 一、消息结构体（★与原项目逐字节一致，不要改）
 * ===========================================================================
 *
 *  原项目定义：
 *      typedef struct __attribute__((packed))
 *      {
 *          uint32_t command;
 *          uint16_t length;
 *          char data[MAX_DATA_LENGTH];
 *      } data_packet;
 *
 *  原项目里 MAX_DATA_LENGTH = RPMSG_BUFFER_SIZE / 2。
 *  RPMsg 缓冲区通常是 512 字节，所以 MAX_DATA_LENGTH = 256。
 *
 *  ⚠️ 发送时**不是**发 sizeof(data_packet)，而是发 6 + length：
 *         rpmsg_send(ept, &pkt, 6 + pkt.length);
 *     这一点很容易写错，写错会导致尾部填充的垃圾字节一起发过去。
 * =========================================================================== */

/* 载荷缓冲区大小。与原项目一致（RPMsg 缓冲区 512 的一半）。
 * 总包 = 6 + 256 = 262 字节，远小于 RPMsg 单条上限（约 472），安全。 */
#define MAX_DATA_LENGTH         256

/* 包头长度：command(4) + length(2) */
#define APU_PKT_HDR_LEN         6

/* 一条消息的最大字节数 */
#define APU_PKT_MAX_LEN         (APU_PKT_HDR_LEN + MAX_DATA_LENGTH)

typedef struct __attribute__((packed)) {
	uint32_t command;                /* 见下面的 DEVICE_CORE_* */
	uint16_t length;                 /* data 里有效字节数（不含结尾的 '\0'） */
	char     data[MAX_DATA_LENGTH];  /* 文本载荷，以 '\0' 结尾 */
} data_packet;

/* ===========================================================================
 * 二、命令码（★与原项目一致）
 * =========================================================================== */
#define DEVICE_CORE_START       0x0001U   /* 启动（原项目用于内核 start 语义） */
#define DEVICE_CORE_STOP        0x0002U   /* 停止 */
#define DEVICE_CORE_CHECK       0x0003U   /* 文本命令 / 命令回执（最常用） */
#define DEVICE_CORE_SENSOR      0x0004U   /* 传感器数据上报 */
#define DEVICE_CORE_HEARTBEAT   0x0005U   /* 心跳 PING/PONG */

/* ===========================================================================
 * 三、载荷文本格式（★与原项目一致，逐字照抄）
 * ===========================================================================
 *
 *  【APU0(Linux) -> APU1(裸机)】
 *
 *    command = DEVICE_CORE_CHECK，data 为下列文本之一：
 *
 *      "CMD:WDT_TEST"          看门狗测试
 *      "CMD:MANUAL_START"      进入手动遥控模式
 *      "CMD:START"             开始
 *      "CMD:STOP"              停止
 *      "CMD:MOTOR_START"       启动电机
 *      "CMD:MOTOR_STOP"        停止电机
 *      "CMD:CALIBRATE"         校准
 *      "CMD:LED1ON" / "CMD:LED1OFF"
 *      "CMD:LED2ON" / "CMD:LED2OFF"
 *      "SPEED:<油门百分比>,<转向百分比>"     例: "SPEED:30,-15"
 *
 *    command = DEVICE_CORE_HEARTBEAT
 *      "PING:<序号>"           例: "PING:1"
 *
 *  【APU1(裸机) -> APU0(Linux)】
 *
 *    command = DEVICE_CORE_CHECK，data 为回执：
 *      "ACK: ..."              例: "ACK: LED1 已开启"
 *                                    "ACK: REJECTED, SONAR DAMAGED"
 *                                    "ACK: REJECTED, HOST HEARTBEAT LOST"
 *                                    "ACK: 10HZ CENTER CONTROL STARTED, ..."
 *
 *    command = DEVICE_CORE_SENSOR
 *      "R:<roll>,P:<pitch>,Y:<yaw>"     例: "R:1.23,P:-0.45,Y:178.90"
 *      "SONAR:<前>,<左>,<右>"           例: "SONAR:120,45,60"
 *      "FAULT:SONAR:<位掩码>"           例: "FAULT:SONAR:3"
 *      "RECOVER:SONAR"                  超声波恢复正常
 *      "FAULT:NAV:<描述>"               例: "FAULT:NAV:NAV SAFETY OBSTACLE"
 *
 *    command = DEVICE_CORE_HEARTBEAT
 *      "PONG:<序号>"           例: "PONG:1"   （必须原样回序号）
 *
 *  ⚠️ 注意 Linux 侧解析时用的是 sscanf + strcmp，所以：
 *     · 大小写必须完全一致
 *     · 逗号、冒号必须是**半角**
 *     · 不要加多余空格（"ACK: REJECTED" 里的那一个空格要保留）
 * =========================================================================== */

/* 心跳参数（★与原项目一致） */
#define APU_HEARTBEAT_PING_PREFIX   "PING:"
#define APU_HEARTBEAT_PONG_PREFIX   "PONG:"

/* Linux 侧心跳：每 500ms 一次；连续 5 次没回（2.5s）判定裸机故障。 */
#define APU_HEARTBEAT_PERIOD_MS         500
#define APU_HEARTBEAT_MAX_MISSES        5
/* 裸机侧反向心跳监控：启动宽限 7.5s，之后 2.5s 没收到 Linux 心跳就刹车。 */
#define APU_HB_STARTUP_TIMEOUT_MS       7500
#define APU_HB_TIMEOUT_MS               2500

/* 超声波故障位掩码（与原项目 comm.py 的 SONAR_FAULT_CHANNELS 对应） */
#define APU_SONAR_FAULT_FRONT   0x01
#define APU_SONAR_FAULT_LEFT    0x02
#define APU_SONAR_FAULT_RIGHT   0x04

/* ===========================================================================
 * 四、几个好用的辅助函数（两端都能用，保持"6+length"发送语义）
 * =========================================================================== */

/* 填充一个包：设置 command 和 length（自动截断并在末尾补 '\0'） */
static inline void apu_pkt_set(data_packet *p, uint32_t cmd,
			       const char *fmt_text)
{
	uint32_t n = 0;

	p->command = cmd;
	if (fmt_text) {
		while (fmt_text[n] && n < (MAX_DATA_LENGTH - 1)) {
			p->data[n] = fmt_text[n];
			n++;
		}
	}
	p->data[n] = '\0';
	p->length = (uint16_t)n;
}

/* 取实际要发给 rpmsg_send 的字节数 */
static inline uint32_t apu_pkt_wire_len(const data_packet *p)
{
	return (uint32_t)APU_PKT_HDR_LEN + (uint32_t)p->length;
}

/* 从收到的原始缓冲区解析（对照原项目 parse_protocol_data）
 * 返回 0 成功；-1 太短；-2 length 越界；-3 数据不完整 */
static inline int apu_pkt_parse(const void *buf, uint32_t buf_len,
				data_packet *out)
{
	const uint8_t *p = (const uint8_t *)buf;

	if (buf_len < APU_PKT_HDR_LEN)
		return -1;

	out->command = (uint32_t)p[0]
		     | ((uint32_t)p[1] << 8)
		     | ((uint32_t)p[2] << 16)
		     | ((uint32_t)p[3] << 24);
	out->length  = (uint16_t)((uint16_t)p[4] | ((uint16_t)p[5] << 8));

	if (out->length >= MAX_DATA_LENGTH)
		return -2;
	if (buf_len < (uint32_t)APU_PKT_HDR_LEN + out->length)
		return -3;

	{
		uint32_t i;
		for (i = 0; i < out->length; i++)
			out->data[i] = (char)p[APU_PKT_HDR_LEN + i];
		out->data[out->length] = '\0';
	}
	return 0;
}

/* 判断 data 是否以 prefix 开头（等价原项目的 strncmp 用法） */
static inline int apu_str_prefix(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++)
			return 0;
	}
	return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* APU_MSG_H */
