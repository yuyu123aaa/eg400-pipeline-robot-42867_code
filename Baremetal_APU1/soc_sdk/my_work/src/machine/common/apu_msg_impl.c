/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_msg_impl.c —— 消息协议在 APU1（裸机）侧的**传输层**实现
 * ===========================================================================
 *  【分层位置】
 *
 *  本文件（传输层）      只认识"包"：编解码、收发、心跳应答、分发、统计
 *  apu_app.c（业务层）   只认识业务：命令分支、安全闸门、周期任务、状态
 *  apu_link.c（链路层）  全工程唯一调 rpmsg_send 的地方，
 *                        可在"真 RPMsg"与"串口测试替代"之间切换
 *
 *  【本文件对外提供的接口】见 apu_msg_api.h
 *
 *  【P1-b 改动】发送不再直接调 rpmsg_send，而是走 apu_link_send()。
 *  这样独立测试模式（不接 Linux）也能把消息打到串口上，
 *  而业务层一行都不用改。
 * ===========================================================================
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <openamp/open_amp.h>

#include "al_core.h"

#include "apu_msg.h"
#include "apu_msg_api.h"      /* 本文件就是它的实现，include 它可保证签名一致 */
#include "apu_app.h"          /* 业务层入口 */
#include "apu_link.h"         /* ★ 链路层：唯一出口 */
#include "apu_time.h"

/* ===========================================================================
 * 周期上报开关（★ 2026-09-25 简化）
 * ===========================================================================
 *  【以前】这个是"独立测试模式专用静音开关"：TEST 模式默认静音（否则 10Hz 的
 *          SENSOR 帧 + 1Hz 的 tick 行会把串口刷满，命令都敲不进去），
 *          PROD 模式强制开启。
 *  【现在】串口替代出口已删除（见 apu_link.h 顶部），**默认就是开启**：
 *          Linux 侧就靠这些上报工作，绝不能因为忘了敲 @rep:on 而断掉。
 *
 *  保留 apu_msg_set_periodic_report() 的理由：控制台排障时想临时静音
 *  （敲 @rep:off），好让 ACK 不被 10Hz 上报埋掉。复位后自动回到"开启"，
 *  **故意不做持久化**，免得谁静音完忘了开回来。
 *
 *  静音只影响**周期性**输出（姿态帧、超声帧、1Hz tick 行）；
 *  事件型消息（ACK / PONG / FAULT:SONAR / RECOVER:SONAR / FAULT:NAV）
 *  一律照发 —— 那些是验收判据，不能静音。
 * =========================================================================== */
static volatile int g_periodic_report_on = 1;   /* ★ 上电默认开启 */

void apu_msg_set_periodic_report(int on)
{
	g_periodic_report_on = (on != 0) ? 1 : 0;
}

int apu_msg_periodic_report_enabled(void)
{
	return g_periodic_report_on;
}


/* 端点：由 main.c 在创建后通过 apu_set_endpoint() 传进来 */
static struct rpmsg_endpoint *g_ept;

void apu_set_endpoint(struct rpmsg_endpoint *ept)
{
	g_ept = ept;
	/* ★ 同时登记给链路层 —— 它是全工程唯一真正调 rpmsg_send 的地方 */
	apu_link_attach_endpoint(ept);
}

struct rpmsg_endpoint *apu_get_endpoint(void)
{
	return g_ept;
}

/* ---------------------------------------------------------------------------
 * 统计（纯传输层指标，调试用）
 * --------------------------------------------------------------------------- */
static uint32_t g_rx_count;
static uint32_t g_tx_count;
static uint32_t g_err_count;
static uint32_t g_hb_rx_count;      /* 收到 Linux 心跳次数 */
static uint32_t g_hb_tx_count;      /* 回 PONG 次数 */

/* ★ 真停机请求。DEVICE_CORE_STOP(0x0002) 收到后置 1。
 *   main.c 主循环看到 apu_shutdown_requested() 就 break。
 *   ⚠️ 别和 "CMD:STOP" 搞混：
 *      DEVICE_CORE_STOP (0x0002) = 关整个裸机核（不看 data 内容）
 *      "CMD:STOP"（command=0x0003 的文本命令） = 紧急停车（核继续活着） */
static volatile int g_shutdown_req;

void apu_request_shutdown(void)
{
	g_shutdown_req = 1;
}

int apu_shutdown_requested(void)
{
	return g_shutdown_req;
}

/* ---------------------------------------------------------------------------
 * 发送：全部转给链路层（格式串直接透传，不经过中间缓冲）
 * --------------------------------------------------------------------------- */
