/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_console.c —— APU1 裸机侧"串口控制台"实现
 * ===========================================================================
 *  【工作原理】（和 SDK 官方例程一致的单字节中断接收法）
 *
 *      ① 调一次 AlUart_Dev_RecvData(&AlLog->Dev, &g_rx_byte, 1)
 *         —— 它把"1 字节接收缓冲"登记进驱动，并打开 RX 线中断；
 *            如果 FIFO 里已经有数据，它会**立刻**把缓冲填满并同步回调。
 *      ② ISR（AlUart_Dev_RecvDataHandler）收到字节 → 填缓冲 → 回调我们
 *      ③ 我们的回调把字节塞进环形缓冲，然后**重新 arm 下一字节**
 *      ④ 主循环 apu_console_poll() 从环形缓冲里攒行
 *
 *  ⚠️ 三个坑（都是实读 SDK 源码确认的）：
 *      1. 回调里**必须**重新 arm（AlUart_Dev_RecvData），否则只收到一个字节就停了
 *      2. arm 之前要确认 RX 不忙（AlUart_Dev_IsRxBusy），否则断言 AL_UART_ERR_BUSY
 *      3. 驱动**自带环形缓冲是给打印用的**，收数据必须自己写 ring buffer
 *         （与交接文档"UART 没有环形缓冲 → 会丢数据"一致）
 * ===========================================================================
 */

#include "apu_console.h"

#include <stdarg.h>
#include <string.h>

#include "al_core.h"
#include "al_log.h"           /* extern AL_UART_HalStruct *AlLog */
#include "al_uart_hal.h"
#include "al_uart_dev.h"
#include "al_uart_ll.h"       /* RX FIFO / 寄存器操作（自接管中断用） */
#include "al_intr.h"          /* AlIntr_RegHandler（注册我们自己的 UART ISR） */
#include "apu_time.h"         /* apu_time_ms（诊断节流用） */

/* ===========================================================================
 * 环形缓冲（收字节用）
 * =========================================================================== */
#define APU_CONSOLE_RB_SIZE     256U    /* 必须是 2 的幂，才能用 & 取模 */
#define APU_CONSOLE_RB_MASK     (APU_CONSOLE_RB_SIZE - 1U)

static volatile uint8_t  g_rb[APU_CONSOLE_RB_SIZE];
static volatile uint32_t g_rb_head;     /* 写指针（ISR） */
static volatile uint32_t g_rb_tail;     /* 读指针（主循环） */

static volatile uint8_t  g_rx_byte;     /* 驱动往里填的 1 字节缓冲 */
static volatile int      g_ready;       /* 1 = 控制台可用 */

static uint32_t g_rx_bytes;
static uint32_t g_overflow;
/* 接收诊断状态（见 console_diag_print） */
static uint32_t g_diag_last_ms;
static int      g_diag_first_seen;
static uint32_t g_rx_polled;     /* 由主循环轮询取到的字节数 */
static uint32_t g_rx_isr;        /* 由 ISR 取到的字节数（正常应恒为 0：ERBFI 已关） */

/* 正在攒的当前行 */
static char     g_line[APU_CONSOLE_LINE_MAX];
static uint32_t g_line_len;
static int      g_line_overflow;        /* 本行已经超长，丢弃到换行为止 */

/* ===========================================================================
 * 内部：往环形缓冲塞一个字节（ISR 上下文）
 * =========================================================================== */
static void rb_push(uint8_t c)
{
	uint32_t next = (g_rb_head + 1U) & APU_CONSOLE_RB_MASK;

	if (next == g_rb_tail) {
		/* 满：丢最新的（控制台上丢几个字符比丢整行好排查） */
		g_overflow++;
		return;
	}
	g_rb[g_rb_head] = c;
	g_rb_head = next;
}

static int rb_pop(uint8_t *c)
{
	if (g_rb_tail == g_rb_head)
		return 0;
	*c = g_rb[g_rb_tail];
	g_rb_tail = (g_rb_tail + 1U) & APU_CONSOLE_RB_MASK;
	return 1;
}

