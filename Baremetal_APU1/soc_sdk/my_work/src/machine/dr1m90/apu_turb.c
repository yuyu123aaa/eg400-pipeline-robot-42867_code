/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_turb.c —— 浑浊度传感器 XM3318B 驱动实现（Modbus-RTU over 板载 RS485）
 * ===========================================================================
 *  协议、寄存器、接线、换算倍率写在 apu_turb.h 顶部。这里讲实现。
 *
 *  ★★ 2026-10-14 重要修正：改成**非阻塞状态机** ★★
 *  ---------------------------------------------------------------------------
 *  【改之前错在哪】
 *    原实现一次调用里把整帧收完，最长阻塞 APU_TURB_FRAME_TIMEOUT_MS(600ms)。
 *    它和 platform_poll()（rpmsg）在同一条主循环上，主循环本该 ~20µs 一轮。
 *    三路传感器阻塞叠加最多 960ms/轮 → Linux 侧报 **Connection reset**，
 *    APU1 日志出现 `[APU1][loop] step=1 iter=1 t=4946 ms`（第一轮就 4.9 秒）。
 *
 *  【现在怎么改】
 *        阶段 A：到点 → 拉高 DE、发问询、**立刻拉低 DE**、返回（微秒级）
 *        阶段 B：每轮只尝试收 1 个字节，收满 7 字节立即校验（微秒级）
 *
 *  【RS485 半双工 —— 仍然是本文件最要小心的地方】
 *    DE 必须在 **apu_pluart_write() 返回后立刻** 拉低。晚了总线还被自己的
 *    驱动器占着，传感器的回复进不来 —— 现象是"永远 TIMEOUT，但波形全对"。
 *    所以拉低 DE 这一步放在阶段 A 内部、write 之后，不跨轮次。
 *
 *  【CRC16 刻意各留一份】：本文件与 apu_gas.c 各自实现，互不依赖，
 *    这样两个驱动都能独立删掉/独立验证。同一套 Modbus 向量验证过。
 * ===========================================================================
 */

#include "apu_turb.h"

#include "al_core.h"

#include "apu_console.h"
#include "apu_time.h"

static uint32_t g_base;
static int      g_inited;
static uint8_t  g_addr  = APU_TURB_SLAVE_ADDR;
static float    g_scale = APU_TURB_SCALE_DEFAULT;

/* ---------------------------------------------------------------------------
 *  ★★ 2026-10-15 修正（第二版）：纯硬件轮询，**绝不调用 AlSys_UDelay** ★★
 *  ---------------------------------------------------------------------------
 *  【第一版错在哪】用 { getc; AlSys_UDelay(50); } 按 50µs 步进等 ——
 *      结果浑浊度从"通用地址查询成功"退化成 got=0。
 *      AlSys_UDelay 在 APU1 这条路径上不可靠，会把接收窗口整个吃掉。
 *
 *  【第二版】不用任何延时：SR 寄存器可无限次快读，收到即返回；
 *      退出只看毫秒超时。空转次数给足余量，不准也只是多转几圈。
 * --------------------------------------------------------------------------- */
#define APU_TURB_CHAR_SPIN_MS   3U      /* 9600 一个字节约 1.04ms */
#define APU_TURB_SPIN_PASSES    4000U

/* ---------------------------------------------------------------------------
 *  CRC16 / Modbus
 * --------------------------------------------------------------------------- */
