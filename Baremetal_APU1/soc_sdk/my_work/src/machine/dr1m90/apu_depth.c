/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_depth.c —— 下潜深度超声波驱动实现（直连 UART-C）
 * ===========================================================================
 *  协议、语义、接线全部写在 apu_depth.h 顶部。
 *
 *  ★★ 2026-10-14 重要修正：周期任务改成**非阻塞状态机** ★★
 *  ---------------------------------------------------------------------------
 *  【改之前错在哪】
 *    原 apu_depth_poll() 一次调用里把整帧收完，最长阻塞
 *    APU_DEPTH_FRAME_TIMEOUT_MS(60ms)。它和 platform_poll()（rpmsg 核间通信）
 *    在同一条主循环上，主循环本该 ~20µs 一轮。
 *    三路传感器阻塞叠加（60+300+600ms）→ Linux 侧报 **Connection reset**，
 *    APU1 日志 `[APU1][loop] step=1 iter=1 t=4946 ms`。
 *
 *  【现在怎么改】
 *        阶段 A：到点 → 清 RX、发触发字节、返回（微秒级）
 *        阶段 B：每轮只尝试收 1 个字节，收满 4 字节立即校验（微秒级）
 *
 *  · 收帧策略：先找帧头 0xFF（非帧头字节丢弃重来），再收剩下 3 字节。
 *  · 校验失败与"距离 0"严格区分（参考实现里两者都是 0，分不清）。
 * ===========================================================================
 */

#include "apu_depth.h"

#include "al_core.h"

#include "apu_pluart.h"
#include "apu_console.h"
#include "apu_time.h"

static uint32_t g_base;
static int      g_inited;

/* ---------------------------------------------------------------------------
 *  ★★ 2026-10-15 修正（第二版）：纯硬件轮询，**绝不调用 AlSys_UDelay** ★★
 *  ---------------------------------------------------------------------------
 *  【第一版错在哪】用 { getc; AlSys_UDelay(50); } 按 50µs 步进等 ——
 *      结果三路传感器全部退化成 got=0（上一版还能收到数据）。
 *      AlSys_UDelay 在 APU1 这条路径上不可靠，会把接收窗口整个吃掉。
 *
 *  【第二版】不用任何延时：UART-C 是 115200，一个字节仅约 87µs，
 *      SR 寄存器可无限次快读，收到即返回；退出只看毫秒超时。
 * --------------------------------------------------------------------------- */
#define APU_DEPTH_RECV_BUDGET_MS 100U   /* 单次调用的硬上界（一圈约1000ms，占比10%） */

/* 诊断计数：本轮收帧过程中，总共从 FIFO 取出过多少个字节（不做任何过滤）。
 * 用途：区分三种情况 ——
 *   0        : 一个字节都没收到 → 信号没进 FPGA（接线/供电/电平）
 *   >0 但不合帧: 收到了但拼不成 0xFF 开头的帧 → 波特率或协议不对
 *   =4 且正确 : 正常
 * ⚠️ 这个计数只在深度这一路加，用于定位问题，定位完可以删。 */
static uint32_t g_dbg_bytes_seen;
static uint32_t g_dbg_flush_seen;   /* 发触发之前清 FIFO 时清掉了几个字节 */

/* ---------------------------------------------------------------------------
 *  初始化
 * --------------------------------------------------------------------------- */
int apu_depth_init(void)
{
	g_base = apu_pluart_base(APU_PLUART_ULTRA);
	if (g_base == 0U) {
		g_inited = 0;
		return -1;
	}
	apu_pluart_rx_flush(g_base);
	g_inited = 1;
	return 0;
}

static void result_reset(apu_depth_result_t *out)
{
	int i;

	out->res         = APU_DEPTH_E_OK;
	out->distance_mm = 0U;
	out->raw_len     = 0U;
	out->elapsed_ms  = 0U;
	for (i = 0; i < 4; i++)
		out->raw[i] = 0U;
}

/* 清 RX FIFO 并统计清掉了多少字节（诊断用）。
 * 如果每次都清出好几个 0x00，说明 RX 线被拉在低电平（例如传感器没上电）。 */
static void rx_flush_counted(void)
{
	uint8_t  c;
	uint32_t n = 0U;

	apu_pluart_rx_flush(g_base);
	while (apu_pluart_getc(g_base, &c) && n < 64U)
		n++;
	g_dbg_flush_seen = n;
}

