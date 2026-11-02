/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_imu.c —— APU1 裸机侧姿态接收实现
 * ===========================================================================
 *  【收数据的方式：单字节中断 + 自己的环形缓冲】
 *
 *      ① 调一次 AlUart_Hal_RecvData(uart, &b, 1)
 *         —— 登记 1 字节接收缓冲并打开 RX 中断；FIFO 里已有数据会立刻回调
 *      ② ISR 填满缓冲 -> 回调我们
 *      ③ 回调把字节塞进环形缓冲，然后**重新 arm 下一字节**
 *      ④ 主循环 apu_imu_poll() 从环形缓冲攒行并解析
 *
 *  ⚠️ 三个坑（和 apu_console.c 同源）：
 *      1. 回调里**必须**重新 arm，否则只收一个字节就停了
 *      2. arm 前要确认 RX 不忙，否则断言 AL_UART_ERR_BUSY
 *      3. SDK **没有环形缓冲**，必须自己写
 * ===========================================================================
 */

#include "apu_imu.h"

#include <stdio.h>
#include <string.h>

#include "al_core.h"
#include "al_uart_hal.h"
#include "al_uart_dev.h"
#include "al_uart_ll.h"       /* AlUart_ll_* FIFO/寄存器操作（自接管中断用） */
#include "al_intr.h"          /* AlIntr_RegHandler */

#include "apu_app.h"
#include "apu_nav_safety.h"
#include "apu_pipe_center.h"
#include "apu_time.h"

/* UART0：DevId = 0（平台配置里 UART0PS_ENABLE 已开） */
#define APU_IMU_UART_DEV_ID     0U
/* ★★ 2026-09-20：改回 115200 ★★
 *  依据（传承项目源码，逐条可查）：
 *    · 姿态板往外的 angle: 文本走的是它的 **USART1** ——
 *      见 姿态传感器\\Source\\APP\\Main.c:51  Usart1Init(115200)
 *      以及 Source\\Drive\\UART1.c:51-56 的 fputc() 直接写 USART1；
 *      而且 USART1 的波特率**全程不会被改**（AutoScanSensor 只动 USART2）。
 *    · 9600 那个值是姿态板 **USART2**（接维特传感器）那一侧的，
 *      不是给我们看的那一路 —— 之前是被 @imu:raw 抓到的 USART2 侧数值带偏了。
 *    · 至于"收到的全是 0x00"，那是"线上长期为低"，更像共地/供电/接线问题，
 *      换波特率不改变它的性质（115200 和 9600 都试过，一样的 0x00）。
 *  ⚠️ 只影响 UART0（IMU，DevId=0）。控制台 UART1 一直是 115200，不要动。 */
#define APU_IMU_BAUDRATE        115200U

/* ★ 2026-09-20：APU_IMU_REPORT_DIV（上报分频）已挪到 apu_imu.h ——
 *   它是个可调项，而且 @imu:loop 的提示文案要引用它的值。 */

/* ★ 2026-09-20：原始字节快照深度（@imu:raw 用） */
#define APU_IMU_SNAP_SIZE       32U

/* ---------------------------------------------------------------------------
 * 环形缓冲（收字节用）
 * --------------------------------------------------------------------------- */
static volatile uint8_t  g_rb[APU_IMU_RB_SIZE];
static volatile uint32_t g_rb_head;
static volatile uint32_t g_rb_tail;

static volatile uint8_t  g_rx_byte;     /* 驱动往里填的 1 字节缓冲 */
static AL_UART_HalStruct *g_uart;
static int                g_ready;

/* 正在攒的当前行 */
static char     g_line[APU_IMU_LINE_MAX];
static uint32_t g_line_len;
static int      g_line_overflow;

/* 解析结果 */
static float    g_roll, g_pitch, g_yaw;
static int      g_seen;
static uint32_t g_last_tick;

/* 统计 */
static uint32_t g_rx_bytes;
static uint32_t g_lines_ok;
static uint32_t g_lines_bad;
static uint32_t g_overflow;