int apu_send(struct rpmsg_endpoint *ept, uint32_t cmd, const char *fmt, ...)
{
	va_list args;
	int ret;

	/* 保留 ept 参数是为了兼容旧调用点；真正用哪个端点由链路层决定 */
	(void)ept;

	va_start(args, fmt);
	ret = apu_link_send_v(cmd, fmt, args);   /* ★ 传 va_list 必须走 _v 版本 */
	va_end(args);

	if (ret == 0)
		g_tx_count++;
	else
		g_err_count++;

	return ret;
}

/* 回执（原项目用 "ACK: " 前缀）
 * ⚠️ 前缀里的那个空格**必须保留**：Linux 侧用 apu_str_prefix(pkt.data, "ACK:") 判断。
 *    文案由业务层（apu_app.c）决定，必须与传承项目逐字一致。 */
int apu_send_ack(const char *fmt, ...)
{
	char buf[MAX_DATA_LENGTH];
	va_list args;
	int n;

	va_start(args, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	(void)n;

	return apu_send(g_ept, DEVICE_CORE_CHECK, "ACK: %s", buf);
}

/* 传感器上报（原项目用 DEVICE_CORE_SENSOR + "R:/SONAR:/FAULT:" 前缀） */
int apu_send_sensor(const char *fmt, ...)
{
	char buf[MAX_DATA_LENGTH];
	va_list args;
	int n;

	va_start(args, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	(void)n;

	return apu_send(g_ept, DEVICE_CORE_SENSOR, "%s", buf);
}

/* 姿态上报
 * ⚠️ 格式必须严格是 "R:%.2f,P:%.2f,Y:%.2f" ——
 *    Linux 侧是按 "R:<roll>,P:<pitch>,Y:<yaw>" 逐字段解析的。 */
int apu_send_attitude(float roll, float pitch, float yaw)
{
	return apu_send_sensor("R:%.2f,P:%.2f,Y:%.2f", roll, pitch, yaw);
}

/* 超声波距离上报
 * ⚠️ 传承项目的格式带有效性后缀：SONAR:%d,%d,%d,V:%d%d%d
 *    （slaver_00_example.c:1078-1087）。P4 起用 apu_send_sonar_full()。 */
int apu_send_sonar(int front, int left, int right)
{
	return apu_send_sensor("SONAR:%d,%d,%d", front, left, right);
}

/* ★ 完整版：前,左,右 + 三路有效性(0/1)。与传承项目逐字一致。 */
int apu_send_sonar_full(int front, int left, int right,
			int front_valid, int left_valid, int right_valid)
{
	return apu_send_sensor("SONAR:%d,%d,%d,V:%d%d%d",
			       front, left, right,
			       front_valid ? 1 : 0,
			       left_valid ? 1 : 0,
			       right_valid ? 1 : 0);
}

/* 超声波故障上报（位掩码见 APU_SONAR_FAULT_*） */
int apu_send_sonar_fault(unsigned int mask)
{
	return apu_send_sensor("FAULT:SONAR:%u", mask);
}

/* 超声波恢复正常 */
int apu_send_sonar_recover(void)
{
	return apu_send_sensor("RECOVER:SONAR");
}

/* 导航故障上报 */
int apu_send_nav_fault(const char *desc)
{
	return apu_send_sensor("FAULT:NAV:%s", desc);
}

/* ★ 完整版：故障码 + 故障名。与传承项目逐字一致（slaver:1100-1105） */
int apu_send_nav_fault_fmt(int fault_code, const char *fault_name)
{
	return apu_send_sensor("FAULT:NAV:%d,%s", fault_code,
			       (fault_name != AL_NULL) ? fault_name : "UNKNOWN");
}

/* ★★ 环境传感器上报（下潜深度 + 浊度 + 四路气体）★★
 * 格式、单位、有效性语义见 apu_msg_api.h 的声明处。实现要点：
 *   · 一次 apu_send_sensor() 发完全部字段 —— 少占 rpmsg 带宽，
 *     也避免"半套数据"（只到深度没到气体）让 Web 显示错位。
 *   · 用 %d 不用 %f：ASCII 载荷里整数最省字节，且无浮点格式化风险。
 *   · O2 传的是 **0.1%VOL 单位**（209 = 20.9%VOL），**这里不做除法**，
 *     由 Web 侧 ÷10 显示，避免两边各除一次。
 *   · 无效值由调用方（apu_env.c）已经归一成哨兵 0，这里原样发。 */
int apu_send_env(int depth_mm, int turb_unt,
		 int co, int h2s, int o2, int ch4)
{
	return apu_send_sensor("ENV:DEPTH:%d,TURB:%d,GAS:%d,%d,%d,%d",
			       depth_mm, turb_unt, co, h2s, o2, ch4);
}

/* ---------------------------------------------------------------------------
 * 【主入口】rpmsg_endpoint_cb 里调用这个
 * ---------------------------------------------------------------------------
 *  只做三件事：解析包 -> 按 command 分类 -> 把"文本命令"转交给业务层。
 * --------------------------------------------------------------------------- */
int apu_dispatch(struct rpmsg_endpoint *ept, void *data, size_t len)
{
	data_packet pkt;
	int ret;

	g_rx_count++;
	apu_set_endpoint(ept);

	ret = apu_pkt_parse(data, (uint32_t)len, &pkt);
	if (ret) {
		g_err_count++;
		al_printf("[APU1] bad packet (err=%d, len=%u)\r\n", ret,
			  (unsigned int)len);
		return ret;
	}

	switch (pkt.command) {

	/* ---- 心跳：裸机回 PONG，同时喂"主机心跳看门狗" ---- */
	case DEVICE_CORE_HEARTBEAT: {
		unsigned int seq = 0;

		g_hb_rx_count++;

		/* ★ 通知业务层：主机还活着（业务层负责 7.5s 宽限 / 2.5s 超时判断） */
		(void)apu_app_note_host_heartbeat();

		if (sscanf(pkt.data, APU_HEARTBEAT_PING_PREFIX "%u", &seq) == 1) {
			/* 原样回序号。
			 * ⚠️ Linux 用 sscanf("PONG:%u%c")==1 严格校验，
			 *    "PONG:" 后面**只能跟纯数字**，多一个字符就判失败。 */
			apu_send(ept, DEVICE_CORE_HEARTBEAT,
				 APU_HEARTBEAT_PONG_PREFIX "%u", seq);
			g_hb_tx_count++;
		} else {
			al_printf("[APU1] bad heartbeat: %s\r\n", pkt.data);
		}
		return 0;
	}

	/* ---- 文本命令：全部交给业务层 ---- */
	case DEVICE_CORE_CHECK:
		return apu_app_handle_command(pkt.data);

	/* ---- 关核：先做安全动作，再置标志让 main.c 退出主循环 ---- */
	case DEVICE_CORE_STOP:
		al_printf("[APU1] DEVICE_CORE_STOP received -> shutting down\r\n");
		apu_app_on_shutdown();      /* 业务层：停电机 + 关灯 */
		apu_request_shutdown();     /* 传输层：置标志 */
		return 0;

	/* ---- 其它 ---- */
	default:
		al_printf("[APU1] unhandled cmd 0x%04X: %s\r\n",
			  (unsigned int)pkt.command, pkt.data);
		return 0;
	}
}

/* ---------------------------------------------------------------------------
 * 周期任务：由 main.c 的主循环每次 poll 之后调用
 * ---------------------------------------------------------------------------
 *  ★ 传输层自己的周期动作（当前没有）。
 *     真正的传感器上报、外设轮询、控制流水线全部在 apu_app_periodic() 里。
 *     保留这个钩子是为了以后加"链路统计上报"之类的功能。
 * --------------------------------------------------------------------------- */
void apu_periodic(void)
{
	/* 空钩子 */
}

/* ---------------------------------------------------------------------------
 * 统计查询（调试用）
 * --------------------------------------------------------------------------- */
void apu_get_stats(uint32_t *rx, uint32_t *tx, uint32_t *err,
		   uint32_t *hb_rx, uint32_t *hb_tx)
{
	if (rx)    *rx = g_rx_count;
	if (tx)    *tx = g_tx_count;
	if (err)   *err = g_err_count;
	if (hb_rx) *hb_rx = g_hb_rx_count;
	if (hb_tx) *hb_tx = g_hb_tx_count;
}

/* ===========================================================================
 * 接入说明
 * ===========================================================================
 * 生产模式（和 Linux 联调）主循环：
 *        platform_poll(priv);     // 收包（回调里跑 apu_dispatch）
 *        apu_time_poll();
 *        apu_periodic();
 *        apu_app_periodic();
 *
 * 独立测试模式（不接 Linux）主循环：
 *        apu_time_poll();
 *        apu_periodic();
 *        apu_app_periodic();
 *        apu_testlink_poll();     // 从串口取命令 -> apu_app_handle_command()
 * =========================================================================== */