/* 发触发字节。返回 0 成功，否则错误码。 */
static int send_trigger(void)
{
	uint8_t  trig = APU_DEPTH_TRIGGER_BYTE;
	uint32_t sent;
	int      stuck = 0;

	rx_flush_counted();
	sent = apu_pluart_write(g_base, &trig, 1U, &stuck);
	if (stuck != 0 || sent != 1U)
		return APU_DEPTH_E_TX_STUCK;
	return 0;
}

/* ---------------------------------------------------------------------------
 *  校验 + 解析一条已收齐的 4 字节帧
 * --------------------------------------------------------------------------- */
static void parse_frame(apu_depth_result_t *out)
{
	uint8_t sum;

	if (out->raw_len < APU_DEPTH_FRAME_LEN) {
		out->res = APU_DEPTH_E_TIMEOUT_REST;
		return;
	}
	sum = (uint8_t)((uint8_t)(out->raw[0] + out->raw[1]) + out->raw[2]);
	if (sum != out->raw[3]) {
		out->res = APU_DEPTH_E_BAD_SUM;
		return;
	}
	out->distance_mm = (uint16_t)(((uint16_t)out->raw[1] << 8) |
				      (uint16_t)out->raw[2]);
	out->res = (out->distance_mm == APU_DEPTH_INVALID_MM)
		   ? APU_DEPTH_E_INVALID
		   : APU_DEPTH_E_OK;
}

/* ---------------------------------------------------------------------------
 *  ★ 非阻塞一步
 *  返回 0 = 进行中；1 = 本次完成（结果在 *out）；-1 = 起不来
 * --------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 *  ★ 一次同步收发（发触发 + 收完 4 字节）
 *
 *  ★★ 2026-10-15 修 bug（我自己引入的）★★
 *  ---------------------------------------------------------------------------
 *  【原来的错】
 *      原来是两阶段的 depth_step()：第一次调用只"发触发"就 return 0
 *      （表示未完成），第二次调用才收帧。但 apu_depth_poll() 里写的是
 *          (void)depth_step(&r);
 *          apu_depth_print(&r);      ← 不管完成没完成，都打印
 *      于是：
 *        · 这一圈：发触发→return 0→**立刻打印刚被清空的 r**
 *          ⇒ 日志里那条 `0 mm raw=00 00 00 00 0ms [dbg: seen=0]`
 *            根本**不是真实读数**，是"打印了空结构"
 *        · 下一圈：真正收帧，打印的却是上一轮的旧 r ⇒ 错位
 *      ⇒ 上板日志里"0mm 与 1500ms 交替"就是这两个阶段轮流打印造成的假象。
 *
 *  【现在】合成一个同步函数：发完就在同一次调用里把帧收完，
 *      只有"收到完整帧"或"确定超时"两种返回，绝不会打印半成品。
 * --------------------------------------------------------------------------- */
static void depth_read_now(apu_depth_result_t *out)
{
	uint32_t t_start;
	uint8_t  b;
	int      r;

	result_reset(out);

	if (!g_inited || g_base == 0U) {
		out->res = APU_DEPTH_E_NO_DEVICE;
		return;
	}

	/* ---- 发触发字节 ---- */
	g_dbg_bytes_seen = 0U;
	t_start = apu_time_ms_live();
	r = send_trigger();
	if (r != 0) {
		out->res        = r;
		out->elapsed_ms = 0U;
		return;
	}

	/* ---- 在同一次调用里连续抽干 FIFO，直到收满或超预算 ---- */
	while (out->raw_len < APU_DEPTH_FRAME_LEN) {
		if ((uint32_t)(apu_time_ms_live() - t_start) >=
		    APU_DEPTH_RECV_BUDGET_MS) {
			out->elapsed_ms = (uint32_t)(apu_time_ms_live() - t_start);
			out->res = (out->raw_len == 0U)
				   ? APU_DEPTH_E_TIMEOUT_HEAD
				   : APU_DEPTH_E_TIMEOUT_REST;
			return;
		}

		/* 纯寄存器轮询，不用任何延时函数 */
		if (!apu_pluart_getc(g_base, &b))
			continue;

		g_dbg_bytes_seen++;     /* ★ 诊断：不过滤，先记下"确实收到过字节" */

		if (out->raw_len == 0U) {
			/* 找帧头：不是 0xFF 就丢掉继续找 */
			if (b != APU_DEPTH_FRAME_HEADER)
				continue;
		}
		out->raw[out->raw_len++] = b;
	}

	out->elapsed_ms = (uint32_t)(apu_time_ms_live() - t_start);
	parse_frame(out);
}