/* ★ 2026-09-20：上报分频计数（对应传承的 uart_count） */
static uint32_t g_report_div;

/* ★ 2026-09-20：字节分类统计 + 原始快照（全部在过滤**之前**统计，
 *   这样才能看到"线上真正收到的是什么"，而不是被过滤后的结果） */
static uint32_t g_cls_print;
static uint32_t g_cls_crlf;
static uint32_t g_cls_other;
static uint8_t  g_snap[APU_IMU_SNAP_SIZE];
static uint32_t g_snap_len;
static uint32_t g_snap_pos;

/* ---------------------------------------------------------------------------
 * 环形缓冲操作
 * --------------------------------------------------------------------------- */
static void rb_push(uint8_t c)
{
	uint32_t next = (g_rb_head + 1U) & (APU_IMU_RB_SIZE - 1U);

	if (next == g_rb_tail) {
		g_overflow++;
		return;         /* 满：丢最新的 */
	}
	g_rb[g_rb_head] = c;
	g_rb_head = next;
}

static int rb_pop(uint8_t *c)
{
	if (g_rb_tail == g_rb_head)
		return 0;
	*c = g_rb[g_rb_tail];
	g_rb_tail = (g_rb_tail + 1U) & (APU_IMU_RB_SIZE - 1U);
	return 1;
}

/* ---------------------------------------------------------------------------
 * 重新 arm 一字节接收
 * --------------------------------------------------------------------------- */
/* 我们的 UART0 接收中断处理函数（定义在下面，先声明） */
static AL_VOID imu_irq_handler(AL_VOID *ref);

/* ===========================================================================
 * 内部：接管 UART0 的接收中断（与 apu_console.c 完全同一套做法）
 * ===========================================================================
 *  ★★ 2026-09-18 晚：不再用驱动那套"登记 1 字节缓冲 → 回调 → 手动重新 arm"。
 *
 *  那套流程有个**顺序上的坑**（详细经过见 apu_console.c）：
 *    驱动 AlUart_Dev_RecvData() 在"进去时 FIFO 里已有数据"时会走同步完成分支，
 *    **在 RxBusy 还没清掉的时候就回调我们**；回调里再 arm 会被"IsRxBusy 忙"挡住，
 *    驱动的 RecvBuffer 就永远停在 HandledCnt==RequestedCnt（"已满"），之后它不再读
 *    FIFO，只拿旧字节反复发 RECEIVE_DONE。
 *
 *  姿态板的数据是**连续整行**（"angle:r p y"），几乎必然满足"FIFO 里已有数据"，
 *  所以 P5 接上真板之前必须改掉。
 *
 *  自己接管后：中断条件是**电平型**的，ISR 里读空 FIFO 条件自动消失 →
 *  **不需要任何"重新 arm"**，那个坑不存在；而且仍是中断驱动，主循环忙时也不丢字节。
 * =========================================================================== */
static void imu_rx_setup(void)
{
	if (g_uart == AL_NULL)
		return;

	/* 触发门限 = 1 字节：收到一个字节就中断一次（默认的"半满"对整行数据太迟） */
	AlUart_ll_SetRxFifoThre(g_uart->Dev.BaseAddr, AL_UART_RxFIFO_CHAR_1);

	/* 打开"接收数据"中断并**保持常开** —— 不需要再 arm */
	AlUart_ll_SetRxIntr(g_uart->Dev.BaseAddr, AL_FUNC_ENABLE);

	/* 用我们自己的处理函数替换掉驱动的 UART ISR */
	(void)AlIntr_RegHandler((AL_S32)g_uart->Dev.IntrNum, AL_NULL,
				imu_irq_handler, AL_NULL);
}