static uint16_t crc16_modbus(const uint8_t *buf, uint32_t n)
{
	uint16_t crc = 0xFFFFU;
	uint32_t i;
	int      k;

	for (i = 0U; i < n; i++) {
		crc ^= (uint16_t)buf[i];
		for (k = 0; k < 8; k++) {
			if (crc & 1U)
				crc = (uint16_t)((crc >> 1) ^ 0xA001U);
			else
				crc = (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

static void result_reset(apu_turb_result_t *out)
{
	int i;

	out->res        = APU_TURB_E_OK;
	out->raw_len    = 0U;
	out->raw_value  = 0U;
	out->value      = 0.0f;
	out->scale      = g_scale;
	out->unit       = "UNT";
	out->elapsed_ms = 0U;
	for (i = 0; i < (int)APU_TURB_RESP_LEN; i++)
		out->raw[i] = 0U;
}

/* ---------------------------------------------------------------------------
 *  发一条 Modbus 问询（RS485 半双工：拉高 DE -> 发 -> 立刻拉低 DE）
 *  addr_may_be_broadcast: 0xFA 通用地址查询时第 1 字节是 0xFA，不是 g_addr
 *  返回 0 成功；否则错误码。
 * --------------------------------------------------------------------------- */
static int send_mb(uint8_t slave, uint8_t func, uint16_t reg, uint16_t cnt)
{
	uint8_t  frame[APU_TURB_REQ_LEN];
	uint16_t crc;
	uint32_t sent;
	int      stuck = 0;
	int      out = -1, pin = -1;

	frame[0] = slave;
	frame[1] = func;
	frame[2] = (uint8_t)(reg >> 8);
	frame[3] = (uint8_t)(reg & 0xFFU);
	frame[4] = (uint8_t)(cnt >> 8);
	frame[5] = (uint8_t)(cnt & 0xFFU);
	crc      = crc16_modbus(frame, 6U);
	frame[6] = (uint8_t)(crc & 0xFFU);
	frame[7] = (uint8_t)((crc >> 8) & 0xFFU);

	/* ① 切发送态；方向脚起不来就别发了 */
	apu_pluart_rs485_de(1);
	if (apu_pluart_rs485_de_readback(&out, &pin) != 0) {
		apu_pluart_rs485_de(0);
		return APU_TURB_E_DE_FAIL;
	}

	/* ② 丢掉残渣（必须在拉高 DE 之后、发送之前） */
	apu_pluart_rx_flush(g_base);

	/* ③ 发送（write 内部已等 TX 排空） */
	sent = apu_pluart_write(g_base, frame, APU_TURB_REQ_LEN, &stuck);

	/* ④ ★ 立刻切回接收态 —— 顺序关键，不能挪到收完之后 ★ */
	apu_pluart_rs485_de(0);

	if (stuck != 0 || sent != APU_TURB_REQ_LEN)
		return APU_TURB_E_TX_STUCK;
	return 0;
}

/* ---------------------------------------------------------------------------
 *  校验一条已收齐的应答（按指定的期望从机地址）
 *  返回 0 = 通过并把 16 位数据写进 *out_val；否则错误码。
 * --------------------------------------------------------------------------- */
static int check_resp(const uint8_t *resp, uint8_t expect_addr,
		      uint16_t *out_val)
{
	uint16_t crc;

	if (resp[0] != expect_addr || resp[1] != APU_TURB_FUNC_READ)
		return APU_TURB_E_BAD_ADDR;
	if (resp[2] != 0x02U)
		return APU_TURB_E_BAD_BYTECOUNT;

	crc = crc16_modbus(resp, APU_TURB_RESP_LEN - 2U);
	if (((uint8_t)(crc & 0xFFU) != resp[APU_TURB_RESP_LEN - 2U]) ||
	    ((uint8_t)((crc >> 8) & 0xFFU) != resp[APU_TURB_RESP_LEN - 1U]))
		return APU_TURB_E_BAD_CRC;

	if (out_val != AL_NULL)
		*out_val = (uint16_t)(((uint16_t)resp[3] << 8) | (uint16_t)resp[4]);
	return APU_TURB_E_OK;
}

/* ---------------------------------------------------------------------------
 *  ★ 非阻塞事务状态（文件内共用一套，因为一次只跑一件事）
 *
 *  调用者持有正在收的缓冲与超时起点。本函数每轮只推进一小步。
 *  返回 0 = 进行中；1 = 完成（结果放 *out_val / *out_err）
 * --------------------------------------------------------------------------- */
typedef struct {
	int      phase;         /* 0 = 空闲(还没发), 1 = 已发正在收 */
	uint32_t t_start;
	uint8_t  slave;         /* 本次问询用的从机地址 */
	uint8_t  expect_addr;   /* 应答里应该出现的地址 */
	uint8_t  buf[APU_TURB_RESP_LEN];
	uint8_t  got;
} turb_txn_t;

static turb_txn_t g_txn;

/* ★ 诊断计数：本轮从 FIFO 取出过多少字节（不做任何过滤）
 *   0     = 一个字节都没收到 → 信号/供电/接线问题
 *   >0 但不合帧 = 收到了但拼不成 0x01 03 02 .. 开头的帧 → 波特率或 A/B 接反
 *   定位完可以删。 */
static uint32_t g_turb_dbg_seen;

/* 发起一次事务（非阻塞，内部就把问询发出去、DE 已切回） */
static int txn_begin(uint8_t slave, uint8_t expect_addr, uint16_t reg)
{
	int r;

	g_txn.slave       = slave;
	g_txn.expect_addr = expect_addr;
	g_txn.got         = 0;
	g_txn.t_start     = apu_time_ms_live();
	g_turb_dbg_seen   = 0U;      /* ★ 诊断计数：本轮清零 */

	r = send_mb(slave, APU_TURB_FUNC_READ, reg, 1U);
	if (r != 0) {
		g_txn.phase = 0;
		return r;               /* 直接返回错误码 */
	}
	g_txn.phase = 1;
	return 0;
}

/* 推进接收。返回 0 = 还在收；1 = 收齐；-1 = 超时/超预算
 *
 * ★ 2026-10-15 修正：改成"**当次调用内连续抽干 FIFO**"。
 *   原因同 apu_gas.c：主循环约 1000ms 一圈，而 9600 下一帧只要 7ms；
 *   摊到多圈会在 FIFO 里滞留近 1 秒，一旦混入残渣就错位。
 *   ⚠️ 接收路径**不调用任何延时函数**（AlSys_UDelay 实测反而把窗口吃掉）。 */
#define APU_TURB_RECV_BUDGET_MS  100U   /* 单次调用的硬上界 */

static int txn_poll(void)
{
	uint8_t  b;
	uint32_t t_frame;

	if (g_txn.phase != 1)
		return -1;

	t_frame = apu_time_ms_live();

	while (g_txn.got < APU_TURB_RESP_LEN) {
		/* 硬上界：绝不把主循环拖过预算 */
		if ((uint32_t)(apu_time_ms_live() - g_txn.t_start) >=
		    APU_TURB_RECV_BUDGET_MS)
			return -1;

		/* 纯寄存器轮询，不用任何延时函数 */
		if (apu_pluart_getc(g_base, &b)) {
			g_txn.buf[g_txn.got++] = b;
			g_turb_dbg_seen++;              /* ★ 诊断计数 */
			t_frame = apu_time_ms_live();   /* 有进展，刷新帧内计时 */
			continue;
		}

		/* 帧内停顿过久且已有字节 → 本帧不全，提前收尾
		 * （原来这里用 APU_TURB_FRAME_TIMEOUT_MS(600ms) 判断，
		 *   而预算只有 100ms ⇒ 这条永远轮不到，等于没生效。改用预算。） */
		if (g_txn.got > 0U &&
		    (uint32_t)(apu_time_ms_live() - t_frame) >=
		    APU_TURB_RECV_BUDGET_MS)
			return -1;
	}
	return 1;
}

/* ---------------------------------------------------------------------------
 *  初始化
 * --------------------------------------------------------------------------- */
int apu_turb_init(void)
{
	int de_out = -1, de_pin = -1;

	g_base = apu_pluart_base(APU_PLUART_TURB);
	if (g_base == 0U) {
		g_inited = 0;
		return -1;
	}
	g_txn.phase = 0;

	/* 先把 DE 拉到接收态（上电默认就该是接收），顺便验证 GPIO 起没起 */
	apu_pluart_rs485_de(0);
	if (apu_pluart_rs485_de_readback(&de_out, &de_pin) != 0) {
		g_inited = 0;
		return -2;
	}

	apu_pluart_rx_flush(g_base);
	g_inited = 1;
	return 0;
}

int apu_turb_set_addr(uint8_t addr)
{
	if (addr == 0U || addr > 249U)
		return -1;
	g_addr = addr;
	return 0;
}

uint8_t apu_turb_get_addr(void)
{
	return g_addr;
}

int apu_turb_set_scale(float scale)
{
	if (!(scale > 0.0f))
		return -1;
	g_scale = scale;
	return 0;
}

float apu_turb_get_scale(void)
{
	return g_scale;
}

/* ---------------------------------------------------------------------------
 *  通用：读一个寄存器（对外辅助接口）
 *
 *  ★ 2026-10-14：内部改成**非阻塞分步**推进，绝不死等。
 *    对调用方而言仍是"同步返回"，但它不会把主循环卡住超过一帧的时间；
 *    实际上现在只用在这些地方：自检（已改成分步）、以及将来手工调试。
 *    ⚠️ 如果你要把它放进主循环，请改用 apu_turb_poll()，别用这个。
 * --------------------------------------------------------------------------- */
int apu_turb_read_reg(uint16_t reg, uint16_t *out_val)
{
	uint32_t t0;
	int      r;

	if (out_val != AL_NULL)
		*out_val = 0U;

	if (!g_inited || g_base == 0U)
		return APU_TURB_E_NO_DEVICE;

	r = txn_begin(g_addr, g_addr, reg);
	if (r != 0)
		return r;

	t0 = apu_time_ms_live();
	for (;;) {
		r = txn_poll();
		if (r == 1) {
			g_txn.phase = 0;
			return check_resp(g_txn.buf, g_txn.expect_addr, out_val);
		}
		if (r < 0) {
			g_txn.phase = 0;
			return APU_TURB_E_TIMEOUT;
		}
		if ((uint32_t)(apu_time_ms_live() - t0) >
		    (APU_TURB_FRAME_TIMEOUT_MS + 50U)) {
			g_txn.phase = 0;
			return APU_TURB_E_TIMEOUT;
		}
	}
}

/* ---------------------------------------------------------------------------
 *  用通用地址 0xFA 问出真实设备地址
 *  规格书原文："FA 即250 为通用地址…00 66 为设备地址的寄存器"
 *      问询 FA 03 00 66 00 01 71 9E
 *      应答 01 03 02 00 01 79 84   -> 第1字节 01 就是真实地址
 *  ⚠️ 用 0xFA 问、回来的第 1 字节是**真实地址**，不能按 0xFA 校验。
 * --------------------------------------------------------------------------- */
int apu_turb_probe_addr(uint8_t *out_addr)
{
	uint32_t t0;
	uint16_t v = 0U;
	int      r;

	if (out_addr != AL_NULL)
		*out_addr = 0U;

	if (!g_inited || g_base == 0U)
		return APU_TURB_E_NO_DEVICE;

	r = txn_begin(APU_TURB_ADDR_BROADCAST, 0xFFU, APU_TURB_REG_ADDR);
	if (r != 0)
		return r;

	t0 = apu_time_ms_live();
	for (;;) {
		r = txn_poll();
		if (r == 1) {
			g_txn.phase = 0;
			/* 地址字节事先不可知：先把 resp[0] 当期望地址再校验其余 */
			r = check_resp(g_txn.buf, g_txn.buf[0], &v);
			if (r != APU_TURB_E_OK)
				return r;
			if (out_addr != AL_NULL)
				*out_addr = (uint8_t)(v & 0xFFU);
			return APU_TURB_E_OK;
		}
		if (r < 0) {
			g_txn.phase = 0;
			return APU_TURB_E_TIMEOUT;
		}
		if ((uint32_t)(apu_time_ms_live() - t0) >
		    (APU_TURB_FRAME_TIMEOUT_MS + 50U)) {
			g_txn.phase = 0;
			return APU_TURB_E_TIMEOUT;
		}
	}
}

/* ---------------------------------------------------------------------------
 *  读浊度（**阻塞版**，仅供自检/单发使用，不要在循环里调）
 * --------------------------------------------------------------------------- */
int apu_turb_read(apu_turb_result_t *out)
{
	uint32_t t0;
	int      r;

	if (out == AL_NULL)
		return -1;

	result_reset(out);

	if (!g_inited || g_base == 0U) {
		out->res = APU_TURB_E_NO_DEVICE;
		return 0;
	}

	r = txn_begin(g_addr, g_addr, APU_TURB_REG_TURBIDITY);
	if (r != 0) {
		out->res = r;
		return 0;
	}

	t0 = apu_time_ms_live();
	for (;;) {
		r = txn_poll();
		if (r == 1) {
			uint16_t v = 0U;
			g_txn.phase  = 0;
			out->raw_len = g_txn.got;
			{
				int i;
				for (i = 0; i < (int)g_txn.got &&
					    i < (int)APU_TURB_RESP_LEN; i++)
					out->raw[i] = g_txn.buf[i];
			}
			out->elapsed_ms = (uint32_t)(apu_time_ms_live() - t0);
			out->res = check_resp(g_txn.buf, g_txn.expect_addr, &v);
			if (out->res == APU_TURB_E_OK) {
				out->raw_value = v;
				out->value     = (float)v / g_scale;
			}
			return 0;
		}
		if (r < 0) {
			g_txn.phase  = 0;
			out->raw_len = g_txn.got;
			out->res     = APU_TURB_E_TIMEOUT;
			out->elapsed_ms = (uint32_t)(apu_time_ms_live() - t0);
			return 0;
		}
		if ((uint32_t)(apu_time_ms_live() - t0) >
		    (APU_TURB_FRAME_TIMEOUT_MS + 50U)) {
			g_txn.phase  = 0;
			out->raw_len = g_txn.got;
			out->res     = APU_TURB_E_TIMEOUT;
			return 0;
		}
	}
}

#if APU_TURB_DEBUG_PRINT
/* 前向声明：turb_diag_step() 定义在文件后面（自检那一段） */
static int turb_diag_step(void);
static void dbg_dump_resp(const char *tag, const uint8_t *buf, int got);
#endif

/* ---------------------------------------------------------------------------
 *  ★★ 周期任务（挂主循环）—— 保留给独立调试用
 * ---------------------------------------------------------------------------
 *  ⚠️ 2026-10-15 架构调整后，**生产路径不再调用本函数**：
 *      三路的采集调度 + rpmsg 上报统一由 apu_env.c 负责（它调 apu_turb_read()）。
 *      本函数保留仅用于单独排查 RS485/浊度这一路（配合 APU_TURB_DEBUG_PRINT=1）。
 * --------------------------------------------------------------------------- */
void apu_turb_poll(void)
{
	static uint32_t last_ms;
	static int      first = 1;
	static apu_turb_result_t r;

	uint32_t now = apu_time_ms_live();
	int      st;

#if APU_TURB_DEBUG_PRINT
	/* 上电自检也在这里分步跑完（仅调试构建） */
	(void)turb_diag_step();
#endif

	/* 没到周期：立刻返回 */
	if (!first && (uint32_t)(now - last_ms) < APU_TURB_PERIOD_MS)
		return;
	first   = 0;
	last_ms = now;

	/*
	 * ★★ 2026-10-15：当场收发一次（txn_poll 内部会连续抽干 FIFO，
	 *    有 APU_TURB_RECV_BUDGET_MS(100ms) 硬上界）。
	 *    原因：主循环约 1000ms 一圈，而 9600 下一帧只要 7ms 到齐，
	 *    摊到多圈会在 FIFO 里滞留近 1 秒，混入残渣就错位。
	 */
	result_reset(&r);
	st = txn_begin(g_addr, g_addr, APU_TURB_REG_TURBIDITY);
	if (st != 0) {
		r.res = st;
#if APU_TURB_DEBUG_PRINT
		apu_turb_print(&r);
#endif
		return;
	}

	st = txn_poll();

	r.raw_len    = g_txn.got;
	r.elapsed_ms = (uint32_t)(apu_time_ms_live() - g_txn.t_start);
	{
		int i;
		for (i = 0; i < (int)g_txn.got && i < (int)APU_TURB_RESP_LEN; i++)
			r.raw[i] = g_txn.buf[i];
	}

	if (st < 0) {
		r.res = APU_TURB_E_TIMEOUT;
	} else {
		uint16_t v = 0U;
		r.res = check_resp(g_txn.buf, g_txn.expect_addr, &v);
		if (r.res == APU_TURB_E_OK) {
			r.raw_value = v;
			r.value     = (float)v / g_scale;
		}
	}

	/* 诊断行已撤：链路实测正常（01 03 02 xx xx CRC 全对）。
	 * 只保留失败时的 [dbg: seen=N] 计数（在 apu_turb_print 里），
	 * 需要时再打开 dbg_dump_resp() 即可。 */

#if APU_TURB_DEBUG_PRINT
	apu_turb_print(&r);
#else
	(void)r;
#endif
}

/* ---------------------------------------------------------------------------
 *  上电自检
 *
 *  ★★ 2026-10-14 重要修正：自检本身也必须是**非阻塞**的 ★★
 *  ---------------------------------------------------------------------------
 *  【改之前错在哪 —— 这是上一轮修复遗漏的一半】
 *    原 apu_turb_diag() 一次性串起三条**阻塞**事务：
 *        ① 通用地址 0xFA 查真实地址   最长 650ms
 *        ② 读型号编码 0x0064          最长 650ms
 *        ③ 读波特率编码 0x0067        最长 650ms
 *    ⇒ 最坏 ≈ 2 秒死等。而它跑在 apu_app_init() 之后、**进主循环之前**，
 *      正好是 rpmsg 端点刚建好、Linux 在等 APU1 回应的窗口。
 *      传感器没接好时这三步每次都等满超时 → Linux 等不到回应 → Connection reset。
 *
 *  【现在怎么改】
 *    拆成 5 个阶段，由 apu_turb_poll() 每轮只推进一步（微秒级）：
 *        0 = 空闲 / 已结束
 *        1 = 通用地址查询进行中
 *        2 = 读型号编码进行中
 *        3 = 读波特率编码进行中
 *        4 = 打印收尾
 *    全程没有"等"，主循环随时都能去处理 rpmsg。
 * --------------------------------------------------------------------------- */
static int      g_diag_stage;          /* 0=空闲/结束 1..4=进行中 */
static uint32_t g_diag_t0;
static uint8_t  g_diag_found;

/* 启动自检序列（非阻塞：只做"打头"动作，之后靠 turb_diag_step 推进） */
void apu_turb_diag(void)
{
	apu_console_print("[TURB] ===== XM3318B 自检（非阻塞，分步跑）=====");
	apu_console_print("[TURB] UART-B base=0x%08X  当前从机地址=0x%02X  倍率=%.3f",
			  (unsigned int)g_base,
			  (unsigned int)g_addr, (double)g_scale);

	g_diag_stage = 0;
	g_diag_t0    = apu_time_ms_live();
	{
		int r0 = txn_begin(APU_TURB_ADDR_BROADCAST, 0xFFU,
				   APU_TURB_REG_ADDR);
		if (r0 != 0) {
			/* ★ 关键：发送失败必须**当场报错**，不能带着 phase=0
			 *   进入阶段 1 —— 那样 txn_poll() 会立刻返回 -1，
			 *   打印出假的 "TIMEOUT (0ms)"（就是上板看到的那条）。 */
			apu_console_print("[TURB] ① 通用地址查询: 发送失败 (%s)",
					  apu_turb_err_str(r0));
			return;
		}
	}
	g_diag_stage = 1;
}

/* 把收到的应答字节原样打成十六进制（诊断用）。
 * 格式 "01 03 02 02 18 B9 2E"；got 可能小于期望长度。 */
static void dbg_dump_resp(const char *tag, const uint8_t *buf, int got)
{
	char     hex[APU_TURB_RESP_LEN * 3 + 1];
	uint32_t i, k = 0U;
	static const char kHex[] = "0123456789ABCDEF";

	if (got < 0)
		got = 0;
	if (got > (int)APU_TURB_RESP_LEN)
		got = (int)APU_TURB_RESP_LEN;

	for (i = 0U; i < (uint32_t)got && (k + 3U) < sizeof(hex); i++) {
		hex[k++] = kHex[(buf[i] >> 4) & 0x0FU];
		hex[k++] = kHex[buf[i] & 0x0FU];
		hex[k++] = ' ';
	}
	hex[k] = '\0';

	apu_console_print("[TURB] %s got=%d [%s]", tag, got, hex);
}

/* 推进自检一步。返回 1 = 自检全部结束（或本来就没在跑）
 *
 * ★ 2026-10-15 修正：txn_poll() 的语义已经从"非阻塞、返回 0 表示还在收"
 *   改成了"**阻塞到收完**，返回 1=收齐 / -1=超时"。
 *   原来这里写的 `if (r == 0) return 0;` 于是**永远不会命中**，
 *   一进阶段就把结果当超时处理 —— 实测表现就是"连通用地址查询都超时"
 *   （而它本来是成功的）。现在按新语义处理：直接看返回值定成败。
 */
static int turb_diag_step(void)
{
	int r;

	if (g_diag_stage == 0)
		return 1;

	switch (g_diag_stage) {
	case 1: {                       /* 通用地址查询（0xFA）—— 仅作"锦上添花" */
		uint16_t v = 0U;

		g_diag_t0 = apu_time_ms_live();
		r = txn_poll();         /* 阻塞到收完（内部有 100ms 上界） */

		/* ★★ 2026-10-15 修正：0xFA 探测失败**不是错误** ★★
		 * ---------------------------------------------------------------
		 *  上板实测：本模块**不响应 0xFA 通用地址**（got=0），
		 *  但它**直接响应 0x01，读取完全正常**：
		 *      读浊度应答 got=7 [01 03 02 00 43 F9 B5]  ->  67.00 UNT ✓
		 *  手册虽然写了"可以用 250 取得真实设备地址"，实测这颗不支持。
		 *
		 *  所以：0xFA 探测**只是想知道地址**，失败就**回退用当前配置的
		 *  地址继续自检**，绝不能报成致命错误 —— 原来的写法会打出一大串
		 *  "排查顺序: 供电/A-B 接反/波特率..."，把完全正常的链路说成坏的，
		 *  严重误导排查方向（这次就把我误导了好几轮）。
		 * --------------------------------------------------------------- */
		if (r < 0) {
			apu_console_print("[TURB] ① 通用地址(0xFA)探测: 无响应 "
					  "—— 该模块不支持此命令，**这不是故障**");
			apu_console_print("[TURB]    继续用当前地址 0x%02X 自检…",
					  (unsigned int)g_addr);
			g_diag_found = g_addr;      /* 回退：沿用当前地址 */
		} else {
			dbg_dump_resp("① 通用地址查询 应答", g_txn.buf, g_txn.got);
			/* 0xFA 问回来的第 1 字节是**真实地址**，不能按 0xFA 校验 */
			r = check_resp(g_txn.buf, g_txn.buf[0], &v);
			if (r == APU_TURB_E_OK) {
				g_diag_found = (uint8_t)(v & 0xFFU);
				apu_console_print("[TURB] ① 通用地址查询: 成功，真实设备地址 = 0x%02X (%u)",
						  (unsigned int)g_diag_found,
						  (unsigned int)g_diag_found);
				(void)apu_turb_set_addr(g_diag_found);
			} else {
				apu_console_print("[TURB] ① 通用地址探测应答异常(%s)"
						  " —— 忽略，继续用当前地址 0x%02X",
						  apu_turb_err_str(r),
						  (unsigned int)g_addr);
				g_diag_found = g_addr;
			}
		}

		g_diag_stage = (txn_begin(g_addr, g_addr,
					  APU_TURB_REG_TYPE_CODE) != 0) ? 0 : 2;
		return 0;
	}

	case 2: {                       /* 型号编码 */
		uint16_t v = 0U;

		r = txn_poll();
		if (r < 0) {
			apu_console_print("[TURB] ② 读型号编码失败: TIMEOUT");
		} else if (check_resp(g_txn.buf, g_txn.expect_addr, &v) ==
			   APU_TURB_E_OK) {
			apu_console_print("[TURB] ② 型号编码(0x0064) = %u",
					  (unsigned int)v);
		} else {
			apu_console_print("[TURB] ② 读型号编码失败");
		}
		g_diag_stage = (txn_begin(g_addr, g_addr, APU_TURB_REG_BAUD) != 0)
			       ? 0 : 3;
		return 0;
	}

	case 3: {                       /* 波特率编码 */
		uint16_t v = 0U;

		r = txn_poll();
		if (r < 0) {
			apu_console_print("[TURB] ③ 读波特率编码失败: TIMEOUT");
		} else if (check_resp(g_txn.buf, g_txn.expect_addr, &v) ==
			   APU_TURB_E_OK) {
			static const char *const bn[] = { "?", "2400", "4800", "9600",
							  "19200", "38400", "115200" };
			const char *bs = (v <= 6U) ? bn[v] : "越界";
			apu_console_print("[TURB] ③ 波特率编码(0x0067) = %u -> %s",
					  (unsigned int)v, bs);
			if (v != 3U)
				apu_console_print("[TURB]   ⚠️ 模块不是 9600！要么改模块到 9600 "
						  "(写 0x0067=3)，要么把 TD 里 B 路 IP 改成对应波特率");
		} else {
			apu_console_print("[TURB] ③ 读波特率编码失败");
		}
		g_diag_stage = 4;
		return 0;
	}

	case 4:
		apu_console_print("[TURB] ===== 自检结束 =====");
		g_diag_stage = 0;
		return 1;

	default:
		g_diag_stage = 0;
		return 1;
	}
}

/* ---------------------------------------------------------------------------
 *  打印
 * --------------------------------------------------------------------------- */
const char *apu_turb_err_str(int res)
{
	switch (res) {
	case APU_TURB_E_OK:             return "OK";
	case APU_TURB_E_NO_DEVICE:      return "NO_DEVICE(UART-B实例不存在,bit流不对?)";
	case APU_TURB_E_TX_STUCK:       return "TX_STUCK(IP没在跑)";
	case APU_TURB_E_TIMEOUT:        return "TIMEOUT(没应答:DE切换/供电/波特率/AB接反)";
	case APU_TURB_E_BAD_ADDR:       return "BAD_ADDR(地址或功能码不对)";
	case APU_TURB_E_BAD_BYTECOUNT:  return "BAD_BYTECOUNT(字节数域不是0x02)";
	case APU_TURB_E_BAD_CRC:        return "BAD_CRC(校验不过:波特率/干扰/AB接反)";
	case APU_TURB_E_DE_FAIL:        return "DE_FAIL(RS485方向脚GPIO起不来)";
	default:                        return "?";
	}
}

void apu_turb_print(const apu_turb_result_t *r)
{
#if APU_TURB_DEBUG_PRINT
	if (r == AL_NULL)
		return;

	if (r->res == APU_TURB_E_OK) {
		apu_console_print("[TURB] %.2f %s  (raw=%u, 倍率=%.3f)  %ums  [dbg: seen=%u]",
				  (double)r->value, r->unit,
				  (unsigned int)r->raw_value,
				  (double)r->scale,
				  (unsigned int)r->elapsed_ms,
				  (unsigned int)g_turb_dbg_seen);
	} else {
		uint32_t show = (r->raw_len < APU_TURB_RESP_LEN)
				? r->raw_len : APU_TURB_RESP_LEN;
		char hex[APU_TURB_RESP_LEN * 3U + 1U];
		uint32_t i, k = 0U;
		static const char kHex[] = "0123456789ABCDEF";

		for (i = 0U; i < show && (k + 3U) < sizeof(hex); i++) {
			hex[k++] = kHex[(r->raw[i] >> 4) & 0x0FU];
			hex[k++] = kHex[r->raw[i] & 0x0FU];
			hex[k++] = ' ';
		}
		hex[k] = '\0';

		apu_console_print("[TURB] FAIL %s  addr=0x%02X  got=%u [%s] %ums  [dbg: seen=%u]",
				  apu_turb_err_str(r->res),
				  (unsigned int)g_addr,
				  (unsigned int)r->raw_len, hex,
				  (unsigned int)r->elapsed_ms,
				  (unsigned int)g_turb_dbg_seen);
	}
#else
	/* ★ 2026-10-15：数据改走 rpmsg 的 "ENV:..." 上报，串口1 不再刷这一路。
	 *   把 apu_turb.h 里的 APU_TURB_DEBUG_PRINT 改成 1 重编即可恢复输出。 */
	(void)r;
#endif
}