/* ===========================================================================
 * 内部：重新 arm 一字节接收
 * =========================================================================== */
/* 我们的 UART1 接收中断处理函数（定义在下面，这里先声明） */
static AL_VOID console_irq_handler(AL_VOID *ref);

/* ===========================================================================
 * 内部：接管 UART1 的接收中断
 * ===========================================================================
 *  ★★ 2026-09-18 晚 第三次修正 —— 最终方案：**自己接管中断 + ISR 里掏空 FIFO** ★★
 *
 *  不再用驱动那套"登记 1 字节缓冲 → 中断回调 → 手动重新 arm"的流程。原因：
 *    驱动 AlUart_Dev_RecvData() 在"进去时 FIFO 里已经有数据"时会走**同步完成**分支，
 *    **在 RxBusy 还没清掉的时候就回调我们**；回调里再调 RecvData 会被"RxBusy 忙"
 *    挡住 → 驱动的 RecvBuffer 永远停在 HandledCnt == RequestedCnt（"已满"）
 *    → 之后它不再去读 FIFO，只会拿旧字节反复发 RECEIVE_DONE。
 *
 *  自己接管后：
 *    · 中断条件是**电平型**的（FIFO 里有数据就一直是 1），ISR 里把 FIFO 读空，
 *      条件自动消失 → **根本不需要"重新 arm"** → 那个"半步"的坑不存在；
 *    · 仍然是**中断驱动** → 主循环被 I2C 超时卡住时，字节照样收，不会丢；
 *    · 完全不碰驱动的 RecvBuffer / RxBusy 状态机。
 *
 *  影响面：只替换 IRQ（UART1 = 日志口）的处理函数。日志发送走的是阻塞式
 *  SendByte，不用中断发送，所以没有副作用。
 * =========================================================================== */
static void console_rx_setup(void)
{
	if (AlLog == AL_NULL)
		return;

	/* FIFO 触发门限 = 1 字节：敲一个字节就中断一次
	 * （驱动默认的"半满"对交互式控制台太迟） */
	AlUart_ll_SetRxFifoThre(AlLog->Dev.BaseAddr, AL_UART_RxFIFO_CHAR_1);

	/* 打开"接收数据"中断并**保持常开** —— 不需要再 arm / 重新 arm */
	AlUart_ll_SetRxIntr(AlLog->Dev.BaseAddr, AL_FUNC_ENABLE);

	/* 用我们自己的处理函数替换掉驱动的 UART ISR */
	(void)AlIntr_RegHandler((AL_S32)AlLog->Dev.IntrNum, AL_NULL,
				console_irq_handler, AL_NULL);
}

/* ===========================================================================
 * 我们的 UART1 接收中断处理函数（ISR 上下文：不能打印、不能阻塞）
 * ===========================================================================
 *  把 RX FIFO 一次掏空。中断是电平型的，读空 FIFO 后条件自动消失，
 *  所以**不需要任何"重新 arm"**。
 *  ⚠️ 这里刻意不调用任何 AlUart_Dev_* 接口 —— 完全不碰驱动的接收状态机。
 * =========================================================================== */
static AL_VOID console_irq_handler(AL_VOID *ref)
{
	AL_U32 base;
	uint32_t n = 0U;

	(void)ref;
	if (AlLog == AL_NULL)
		return;

	base = AlLog->Dev.BaseAddr;

	while (AlUart_ll_IsRxDataReady(base)) {
		uint8_t b = (uint8_t)AlUart_ll_RecvByte(base);

		g_rx_isr++;
		g_rx_bytes++;
		rb_push(b);

		/* 兜底：一次中断最多取 64 字节，避免异常情况下长时间停在 ISR 里 */
		if (++n >= 64U)
			break;
	}
}

/* ===========================================================================
 * 初始化
 * =========================================================================== */