/* ===========================================================================
 * 我们的 UART0 接收中断处理函数（ISR 上下文：不能打印、不能阻塞）
 * ===========================================================================
 *  把 RX FIFO 一次掏空，直接塞进环形缓冲。
 *  中断是**电平型**的，读空 FIFO 后条件自动消失 → **不需要任何"重新 arm"**。
 *  ⚠️ 刻意不调用任何 AlUart_Dev_* 接口 —— 完全不碰驱动的接收状态机。
 * =========================================================================== */
static AL_VOID imu_irq_handler(AL_VOID *ref)
{
	AL_U32 base;
	uint32_t n = 0U;

	(void)ref;
	if (g_uart == AL_NULL)
		return;

	base = g_uart->Dev.BaseAddr;

	while (AlUart_ll_IsRxDataReady(base)) {
		uint8_t c = (uint8_t)AlUart_ll_RecvByte(base);

		g_rx_bytes++;
		rb_push(c);

		/* 兜底：一次中断最多取 64 字节，避免异常情况下长时间停在 ISR 里 */
		if (++n >= 64U)
			break;
	}
}

/* ---------------------------------------------------------------------------
 * 初始化
 * --------------------------------------------------------------------------- */
int apu_imu_init(void)
{
	AL_UART_InitStruct cfg = {
		.BaudRate          = APU_IMU_BAUDRATE,
		.Parity            = AL_UART_NO_PARITY,
		.WordLength        = AL_UART_CHAR_8BITS,
		.StopBits          = AL_UART_STOP_1BIT,
		.HwFlowCtl         = AL_FALSE,
		.CharTimeoutEnable = AL_TRUE,
	};
	AL_S32 ret;

	apu_imu_reset();

	ret = AlUart_Hal_Init(&g_uart, APU_IMU_UART_DEV_ID, &cfg, AL_NULL);
	if (ret != AL_OK) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][imu] AlUart_Hal_Init(DevId=%u) failed, ret=0x%x "
		       "- is UART0PS_ENABLE on in soc_plat.h?",
		       (unsigned int)APU_IMU_UART_DEV_ID, (unsigned int)ret);
		g_uart = AL_NULL;
		g_ready = 0;
		return -1;
	}

	/* 不用驱动那套"事件回调"机制（我们已自己接管 IRQ，见 imu_rx_setup），
	 * 驱动的 ISR 不会再被调用，所以这里刻意**不注册**回调。 */

	/* 接管 UART0 的接收中断：自己掏 FIFO，不需要 arm（见 imu_rx_setup） */
	imu_rx_setup();

	/* "线路状态"中断也打开：只用于把错误状态暴露出来，我们不做处理 */
	AlUart_ll_EnableRecvLineIntr(g_uart->Dev.BaseAddr, AL_TRUE);

	g_ready = 1;

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][imu] UART0 ready (DevId=%u, %u 8N1) -> CEP pin7/8 (T5/U5) "
	       "expect lines: \"angle:<roll> <pitch> <yaw>\"",
	       (unsigned int)APU_IMU_UART_DEV_ID, (unsigned int)APU_IMU_BAUDRATE);
	return 0;
}

void apu_imu_reset(void)
{
	g_rb_head = 0U;
	g_rb_tail = 0U;
	g_line_len = 0U;
	g_line_overflow = 0;
	g_roll = 0.0f;
	g_pitch = 0.0f;
	g_yaw = 0.0f;
	g_seen = 0;
	g_last_tick = 0U;
	g_rx_bytes = 0U;
	g_lines_ok = 0U;
	g_lines_bad = 0U;
	g_overflow = 0U;
	g_report_div = 0U;
	g_cls_print = 0U;
	g_cls_crlf  = 0U;
	g_cls_other = 0U;
	g_snap_len  = 0U;
	g_snap_pos  = 0U;
}

/* ---------------------------------------------------------------------------
 * 解析一行（对应传承项目 slaver_00_example.c:966-991）
 * ---------------------------------------------------------------------------
 *  返回 1 = 收下一行有效数据，0 = 这一行不合法（丢弃，与原项目一致）
 * --------------------------------------------------------------------------- */