/* 本次读取是不是"物理层压根没收到字节"（= 值得重试一次）。
 * 只有这两种才是传输层失败；OK / INVALID / BAD_SUM 都说明**帧回来了**，
 * 是有效的一次测量结果，重试没有意义（重试也只会拿到同样的东西）。 */
static int res_is_transport_fail(int res)
{
	return (res == APU_DEPTH_E_TIMEOUT_HEAD) ||
	       (res == APU_DEPTH_E_TIMEOUT_REST);
}

/* ---------------------------------------------------------------------------
 *  ★★ 2026-10-15 对外读取接口（重写）★★
 *  ---------------------------------------------------------------------------
 *  【重写前的严重 bug —— 真实距离永远到不了上报层】
 *
 *      旧实现：
 *          for (;;) {
 *              if (depth_step(out) != 0) return 0;      // 只有 NO_DEVICE 才非0
 *              if (now - t0 > FRAME_TIMEOUT + 20) {
 *                  out->res = APU_DEPTH_E_TIMEOUT_HEAD; // ← 无条件覆盖！
 *                  return 0;
 *              }
 *          }
 *
 *      两处叠加的错：
 *        ① depth_step() 对 OK / INVALID / BAD_SUM **一律返回 0** ⇒ 循环不退出，
 *           即使已经解析出 178mm 也继续重试，白烧掉约 120ms；
 *        ② 超时后**无条件**把 res 覆盖成 TIMEOUT_HEAD
 *           ⇒ 最后一次的真实解析结果被丢弃。
 *
 *      症状（上板日志原文）：
 *          [DEPTH] FAIL TIMEOUT_HEAD(没收到帧头0xFF) got=4 [FF 00 B2 B1] 16ms
 *        —— 自相矛盾：报"没收到帧头"，可手里明明攥着 4 个字节，且首字节就是 0xFF。
 *        本质是：**传感器送回了真实距离(0x00B2=178mm)，但被这个循环丢掉了**，
 *        于是 Web 侧只看到 DEPTHV=0 / DEPTH=0。
 *
 *  【现在的策略】
 *      · 首轮就拿到"帧回来了"的结果（OK/INVALID/BAD_SUM）⇒ **立即返回**，
 *        绝不因为"值不理想"而重试（那只会把好数据换成超时）。
 *      · 只有物理层没收到字节（TIMEOUT_*）才重试一次，给干扰一次机会。
 *      · 若重试后仍失败，**保留倒数第二次的真实错误**优先上报：
 *        只要在整轮里收到过帧，就把那个解析结果报出去，
 *        而不是退化成笼统的 TIMEOUT_HEAD。
 * --------------------------------------------------------------------------- */
int apu_depth_read(apu_depth_result_t *out)
{
	uint32_t t0 = apu_time_ms_live();
	apu_depth_result_t best = { 0 };
	int have_best = 0;
	int attempt;

	if (out == AL_NULL)
		return -1;

	for (attempt = 0; attempt < 2; attempt++) {
		depth_read_now(out);

		/* 设备起不来（bit 流不对/UART-C 实例不存在）：立即上报，不重试 */
		if (out->res == APU_DEPTH_E_NO_DEVICE)
			return 0;

		/* ★ 帧回来了（哪怕是 INVALID/BAD_SUM）⇒ 这就是本次测量结果，立即返回。
		 *   不要重试 —— 重试会把"178mm"换成"TIMEOUT"，正是旧 bug 的成因。 */
		if (!res_is_transport_fail(out->res))
			return 0;

		/* 传输层失败：记下（优先保留非传输层的）上一次结果，再试一次 */
		if (!have_best) {
			best      = *out;
			have_best = 1;
		}
	}

	/* 两次都没收到字节：把最好的一次结果报出去。
	 * ⚠️ 这里**不再覆盖 res**（旧 bug 就是无条件覆盖）。 */
	if (have_best)
		*out = best;

	(void)t0;
	return 0;
}

/* ---------------------------------------------------------------------------
 *  错误串 / 打印
 * --------------------------------------------------------------------------- */