int apu_console_init(void)
{
	if (AlLog == AL_NULL) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][console] AlLog is NULL - log UART not initialised, console disabled");
		return -1;
	}

	g_rb_head = 0U;
	g_rb_tail = 0U;
	g_line_len = 0U;
	g_line_overflow = 0;
	g_rx_bytes = 0U;
	g_overflow = 0U;

	/* 不用驱动那套"事件回调"机制了（我们已自己接管 IRQ，见 console_rx_setup）。
	 * 驱动的 ISR 不会再被调用，所以这里刻意**不注册**回调。 */

	/* 打开 RX 线中断并 arm 第一字节 */
	/* 接管 UART1 的接收中断：自己掏 FIFO，不需要 arm（见 console_rx_setup） */
	console_rx_setup();

	/* "线路状态"中断也打开：只用于把错误状态暴露出来，我们不做处理 */
	AlUart_ll_EnableRecvLineIntr(AlLog->Dev.BaseAddr, AL_TRUE);

	g_ready = 1;

	{
		/* ---- 接收通路自检 ---- */
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][console] rx diag: DevId=%u base=0x%08X irq=%u state=0x%X rxbusy=%d IER=0x%02X LSR=0x%02X (自接管中断: IER bit0 应为 1, 无需 arm)",
		       (unsigned int)AlLog->Dev.DevId,
		       (unsigned int)AlLog->Dev.BaseAddr,
		       (unsigned int)AlLog->Dev.IntrNum,
		       (unsigned int)AlLog->Dev.State,
		       (int)AlUart_Dev_IsRxBusy(&AlLog->Dev),
		       (unsigned int)(AL_REG32_READ(AlLog->Dev.BaseAddr + UART__IER_DLH__OFFSET) & 0xFFU),
		       (unsigned int)(AL_REG32_READ(AlLog->Dev.BaseAddr + UART__LSR__OFFSET) & 0xFFU));
	}

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][console] ready on log UART (DevId=%u) - type commands like: CMD:STOP",
	       (unsigned int)AlLog->Dev.DevId);
	return 0;
}

/* ===========================================================================
 * 取命令行（主循环调用）
 * =========================================================================== */
/* ===========================================================================
 * 接收诊断（2026-09-18 晚，第二版：**不会闭嘴**）
 * ===========================================================================
 *  上一版的诊断"收到第一个字节后就永久闭嘴"，结果真正失败时**日志里什么信息都没有**，
 *  只能靠猜 —— 那是个糟糕的设计。这一版一直报，状态一眼可见：
 *
 *      bytes=0 且不变            → 一个字节都没到（接线 / 工具没发送）
 *      line_len>0 且长期不变      → 字节到了，但**这一行没结束**（回车没到）
 *      line_len 归零 + 出现回复   → 正常
 *      **连这行都不再出现**       → **主循环卡住了**（这一版最要防的就是 ISR 卡死主循环）
 *
 *  节流：有半行卡着、或还没收到字节时，每 2 秒一行；空闲时每 10 秒一行心跳。
 * =========================================================================== */
static void console_diag_print(void)
{
	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][console] rx: bytes=%u isr=%u polled=%u line_len=%u ovf=%u LSR=0x%02X rx_ready=%d",
	       (unsigned int)g_rx_bytes, (unsigned int)g_rx_isr, (unsigned int)g_rx_polled,
	       (unsigned int)g_line_len, (unsigned int)g_overflow,
	       (unsigned int)(AL_REG32_READ(AlLog->Dev.BaseAddr + UART__LSR__OFFSET) & 0xFFU),
	       (int)AlUart_ll_IsRxDataReady(AlLog->Dev.BaseAddr));
}