static int imu_parse_line(const char *line)
{
	float parsed_roll;
	float parsed_pitch;
	float parsed_yaw;

	/* ① 按 "angle:%f %f %f" 解析；不匹配就丢（原项目 :970-977） */
	if (sscanf(line, "angle:%f %f %f",
		   &parsed_roll, &parsed_pitch, &parsed_yaw) != 3)
		return 0;

	/* ② yaw 归一化到 -180..+180（原项目 :979） */
	parsed_yaw = apu_pipe_center_wrap180(parsed_yaw);

	/* ③ 范围校验：非法就丢（原项目 :980-985） */
	if (!apu_nav_safety_imu_angles_valid(parsed_roll, parsed_pitch, parsed_yaw))
		return 0;

	/* ④ 收下 */
	g_roll = parsed_roll;
	g_pitch = parsed_pitch;
	g_yaw = parsed_yaw;
	g_last_tick = apu_tick_now();
	g_seen = 1;
	g_lines_ok++;

	/* ⑤ 交给业务层（★ 只有真驱动会调 —— 注入命令已于 2026-09-21 删除） */
apu_app_update_attitude(g_roll, g_pitch, g_yaw);

	/* ⑥ ★ 2026-09-20：上报 —— **逐字照传承代码的做法**
	 *    （slaver_00_example.c:993-1000）：
	 *        uart_count++;
	 *        if (uart_count < 10) continue;
	 *        uart_count = 0;
	 *        send_str_to_linux(..., "R:%.2f,P:%.2f,Y:%.2f", ...);
	 *    也就是「每收满 10 行有效数据上报一次」，不是定时器周期发。
	 *    上报门 @rep 由 apu_app_report_attitude() 内部处理。 */
	if (++g_report_div >= APU_IMU_REPORT_DIV) {
		g_report_div = 0;
		apu_app_report_attitude(g_roll, g_pitch, g_yaw);
	}

	return 1;
}

/* ---------------------------------------------------------------------------
 * 主循环调用：攒行 + 解析
 * --------------------------------------------------------------------------- */
int apu_imu_poll(void)
{
	uint8_t c;
	int parsed = 0;

	if (!g_ready)
		return 0;

	while (rb_pop(&c)) {
		/* ★ 2026-09-20：先分类统计 + 存原始快照（放在所有过滤之前） */
		if (c == (uint8_t)'\r' || c == (uint8_t)'\n')
			g_cls_crlf++;
		else if (c >= 0x20U && c <= 0x7EU)
			g_cls_print++;
		else
			g_cls_other++;

		g_snap[g_snap_pos] = c;
		g_snap_pos = (g_snap_pos + 1U) & (APU_IMU_SNAP_SIZE - 1U);
		if (g_snap_len < APU_IMU_SNAP_SIZE)
			g_snap_len++;
		if (c == (uint8_t)'\r' || c == (uint8_t)'\n') {
			if (g_line_overflow) {
				g_line_overflow = 0;
				g_line_len = 0U;
				continue;
			}
			if (g_line_len == 0U)
				continue;

			g_line[g_line_len] = '\0';
			g_line_len = 0U;

			if (!imu_parse_line(g_line))
				g_lines_bad++;      /* 不合法行：计数但不打印（会刷屏） */
			else
				parsed++;
			continue;
		}

		if (g_line_overflow)
			continue;                   /* 丢弃到换行为止 */

		if (g_line_len >= (APU_IMU_LINE_MAX - 1U)) {
			g_line_overflow = 1;
			g_lines_bad++;
			continue;
		}

		/* 只收可打印 ASCII + 去掉行首空格，避免串口杂波 */
		if (c < 0x20U || c > 0x7EU)
			continue;

		g_line[g_line_len] = (char)c;
		g_line_len++;
	}

	return parsed;
}

/* ---------------------------------------------------------------------------
 * 查询
 * --------------------------------------------------------------------------- */
void apu_imu_get_angles(float *roll, float *pitch, float *yaw)
{
	if (roll)  *roll = g_roll;
	if (pitch) *pitch = g_pitch;
	if (yaw)   *yaw = g_yaw;
}