const char *apu_depth_err_str(int res)
{
	switch (res) {
	case APU_DEPTH_E_OK:           return "OK";
	case APU_DEPTH_E_NO_DEVICE:    return "NO_DEVICE(UART-C 实例不存在,bit流不对?)";
	case APU_DEPTH_E_TX_STUCK:     return "TX_STUCK(IP没在跑)";
	case APU_DEPTH_E_TIMEOUT_HEAD: return "TIMEOUT_HEAD(一个字节都没收到:接线/供电/电平)";
	case APU_DEPTH_E_TIMEOUT_REST: return "TIMEOUT_REST(帧没收齐:只到一半就停了)";
	case APU_DEPTH_E_BAD_SUM:      return "BAD_SUM(校验和不匹配)";
	case APU_DEPTH_E_INVALID:      return "INVALID(65535丢波/失效)";
	default:                       return "?";
	}
}

void apu_depth_print(const apu_depth_result_t *r)
{
#if APU_DEPTH_DEBUG_PRINT
	if (r == AL_NULL)
		return;

	if (r->res == APU_DEPTH_E_OK) {
		apu_console_print("[DEPTH] %5u mm   raw=%02X %02X %02X %02X  %ums"
				  "  [dbg: flushed=%u seen=%u]",
				  (unsigned int)r->distance_mm,
				  r->raw[0], r->raw[1], r->raw[2], r->raw[3],
				  (unsigned int)r->elapsed_ms,
				  (unsigned int)g_dbg_flush_seen,
				  (unsigned int)g_dbg_bytes_seen);
	} else {
		apu_console_print("[DEPTH] FAIL %s   got=%u [%02X %02X %02X %02X] %ums"
				  "  [dbg: flushed=%u seen=%u]",
				  apu_depth_err_str(r->res),
				  (unsigned int)r->raw_len,
				  r->raw[0], r->raw[1], r->raw[2], r->raw[3],
				  (unsigned int)r->elapsed_ms,
				  (unsigned int)g_dbg_flush_seen,
				  (unsigned int)g_dbg_bytes_seen);
	}
#else
	/* ★ 2026-10-15：数据改走 rpmsg 的 "ENV:..." 上报（apu_env.c），
	 *   串口1 不再刷这一路。把 apu_depth.h 里的 APU_DEPTH_DEBUG_PRINT
	 *   改成 1 重编即可恢复本函数全部输出。 */
	(void)r;
#endif
}

/* ---------------------------------------------------------------------------
 *  ★★ 周期任务（挂主循环）★★
 *
 *  设计约束与 apu_gas.c 相同：主循环约 1000ms 一圈，而 115200 下 4 字节帧
 *  只要 0.35ms 到齐 ⇒ 必须**当次收完**，不能摊到多圈。
 *  到点时会阻塞最多 APU_DEPTH_RECV_BUDGET_MS(100ms)，是**故意的**：
 *  100ms / 1000ms = 10%，不会饿死 rpmsg，换来的是真能收到帧。
 *  ⚠️ 主循环里用轮转（main.c），保证同一圈只有一路传感器在收发。
 * --------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 *  ★★ 周期任务（**独立调试专用**）★★
 * ---------------------------------------------------------------------------
 *  ⚠️ 2026-10-15 架构调整后，**生产路径不再调用本函数**：
 *      三路的采集调度 + rpmsg 上报统一由 apu_env.c 负责（它调 apu_depth_read()）。
 *      本函数保留仅用于单独排查下潜超声（配合 APU_DEPTH_DEBUG_PRINT=1）。
 *      若误在生产路径上调用它，会与 apu_env 重复收发同一串口。
 * --------------------------------------------------------------------------- */
#if APU_DEPTH_DEBUG_PRINT
void apu_depth_poll(void)
{
	static uint32_t last_ms;
	static int      first = 1;
	static apu_depth_result_t r;

	uint32_t now = apu_time_ms_live();

	/* 没到周期：立刻返回 */
	if (!first && (uint32_t)(now - last_ms) < APU_DEPTH_PERIOD_MS)
		return;
	first   = 0;
	last_ms = now;

	/* 当场同步收发一次，拿到**完整**结果才打印
	 * （原来直接调分阶段的 depth_step 并立刻打印，会打出刚被清空的
	 *   结构体 —— 那就是日志里 `0 mm raw=00 00 00 00 0ms` 的来源） */
	depth_read_now(&r);
	apu_depth_print(&r);
}
#endif /* APU_DEPTH_DEBUG_PRINT */