int apu_console_poll(char *line, uint32_t max_len)
{
	uint8_t c;

	if (!g_ready || line == AL_NULL || max_len == 0U)
		return 0;

	/* ---- 轮询兜底（**备用**路径）----
	 * 正常情况字节全部由中断收走（isr 增长、polled 不增长）。
	 * 保留它是为了"万一中断又出问题，控制台也不至于完全失联"。
	 * 两条路径读的是同一个 FIFO，每个字节只会被其中一条取走，不冲突。 */
	while (AlUart_ll_IsRxDataReady(AlLog->Dev.BaseAddr)) {
		uint8_t b = (uint8_t)AlUart_ll_RecvByte(AlLog->Dev.BaseAddr);

		g_rx_polled++;
		g_rx_bytes++;
		rb_push(b);
	}

	/* ---- 接收诊断（不会闭嘴版）---- */
	{
		uint32_t diag_now = apu_time_ms();
		uint32_t diag_period;

		/* 有半行卡着、或一个字节都没收到 → 2 秒一行；空闲 → 10 秒一行心跳 */
		if ((g_rx_bytes == 0U) || (g_line_len > 0U))
			diag_period = 2000U;
		else
			diag_period = 10000U;

		if ((uint32_t)(diag_now - g_diag_last_ms) >= diag_period) {
			g_diag_last_ms = diag_now;
			console_diag_print();
		}
	}

	/* ---- 收到第一个字节时报一次"通路成立"，并附 GIC 状态 ---- */
	if ((g_rx_bytes != 0U) && (g_diag_first_seen == 0)) {
		AL_U32 isen = AL_REG32_READ(0xDD00010CU);
		AL_U32 irou = AL_REG32_READ(0xDD006338U);

		g_diag_first_seen = 1;
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][console] ★ 收到字节了: bytes=%u (isr=%u polled=%u)",
		       (unsigned int)g_rx_bytes, (unsigned int)g_rx_isr, (unsigned int)g_rx_polled);
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][console] gic: GICD_CTLR=0x%08X ISENABLER3=0x%08X bit7=%u IROUTER103=0x%08X",
		       (unsigned int)AL_REG32_READ(0xDD000000U),
		       (unsigned int)isen, (unsigned int)((isen >> 7) & 1U),
		       (unsigned int)irou);
	}

	while (rb_pop(&c)) {
		if (c == (uint8_t)'\r' || c == (uint8_t)'\n') {
			/* 行结束 */
			if (g_line_overflow) {
				/* 这一行因为太长被丢了，换行时清标志并忽略 */
				g_line_overflow = 0;
				g_line_len = 0U;
				continue;
			}
			if (g_line_len == 0U)
				continue;       /* 空行 */

			g_line[g_line_len] = '\0';
			g_line_len = 0U;

			if (strlen(g_line) >= max_len)
				continue;       /* 目标缓冲太小，安全起见丢掉 */

			(void)strcpy(line, g_line);
			return 1;
		}

		if (g_line_overflow)
			continue;               /* 丢弃到换行为止 */

		if (g_line_len >= (APU_CONSOLE_LINE_MAX - 1U)) {
			g_line_overflow = 1;
			AL_LOG(AL_LOG_LEVEL_ERROR,
			       "[APU1][console] line too long (>%u), discarded",
			       (unsigned int)(APU_CONSOLE_LINE_MAX - 1U));
			continue;
		}

		/* 支持退格，方便手打时改错 */
		if (c == 0x08U || c == 0x7FU) {
			if (g_line_len > 0U)
				g_line_len--;
			continue;
		}

		/* 只接受可打印 ASCII，避免串口杂波把命令搞脏 */
		if (c < 0x20U || c > 0x7EU)
			continue;

		g_line[g_line_len] = (char)c;
		g_line_len++;
	}

	return 0;
}

/* ===========================================================================
 * 打印
 * =========================================================================== */
void apu_console_print(const char *fmt, ...)
{
	char buf[APU_CONSOLE_LINE_MAX * 2];
	va_list args;

	va_start(args, fmt);
	(void)vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	/* AL_LOG 会自动补 \r\n */
	AL_LOG(AL_LOG_LEVEL_INFO, "%s", buf);
}

void apu_console_write_raw(const char *s)
{
	if (s == AL_NULL)
		return;
	AlLog_Write(s, (AL_U32)strlen(s));
}

uint32_t apu_console_rx_bytes(void)
{
	return g_rx_bytes;
}

uint32_t apu_console_overflow_count(void)
{
	return g_overflow;
}