int apu_imu_seen(void)
{
	return g_seen;
}

uint32_t apu_imu_last_tick(void)
{
	return g_last_tick;
}

int apu_imu_is_stale(void)
{
	if (!g_seen)
		return 1;
	return (uint32_t)(apu_tick_now() - g_last_tick) >= NAV_IMU_TIMEOUT_TICKS;
}

uint32_t apu_imu_rx_bytes(void)
{
	return g_rx_bytes;
}

uint32_t apu_imu_lines_ok(void)
{
	return g_lines_ok;
}

uint32_t apu_imu_lines_bad(void)
{
	return g_lines_bad;
}

uint32_t apu_imu_overflow(void)
{
	return g_overflow;
}

/* ===========================================================================
 * ★★ 2026-09-20 新增：UART0 自检（把安路这一侧单独摘出来验）
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 *  在 UART0 TX 上直接发一串字节。
 *  ⚠️ 每发一个字节都等 "TSR + TX FIFO 都空" —— 慢（25 字节约 2ms），
 *     但**最稳**，不依赖 DMA / 中断 / 驱动状态机。
 *  ⚠️ guard 上限：TX 万一坏掉，立刻返回 -1，**绝不在这里死循环**
 *     （I2C 那次 Polling 无超时死循环的教训，见 F-16）。
 * --------------------------------------------------------------------------- */
int apu_imu_tx_raw(const char *s)
{
	AL_U32 base;
	uint32_t i;

	if (g_uart == AL_NULL || s == AL_NULL)
		return -1;

	base = g_uart->Dev.BaseAddr;

	for (i = 0U; s[i] != '\0'; i++) {
		uint32_t guard = 0U;

		while (!AlUart_ll_IsEmptyTsrAndTxFifo(base)) {
			if (++guard > 2000000U)
				return -1;
		}
		AlUart_ll_SendByte(base, (AL_U8)s[i]);
	}

	/* 等最后一个字节真的出去 */
	{
		uint32_t guard = 0U;

		while (!AlUart_ll_IsEmptyTsrAndTxFifo(base)) {
			if (++guard > 2000000U)
				return -1;
		}
	}

	return (int)i;
}

/* ★ 2026-09-20：回环自检 —— 连发 APU_IMU_REPORT_DIV 行，凑满一个上报周期。
 *  为什么不是发 1 行：上报已按传承代码改成"每收满 10 行发一次"，
 *  只发 1 行的话 @rep:on 也看不到 SENSOR 帧（会让人误判成回环失败）。 */
int apu_imu_selftest_loopback(void)
{
	static const char kLine[] = "angle:1.23 -0.45 178.90\r\n";
	uint32_t k;
	int      total = 0;

	for (k = 0U; k < APU_IMU_REPORT_DIV; k++) {
		int n = apu_imu_tx_raw(kLine);

		if (n < 0)
			return -1;
		total += n;
	}

	return total;
}

void apu_imu_byte_classes(uint32_t *printable, uint32_t *crlf, uint32_t *other)
{
	if (printable) *printable = g_cls_print;
	if (crlf)      *crlf      = g_cls_crlf;
	if (other)     *other     = g_cls_other;
}

uint32_t apu_imu_raw_snapshot(uint8_t *buf, uint32_t max_len)
{
	uint32_t n, k, start;

	if (buf == AL_NULL || max_len == 0U)
		return 0U;

	n = (g_snap_len < max_len) ? g_snap_len : max_len;
	/* g_snap_pos 是"下一个要写的位置"，所以最老的字节在 pos-n 处 */
	start = (g_snap_pos + APU_IMU_SNAP_SIZE - n) & (APU_IMU_SNAP_SIZE - 1U);

	for (k = 0U; k < n; k++)
		buf[k] = g_snap[(start + k) & (APU_IMU_SNAP_SIZE - 1U)];

	return n;
}
