/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_link.c —— APU1 裸机侧"核间链路"层实现（**只有 RPMsg 一条出口**）
 * ===========================================================================
 *  本文件是**全工程唯一**调用 rpmsg_send() 的地方。
 *
 *  ★ 2026-09-25：原先的 APU_LINK_MODE_TEST（串口替代出口）+ `@link:test/prod`
 *    运行期切换**已整段删除**。理由见 apu_link.h 顶部：一份固件不能有两个
 *    互斥出口，否则"上电到底在跑哪条路"要靠猜 —— 那就是变量控不住。
 *    现在 apu_link_send_v() 无条件走 rpmsg_send()。
 * ===========================================================================
 */

#include "apu_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <openamp/open_amp.h>

#include "al_core.h"
#include "apu_msg.h"

/* 生产模式下 RPMsg 发送用的端点（由 apu_msg_impl 创建后登记） */
static struct rpmsg_endpoint *g_tx_ept;

/* 统计 */
static uint32_t g_tx_ok;
static uint32_t g_tx_fail;

/* ---------------------------------------------------------------------------
 * 端点登记（apu_msg_impl 在创建 endpoint 后调用，不再对外暴露）
 * --------------------------------------------------------------------------- */
void apu_link_attach_endpoint(struct rpmsg_endpoint *ept)
{
	g_tx_ept = ept;
}

int apu_link_tx_ready(void)
{
	return (g_tx_ept != AL_NULL) && (g_tx_ept->dest_addr != RPMSG_ADDR_ANY);
}

/* ---------------------------------------------------------------------------
 * 初始化
 * --------------------------------------------------------------------------- */
int apu_link_init(void)
{
	g_tx_ok = 0U;
	g_tx_fail = 0U;

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][link] link layer ready, single TX path = RPMsg "
	       "(needs Linux + remoteproc running)");
	return 0;
}

/* ===========================================================================
 * 发送（唯一的出口）
 * =========================================================================== */
int apu_link_send(uint32_t cmd, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = apu_link_send_v(cmd, fmt, args);
	va_end(args);

	return ret;
}

/* va_list 版本：给 apu_msg_impl.c 的 apu_send() 用（它自己已经有 va_list 了） */
int apu_link_send_v(uint32_t cmd, const char *fmt, va_list args)
{
	data_packet pkt;
	int n;
	int ret;

	/* 统一格式化载荷 */
	n = vsnprintf(pkt.data, MAX_DATA_LENGTH, fmt, args);

	if (n < 0)
		return -1;
	if (n >= MAX_DATA_LENGTH)
		n = MAX_DATA_LENGTH - 1;

	pkt.command = cmd;
	pkt.length = (uint16_t)n;

	/* ---- 唯一出口：真发 RPMsg ---- */
	if (g_tx_ept == AL_NULL || g_tx_ept->dest_addr == RPMSG_ADDR_ANY) {
		g_tx_fail++;
		return -1;              /* Linux 不在 / 端点没绑定 */
	}

	/* ★ 只发 6+length，不要把整个 struct（含尾部垃圾）发过去 */
	ret = rpmsg_send(g_tx_ept, &pkt, apu_pkt_wire_len(&pkt));
	if (ret < 0) {
		g_tx_fail++;
		al_printf("[APU1] rpmsg_send failed (%d)\r\n", ret);
		return -1;
	}

	g_tx_ok++;
	return 0;
}

/* ===========================================================================
 * 统计查询（调试用）
 * =========================================================================== */
void apu_link_get_stats(uint32_t *ok, uint32_t *fail)
{
	if (ok)   *ok = g_tx_ok;
	if (fail) *fail = g_tx_fail;
}
