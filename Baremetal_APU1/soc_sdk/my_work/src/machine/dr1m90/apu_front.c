/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_front.c —— 前向超声波直连 UART-D（AXI_UARTLite_3）实现
 * ===========================================================================
 *  设计约束见 apu_front.h 顶部。这里只强调实现上的三个点：
 *
 *  【1】完全不阻塞
 *      触发只写 1 字节，TX FIFO 满就放弃本次（下一圈再试），不等待；
 *      收帧每圈最多读 4 字节，`apu_pluart_getc()` 本身非阻塞；
 *      超时只看毫秒计数，不 delay、不死循环。
 *      ⇒ 左/右 I2C 的 10Hz 采集/上报不会被前向拖慢。
 *
 *  【2】状态机
 *      IDLE  --(到 200ms)--> 发 0x01 --> WAIT
 *      WAIT  --(收满4字节)--> 解析 --> IDLE
 *      WAIT  --(80ms 超时)--> 标记无效 --> IDLE
 *
 *  【3】只提供“原始合法值”
 *      滤波不在本模块做：apu_sonar.c 用同一个 g_filt_front 做 5 点中值，
 *      这样前/左/右三路的滤波语义完全一致。
 * ===========================================================================
 */

#include "apu_front.h"

#include "al_core.h"
#include "al_axi_uart_lite_ll.h"

#include "apu_pluart.h"
#include "apu_time.h"

/* 状态机 */
enum {
	FRONT_ST_IDLE = 0,
	FRONT_ST_WAIT
};

static uint32_t g_base;
static int      g_inited;
static int      g_state;
static uint8_t  g_buf[APU_FRONT_FRAME_LEN];
static uint8_t  g_len;
static uint32_t g_t0_ms;
static uint32_t g_next_ms;
static uint32_t g_last_ok_ms;

static uint16_t g_mm;        /* 最近一次合法原始值（无效帧不覆盖） */
static int      g_valid;     /* 最近一次帧是否合法 */
static uint32_t g_ok_count;
static uint32_t g_err_count;

/* ---------------------------------------------------------------------------
 *  非阻塞发触发：只写 1 字节，FIFO 满就跳过
 * --------------------------------------------------------------------------- */
static int front_trigger(void)
{
	uint32_t st;

	if (g_base == 0U)
		return -1;

	st = AlAxiUartLite_GetStatus(g_base);
	if (st & BIT(AL_AXI_UART_LITE_SR_TXFIFO_FULL_SHIFT))
		return -1;

	AlAxiUartLite_SendData(g_base, APU_FRONT_TRIGGER_BYTE);
	return 0;
}

/* ---------------------------------------------------------------------------
 *  解析已收齐的 4 字节
 * --------------------------------------------------------------------------- */
static void front_parse(uint32_t now_ms)
{
	uint8_t  sum;
	uint16_t mm;

	sum = (uint8_t)((uint8_t)(g_buf[0] + g_buf[1]) + g_buf[2]);
	if (sum != g_buf[3]) {
		/* 校验失败：不覆盖 g_mm，只把 valid 拉低 */
		g_valid = 0;
		g_err_count++;
		return;
	}

	mm = (uint16_t)(((uint16_t)g_buf[1] << 8) | (uint16_t)g_buf[2]);
	g_mm = mm;

	if (mm < (uint16_t)APU_FRONT_MIN_VALID_MM ||
	    mm > (uint16_t)APU_FRONT_MAX_VALID_MM) {
		/* 65533/65535 无回波、0、超量程：都算本次无效 */
		g_valid = 0;
		g_err_count++;
		return;
	}

	g_valid       = 1;
	g_last_ok_ms  = now_ms;
	g_ok_count++;
}

/* ---------------------------------------------------------------------------
 *  初始化
 * --------------------------------------------------------------------------- */
int apu_front_init(void)
{
	g_base = apu_pluart_base(APU_PLUART_FRONT);
	if (g_base == 0U) {
		g_inited = 0;
		g_valid  = 0;
		return -1;
	}

	apu_pluart_rx_flush(g_base);

	g_inited     = 1;
	g_state      = FRONT_ST_IDLE;
	g_len        = 0U;
	g_t0_ms      = 0U;
	g_next_ms    = 0U;
	g_last_ok_ms = 0U;
	g_mm         = 0U;
	g_valid      = 0;
	g_ok_count   = 0U;
	g_err_count  = 0U;

	return 0;
}

/* ---------------------------------------------------------------------------
 *  周期轮询（非阻塞）
 * --------------------------------------------------------------------------- */
void apu_front_poll(void)
{
	uint32_t now;
	uint8_t  b;

	if (!g_inited)
		return;

	now = apu_time_ms_live();

	/* ---- IDLE：到点发触发 ---- */
	if (g_state == FRONT_ST_IDLE) {
		if (g_next_ms != 0U &&
		    (uint32_t)(now - g_next_ms) < APU_FRONT_PERIOD_MS)
			return;

		/* 清掉上一帧残渣，避免把旧字节拼进新帧 */
		apu_pluart_rx_flush(g_base);

		if (front_trigger() != 0) {
			/* TX FIFO 满/IP 异常：不等待，下一圈再试 */
			g_next_ms = now;
			return;
		}

		g_len   = 0U;
		g_t0_ms = now;
		g_state = FRONT_ST_WAIT;
		return;
	}

	/* ---- WAIT：每次最多抽 4 个字节，非阻塞 ---- */
	while (g_len < APU_FRONT_FRAME_LEN &&
	       apu_pluart_getc(g_base, &b)) {
		if (g_len == 0U && b != APU_FRONT_FRAME_HEADER)
			continue;       /* 丢非帧头，继续找 0xFF */
		g_buf[g_len++] = b;
	}

	if (g_len >= APU_FRONT_FRAME_LEN) {
		front_parse(now);
		g_state   = FRONT_ST_IDLE;
		g_next_ms = now;
		return;
	}

	/* ---- 超时：只影响本路，不阻塞 ---- */
	if ((uint32_t)(now - g_t0_ms) >= APU_FRONT_TIMEOUT_MS) {
		g_state   = FRONT_ST_IDLE;
		g_next_ms = now;
		g_valid   = 0;
		g_err_count++;
	}
}

/* ---------------------------------------------------------------------------
 *  查询
 * --------------------------------------------------------------------------- */
int apu_front_get(uint16_t *mm, int *valid)
{
	uint32_t now;
	int fresh = 0;

	if (g_inited && g_valid) {
		now = apu_time_ms_live();
		if ((uint32_t)(now - g_last_ok_ms) <= APU_FRONT_STALE_MS)
			fresh = 1;
	}

	if (mm != AL_NULL)
		*mm = g_mm;
	if (valid != AL_NULL)
		*valid = fresh;

	return fresh ? 0 : -1;
}

uint32_t apu_front_ok_count(void)
{
	return g_ok_count;
}

uint32_t apu_front_err_count(void)
{
	return g_err_count;
}
