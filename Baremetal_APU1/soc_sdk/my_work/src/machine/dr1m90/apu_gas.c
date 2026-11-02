/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_gas.c —— 气体传感器驱动实现（Modbus-RTU，直连 UART-A）
 * ===========================================================================
 *  协议、寄存器映射、状态字全部写在 apu_gas.h 顶部。这里讲实现。
 *
 *  ★★ 2026-10-14 重要修正：改成**非阻塞状态机** ★★
 *  ---------------------------------------------------------------------------
 *  【改之前错在哪 —— 上板实测踩到了】
 *    原实现是"一次调用里把整帧收完"，最长阻塞 APU_GAS_FRAME_TIMEOUT_MS(300ms)。
 *    它挂在主循环里，和 platform_poll()（rpmsg 核间通信）同一条循环：
 *
 *        platform_poll()      <- rpmsg，必须及时
 *        ...
 *        apu_gas_poll()       <- 阻塞 300ms
 *        apu_turb_poll()      <- 阻塞 600ms
 *
 *    三路加起来单轮最多阻塞 60+300+600 = 960ms。而主循环本来就该是
 *    **~20µs 一轮**（apu_time.h:53 明确写了）。后果：
 *      · Linux 踢过来的 SGI / rpmsg 消息长时间没人处理
 *      · 上板现象：Linux 侧报 **Connection reset**；
 *        APU1 日志 `[APU1][loop] step=1 iter=1 t=4946 ms` —— 第一轮就花了 4.9 秒
 *
 *  【现在怎么改】
 *    拆成两个阶段，每轮主循环只做**其中一件极小的事**：
 *        阶段 A：到点了 → 发问询帧，立刻返回（微秒级）
 *        阶段 B：发完之后 → 每轮只尝试收 **1 个字节**，收齐立即结束（微秒级）
 *    单轮开销 = 几微秒，主循环恢复正常节奏，rpmsg 不再被饿死。
 *
 *    ⚠️ 阶段 B 要等"整帧收齐"才算完。Modbus 帧是连续的一串，
 *       IP 的 16 字节 FIFO 足够装下本条应答（气体 25 字节？见下），
 *       所以按"收满期望长度"作为完成条件，不需要判 3.5 字符静默间隔。
 *       —— 气体应答是 25 字节 > FIFO 16 字节，但因为我们在循环里持续取，
 *          FIFO 不会溢出的前提下每次只取一个也不会丢。
 *
 *  · 其余不变：每帧开始前清 RX FIFO；CRC16 已用规格书向量验证。
 * ===========================================================================
 */

#include "apu_gas.h"

#include "al_core.h"

#include "apu_console.h"
#include "apu_time.h"

static uint32_t g_base;
static int      g_inited;
static uint8_t  g_addr = APU_GAS_SLAVE_ADDR;

/* ---------------------------------------------------------------------------
 *  ★★ 2026-10-15 修正（第二版）：纯硬件轮询，**绝不调用 AlSys_UDelay** ★★
 *  ---------------------------------------------------------------------------
 *  【第一版错在哪 —— 上板实测的回归】
 *      第一版我写的是「循环 { getc; AlSys_UDelay(50); }」，想按 50µs 步进等。
 *      结果三路传感器**全部**从"能收到数据"退化成 got=0：
 *          上一版(00:18:57)：气体 got=16 真实数据、浑浊度地址查询成功
 *          第一版(19:27:26)：气体 got=0、浑浊度 got=0、下潜 got=0
 *      ⇒ AlSys_UDelay 在 APU1 这条路径上不可靠（延时远超预期或行为异常），
 *        把接收窗口整个吃掉了。
 *
 *  【第二版怎么改】
 *      干脆**不用任何延时**：AXI UART Lite 的 SR 寄存器可以无限次快速读，
 *      收到字节就立刻返回；没收到就空转若干次再看一次。
 *      退出条件只看"毫秒超时"（apu_time_ms_live），不依赖任何睡眠函数。
 *
 *      APU_GAS_SPIN_PASSES 是"一个字节约 1.04ms（9600）"对应的空转次数，
 *      给足余量。即使这个常数不准，也只是多转几圈 —— 不会像 UDelay 那样
 *      把时间窗口吃掉。
 * --------------------------------------------------------------------------- */
/* 接收参数见下面 gas_read_now() 里的 APU_GAS_RECV_BUDGET_MS */

/* ---------------------------------------------------------------------------
 *  CRC16 / Modbus（验证向量 01 03 00 00 00 0A -> C5 CD ✓）
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

/* 气体类型编号 -> 名字（规格书表 3-6，逐项核对过；58 号规格书缺失） */
static const char *const g_gas_names[83] = {
	/* 0  */ "NULL",  "AR",    "ASH3",  "B2H6",  "BR2",   "CO",    "CO2",   "COCL2",
	/* 8  */ "CH2O",  "CH2O2", "CH3BR", "CH4",   "CH4O",  "CH4S",  "CH5N",  "CH6O",
	/* 16 */ "CIC",   "CL2",   "CLO2",  "C2CL4", "C2HCL3","C2H2",  "C2H3CL","C2H",
	/* 24 */ "C2H4O", "C2H6O", "C3H3N", "C3H6O", "C3H8",  "C3H8O", "C4H8O2","C4H8S",
	/* 32 */ "C4H10", "C4H10O","C5H12", "C6H6",  "C6H6S", "C6H12", "C6H14", "C7H8",
	/* 40 */ "C7H16", "C8H8",  "C8H10", "C8H18", "CS2",   "EX",    "ETO",   "F2",
	/* 48 */ "FX",    "GEH4",  "H2",    "H2O2",  "H2S",   "HCL",   "HCN",   "HBR",
	/* 56 */ "HE",    "HF",    "?",     "NO",    "NO2",   "NOX",   "NF3",   "NH3",
	/* 64 */ "N2",    "N2O",   "N2H4",  "O2",    "O3",    "PH3",   "PID",   "P2O5",
	/* 72 */ "SO2",   "SO2F2", "SIH4",  "SIF4",  "SF6",   "THT",   "TVOC",  "VOC",
	/* 80 */ "VOCS",  "SO3",   "NMHC"
};

static const char *gas_name_of(int t)
{
	if (t < 0 || t > 82)
		return NULL;
	return g_gas_names[t];
}

static const char *status_name_of(int s)
{
	switch (s) {
	case 0x00: return "预热";
	case 0x01: return "正常";
	case 0x02: return "数据错误";
	case 0x03: return "传感器故障";
	case 0x04: return "预警";
	case 0x05: return "低报";
	case 0x06: return "高报";
	case 0x07: return "访问故障";
	case 0x08: return "超量程";
	case 0x09: return "需要标定";
	case 0x0A: return "超时";
	case 0x0B: return "STEL报警";
	case 0x0C: return "TWA报警";
	case 0x0F: return "通信故障";
	default:   return "保留/未知";
	}
}

static const char *unit_of(int code)
{
	switch (code) {
	case 0x0: return "ppm";
	case 0x2: return "%LEL";
	case 0x4: return "%VOL";
	case 0x6: return "mg/m3";
	case 0x8: return "ppb";
	default:  return "?";
	}
}

/* 把结果结构体清成"没读到" */
static void result_reset(apu_gas_result_t *out)
{
	int i;

	out->res         = APU_GAS_E_OK;
	out->raw_len     = 0U;
	out->elapsed_ms  = 0U;
	out->conc        = 0.0f;
	out->unit        = "?";
	out->decimals    = 0;
	out->gas_type    = -1;
	out->gas_name    = NULL;
	out->status      = 0;
	out->status_name = "?";
	for (i = 0; i < 10; i++)
		out->reg[i] = 0U;
	for (i = 0; i < (int)APU_GAS_RESP_LEN; i++)
		out->raw[i] = 0U;
}

/* 组问询帧并发出。返回 0 成功；负数为错误码。 */
static int send_request(void)
{
	uint8_t  req[APU_GAS_REQ_LEN];
	uint16_t crc;
	uint32_t sent;
	int      stuck = 0;

	req[0] = g_addr;
	req[1] = APU_GAS_FUNC_READ;
	req[2] = (uint8_t)(APU_GAS_REG_START >> 8);
	req[3] = (uint8_t)(APU_GAS_REG_START & 0xFFU);
	req[4] = (uint8_t)(APU_GAS_REG_COUNT >> 8);
	req[5] = (uint8_t)(APU_GAS_REG_COUNT & 0xFFU);
	crc    = crc16_modbus(req, 6U);
	req[6] = (uint8_t)(crc & 0xFFU);
	req[7] = (uint8_t)((crc >> 8) & 0xFFU);

	apu_pluart_rx_flush(g_base);
	sent = apu_pluart_write(g_base, req, APU_GAS_REQ_LEN, &stuck);
	if (stuck != 0 || sent != APU_GAS_REQ_LEN)
		return APU_GAS_E_TX_STUCK;
	return 0;
}

/* ---------------------------------------------------------------------------
 *  校验 + 解析一条已收齐的应答
 * --------------------------------------------------------------------------- */
static void parse_response(apu_gas_result_t *out)
{
	uint16_t crc;
	int      i;

	if (out->raw_len < APU_GAS_RESP_LEN) {
		out->res = APU_GAS_E_TIMEOUT;
		return;
	}
	if (out->raw[0] != g_addr || out->raw[1] != APU_GAS_FUNC_READ) {
		out->res = APU_GAS_E_BAD_ADDR;
		return;
	}
	if (out->raw[2] != (uint8_t)(APU_GAS_REG_COUNT * 2U)) {
		out->res = APU_GAS_E_BAD_BYTECOUNT;
		return;
	}
	crc = crc16_modbus(out->raw, APU_GAS_RESP_LEN - 2U);
	if (((uint8_t)(crc & 0xFFU) != out->raw[APU_GAS_RESP_LEN - 2U]) ||
	    ((uint8_t)((crc >> 8) & 0xFFU) != out->raw[APU_GAS_RESP_LEN - 1U])) {
		out->res = APU_GAS_E_BAD_CRC;
		return;
	}

	for (i = 0; i < 10; i++)
		out->reg[i] = (uint16_t)(((uint16_t)out->raw[3 + 2 * i] << 8) |
					 (uint16_t)out->raw[4 + 2 * i]);

	{
		uint16_t meta = out->reg[0];
		int      unit_code = (int)((meta >> 12) & 0x0FU);
		int      nd    = (int)((meta >> 8) & 0x0FU) / 4;
		/* ⚠️ 规格书自相矛盾：位定义表说 bit6-7 是浓度第17/18位，
		 *    寄存器映射表却把寄存器0 的 bit7-0 写成"保留位"。
		 *    这里按位定义表处理；若实测浓度偏大很多，把 hi 改成 0 即可。 */
		uint32_t hi    = (uint32_t)((meta >> 6) & 0x03U);
		uint32_t conc_raw = ((hi << 16) | (uint32_t)out->reg[1]);
		float    scale = 1.0f;

		if (nd < 0) nd = 0;
		if (nd > 3) nd = 3;
		for (i = 0; i < nd; i++)
			scale *= 10.0f;

		out->decimals = nd;
		out->unit     = unit_of(unit_code);
		out->conc     = (float)conc_raw / scale;

		out->gas_type = (int)((out->reg[8] >> 8) & 0xFFU);
		out->gas_name = gas_name_of(out->gas_type);

		out->status      = (int)(out->reg[5] & 0xFFU);
		out->status_name = status_name_of(out->status);
	}
	out->res = APU_GAS_E_OK;
}

/* ---------------------------------------------------------------------------
 *  ★★ 同步收一帧（**当次调用内收完**，不跨主循环圈）★★
 *
 *  【为什么必须是"当次收完"—— 两个实测事实决定的】
 *    事实①：本工程主循环约 **1000ms 一圈**（日志 t= 每圈涨 1000，
 *           而 apu_time.h:53 原本期望 ~20µs）。这一圈的耗时来自
 *           platform_poll()/apu_app_periodic() 内部，不是传感器造成的。
 *    事实②：9600 下 25 字节应答只花 **26ms** 就全部到齐，
 *           而 IP 的 RX FIFO 只有 **16 字节**。
 *    ⇒ 若把接收摊到多圈，帧内剩下的字节要在 FIFO 里干等近 1 秒，
 *      第 17 字节起必然被硬件丢弃 —— 实测 got=16 正是这个数。
 *    ⇒ 所以只能在**一次调用里连续把 FIFO 抽干**。
 *
 *  【为什么这样做不会再把 rpmsg 饿死（即不会再 Connection reset）】
 *    · 本函数自身有硬上界：APU_GAS_RECV_BUDGET_MS(100ms)，到点必返回；
 *    · 主循环里加了**轮转**（见 main.c），一圈只让**一个**传感器做收发，
 *      三路不会叠加；
 *    · 最坏 = 一圈里多花 100ms，而一圈本来就是 1000ms ⇒ 占比 10%。
 *    对比出事那版：三路**每圈都等满** 60+300+600=960ms，且是每圈必等。
 * --------------------------------------------------------------------------- */
#define APU_GAS_RECV_BUDGET_MS   100U   /* 单次调用的硬上界 */

static void gas_read_now(apu_gas_result_t *out)
{
	uint32_t t_start, t_frame;
	uint8_t  b;
	int      r;

	result_reset(out);

	if (!g_inited || g_base == 0U) {
		out->res = APU_GAS_E_NO_DEVICE;
		return;
	}

	/* ---- 发问询 ---- */
	t_start = apu_time_ms_live();
	r = send_request();
	if (r != 0) {
		out->res        = r;
		out->elapsed_ms = 0U;
		return;
	}

	/* ---- 连续抽干 FIFO，直到收满 25 字节或超预算 ---- */
	t_frame = t_start;
	while (out->raw_len < APU_GAS_RESP_LEN) {
		/* 硬上界：绝不把主循环拖过预算 */
		if ((uint32_t)(apu_time_ms_live() - t_start) >=
		    APU_GAS_RECV_BUDGET_MS) {
			out->elapsed_ms = (uint32_t)(apu_time_ms_live() - t_start);
			out->res        = APU_GAS_E_TIMEOUT;
			return;
		}

		/* 纯寄存器轮询，不用任何延时函数 */
		if (apu_pluart_getc(g_base, &b)) {
			out->raw[out->raw_len++] = b;
			t_frame = apu_time_ms_live();   /* 有进展，刷新帧内计时 */
			continue;
		}

		/* 帧内停顿超过 APU_GAS_FRAME_TIMEOUT_MS 且已有字节
		 *  → 认为本帧已结束（不全），提前收尾，别白等 */
		if (out->raw_len > 0U &&
		    (uint32_t)(apu_time_ms_live() - t_frame) >=
		    APU_GAS_FRAME_TIMEOUT_MS) {
			out->elapsed_ms = (uint32_t)(apu_time_ms_live() - t_start);
			out->res        = APU_GAS_E_TIMEOUT;
			return;
		}
	}

	out->elapsed_ms = (uint32_t)(apu_time_ms_live() - t_start);
	parse_response(out);
}

/* ---------------------------------------------------------------------------
 *  ★ 对外：一次完整读取（同步版，自检/单发用）
 *  内部就是 gas_read_now()，有 APU_GAS_RECV_BUDGET_MS(100ms) 硬上界。
 *  主循环请用 apu_gas_poll()（它会自己做周期判断）。
 * --------------------------------------------------------------------------- */
int apu_gas_read(apu_gas_result_t *out)
{
	if (out == AL_NULL)
		return -1;

	gas_read_now(out);
	if (out->res == APU_GAS_E_NO_DEVICE)
		return -1;
	return 0;
}

/* ---------------------------------------------------------------------------
 *  初始化
 * --------------------------------------------------------------------------- */
int apu_gas_init(void)
{
	g_base = apu_pluart_base(APU_PLUART_GAS);
	if (g_base == 0U) {
		g_inited = 0;
		return -1;
	}
	apu_pluart_rx_flush(g_base);
	g_inited = 1;
	return 0;
}

int apu_gas_set_addr(uint8_t addr)
{
	if (addr == 0U)
		return -1;
	g_addr = addr;
	return 0;
}

uint8_t apu_gas_get_addr(void)
{
	return g_addr;
}

/* ---------------------------------------------------------------------------
 *  打印
 * --------------------------------------------------------------------------- */
const char *apu_gas_err_str(int res)
{
	switch (res) {
	case APU_GAS_E_OK:              return "OK";
	case APU_GAS_E_NO_DEVICE:       return "NO_DEVICE(UART-A实例不存在,bit流不对?)";
	case APU_GAS_E_TX_STUCK:        return "TX_STUCK(IP没在跑)";
	case APU_GAS_E_TIMEOUT:         return "TIMEOUT(没应答/没收齐)";
	case APU_GAS_E_SHORT:           return "SHORT(长度不对)";
	case APU_GAS_E_BAD_ADDR:        return "BAD_ADDR(地址或功能码不对)";
	case APU_GAS_E_BAD_CRC:         return "BAD_CRC(校验不过:查波特率/接线/干扰)";
	case APU_GAS_E_BAD_BYTECOUNT:   return "BAD_BYTECOUNT(字节数域不是0x14)";
	default:                        return "?";
	}
}

void apu_gas_print(const apu_gas_result_t *r)
{
#if APU_GAS_DEBUG_PRINT
	if (r == AL_NULL)
		return;

	if (r->res == APU_GAS_E_OK) {
		apu_console_print("[GAS] %.3f %s  %s  气体=0x%02X%s  T=%.1f  H=%.1f  %ums",
				  (double)r->conc, r->unit, r->status_name,
				  (unsigned int)r->gas_type,
				  (r->gas_name != NULL) ? r->gas_name : "",
				  (double)r->reg[7] / 10.0,
				  (double)r->reg[9] / 10.0,
				  (unsigned int)r->elapsed_ms);
		apu_console_print("[GAS]   regs: %04X %04X %04X %04X %04X "
				  "%04X %04X %04X %04X %04X",
				  r->reg[0], r->reg[1], r->reg[2], r->reg[3], r->reg[4],
				  r->reg[5], r->reg[6], r->reg[7], r->reg[8], r->reg[9]);
	} else {
		uint32_t show = (r->raw_len < APU_GAS_RESP_LEN)
				? r->raw_len : APU_GAS_RESP_LEN;
		char hex[APU_GAS_RESP_LEN * 3U + 1U];
		uint32_t i, k = 0U;
		static const char kHex[] = "0123456789ABCDEF";

		for (i = 0U; i < show && (k + 3U) < sizeof(hex); i++) {
			hex[k++] = kHex[(r->raw[i] >> 4) & 0x0FU];
			hex[k++] = kHex[r->raw[i] & 0x0FU];
			hex[k++] = ' ';
		}
		hex[k] = '\0';

		apu_console_print("[GAS] FAIL %s  addr=0x%02X  got=%u [%s] %ums",
				  apu_gas_err_str(r->res),
				  (unsigned int)g_addr,
				  (unsigned int)r->raw_len, hex,
				  (unsigned int)r->elapsed_ms);
	}
#else
	/* ★ 2026-10-15：数据改走 rpmsg 的 "ENV:..." 上报，串口1 不再刷这一路。 */
	(void)r;
#endif
}

/* ===========================================================================
 *  ★★ 兼容协议（0x86）—— 四路气体一次读回 ★★
 * ===========================================================================
 *  协议说明、帧格式、单位问题全部写在 apu_gas.h 顶部。这里讲实现要点：
 *
 *  · 请求帧是**固定 9 字节**（没有从机地址概念），校验和 =
 *        (uint8_t)(~(byte1+…+byte7) + 1)
 *    实测 01+86 = 0x87 → ~0x87 = 0x78 → +1 = 0x79 ✓ 与手册一致。
 *
 *  · 应答是**固定 11 字节**，但**只收 10 字节**（0xFF + 9 个数据/命令），
 *    校验字节单独再看一眼：
 *        —— 因为手册的校验和公式自相矛盾（"取反(Byte1..Byte7)+1"覆盖不到
 *           Byte8/Byte9 两个 CH4 字节；C 例程又用 char 累加会溢出）。
 *           所以**校验失败只告警、不丢数据**：前 10 字节的结构（0xFF/0x86
 *           开头 + 4 组大端浓度）本身就是自洽的，实测能稳定复现。
 *           等你确认了正确公式，把 APU_GAS_C_STRICT_SUM 改成 1 即可严格校验。
 *
 *  · 收发沿用 Modbus 那套"当次调用内连续抽干 FIFO"（主循环约 1 秒一圈，
 *    而 11 字节 @9600 只需 11.5ms，跨圈必然 FIFO 溢出）。
 * ===========================================================================
 */
#define APU_GAS_C_STRICT_SUM    0       /* 1 = 校验不符就判失败（默认宽容） */

/* 兼容协议请求帧的校验和：(取反(byte1..byte7) + 1) */
static uint8_t compat_checksum(const uint8_t *buf)
{
	uint8_t s = 0U;
	int     i;

	for (i = 1; i <= 7; i++)
		s = (uint8_t)(s + buf[i]);
	return (uint8_t)((uint8_t)(~s) + 1U);
}

/* 校验应答的校验值。返回 1 = 一致。
 * ⚠️ 公式有两个候选（手册文字版 / C 例程版），两个都试，任一命中就算过。 */
static int compat_sum_ok(const uint8_t *r, uint8_t got, uint8_t *calc_out)
{
	uint8_t s_text = 0U;    /* 文字版：取反(Byte1..Byte7)+1 */
	uint8_t s_data = 0U;    /* 数据版：取反(Byte1..Byte9)+1（含全部数据字节） */
	int     i;

	for (i = 1; i <= 7; i++)
		s_text = (uint8_t)(s_text + r[i]);
	for (i = 1; i <= 9; i++)
		s_data = (uint8_t)(s_data + r[i]);

	{
		uint8_t c_text = (uint8_t)((uint8_t)(~s_text) + 1U);
		uint8_t c_data = (uint8_t)((uint8_t)(~s_data) + 1U);

		if (calc_out != AL_NULL)
			*calc_out = c_data;     /* 打印用：给出数据版 */
		return (got == c_text) || (got == c_data);
	}
}

/* 一次同步收发（兼容协议）。返回 0 = 拿到完整 11 字节 */
static void compat_read_now(apu_gas_all_t *out)
{
	uint8_t  req[APU_GAS_C_REQ_LEN];
	uint32_t t0, sent;
	uint8_t  b;
	int      stuck = 0, i;

	out->res        = APU_GAS_E_OK;
	out->raw_len    = 0U;
	out->co         = 0U;
	out->h2s        = 0U;
	out->o2         = 0U;
	out->ch4        = 0U;
	out->sum_got    = 0U;
	out->sum_calc   = 0U;
	out->sum_ok     = 0;
	out->elapsed_ms = 0U;
	for (i = 0; i < (int)APU_GAS_C_RESP_LEN; i++)
		out->raw[i] = 0U;

	if (!g_inited || g_base == 0U) {
		out->res = APU_GAS_E_NO_DEVICE;
		return;
	}

	/* ---- 组请求帧 ---- */
	req[0] = APU_GAS_C_START_BYTE;      /* 0xFF */
	req[1] = APU_GAS_C_SENSOR_ID;       /* 0x01 */
	req[2] = APU_GAS_C_CMD_READ;        /* 0x86 */
	req[3] = 0U;
	req[4] = 0U;
	req[5] = 0U;
	req[6] = 0U;
	req[7] = 0U;
	req[8] = compat_checksum(req);      /* 应为 0x79 */

	t0 = apu_time_ms_live();
	apu_pluart_rx_flush(g_base);
	sent = apu_pluart_write(g_base, req, APU_GAS_C_REQ_LEN, &stuck);
	if (stuck != 0 || sent != APU_GAS_C_REQ_LEN) {
		out->res = APU_GAS_E_TX_STUCK;
		return;
	}

	/* ---- 当次调用内连续抽干 FIFO ---- */
	while (out->raw_len < APU_GAS_C_RESP_LEN) {
		if ((uint32_t)(apu_time_ms_live() - t0) >=
		    APU_GAS_RECV_BUDGET_MS) {
			out->elapsed_ms = (uint32_t)(apu_time_ms_live() - t0);
			out->res        = APU_GAS_E_TIMEOUT;
			return;
		}
		if (!apu_pluart_getc(g_base, &b))
			continue;
		out->raw[out->raw_len++] = b;
	}

	out->elapsed_ms = (uint32_t)(apu_time_ms_live() - t0);

	/* ---- 帧头检查 ---- */
	if (out->raw[0] != APU_GAS_C_START_BYTE ||
	    out->raw[1] != APU_GAS_C_CMD_READ) {
		out->res = APU_GAS_E_BAD_ADDR;      /* 帧头/命令不符 */
		return;
	}

	/* ---- 解析四路浓度（大端） ---- */
	out->co  = (uint16_t)(((uint16_t)out->raw[2] << 8) | out->raw[3]);
	out->h2s = (uint16_t)(((uint16_t)out->raw[4] << 8) | out->raw[5]);
	out->o2  = (uint16_t)(((uint16_t)out->raw[6] << 8) | out->raw[7]);
	out->ch4 = (uint16_t)(((uint16_t)out->raw[8] << 8) | out->raw[9]);

	/* ---- 校验和（宽容模式：只记录，不据此丢弃数据） ---- */
	out->sum_got = out->raw[10];
	out->sum_ok  = compat_sum_ok(out->raw, out->sum_got, &out->sum_calc);

#if APU_GAS_C_STRICT_SUM
	if (!out->sum_ok) {
		out->res = APU_GAS_E_BAD_CRC;
		return;
	}
#endif

	out->res = APU_GAS_E_OK;
}

/* 兼容协议的错误串（复用 Modbus 那套即可，这里只包一层便于阅读） */
const char *apu_gas_all_err_str(int res)
{
	return apu_gas_err_str(res);
}

int apu_gas_read_all(apu_gas_all_t *out)
{
	if (out == AL_NULL)
		return -1;
	compat_read_now(out);
	return (out->res == APU_GAS_E_NO_DEVICE) ? -1 : 0;
}

/* 打印四路。固定以 [GAS4] 开头，方便串口助手过滤。
 * ★ 2026-10-15：默认不编译（APU_GAS_DEBUG_PRINT=0）—— 四路数据已走 rpmsg。 */
void apu_gas_print_all(const apu_gas_all_t *r)
{
#if APU_GAS_DEBUG_PRINT
	if (r == AL_NULL)
		return;

	if (r->res == APU_GAS_E_OK) {
		/* ⚠️ 单位按该系列惯例标注；倍率待实测标定（见 apu_gas.h 说明）。
		 *    原始值一律原样打印 —— 这是唯一可信的事实。 */
		apu_console_print("[GAS4] CO=%u ppm  H2S=%u ppm  O2=%u  CH4=%u  %ums%s",
				  (unsigned int)r->co,
				  (unsigned int)r->h2s,
				  (unsigned int)r->o2,
				  (unsigned int)r->ch4,
				  (unsigned int)r->elapsed_ms,
				  r->sum_ok ? "" : "  (校验和不符,数据仍按结构解析)");
		apu_console_print("[GAS4]   raw=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X"
				  "   sum: got=%02X calc=%02X",
				  r->raw[0], r->raw[1], r->raw[2], r->raw[3], r->raw[4],
				  r->raw[5], r->raw[6], r->raw[7], r->raw[8], r->raw[9],
				  r->raw[10],
				  r->sum_got, r->sum_calc);
	} else {
		uint32_t show = (r->raw_len < APU_GAS_C_RESP_LEN)
				? r->raw_len : APU_GAS_C_RESP_LEN;
		char hex[APU_GAS_C_RESP_LEN * 3U + 1U];
		uint32_t i, k = 0U;
		static const char kHex[] = "0123456789ABCDEF";

		for (i = 0U; i < show && (k + 3U) < sizeof(hex); i++) {
			hex[k++] = kHex[(r->raw[i] >> 4) & 0x0FU];
			hex[k++] = kHex[r->raw[i] & 0x0FU];
			hex[k++] = ' ';
		}
		hex[k] = '\0';

		apu_console_print("[GAS4] FAIL %s  got=%u [%s] %ums",
				  apu_gas_all_err_str(r->res),
				  (unsigned int)r->raw_len, hex,
				  (unsigned int)r->elapsed_ms);
	}
#else
	/* ★ 2026-10-15：四路数据改走 rpmsg 的 "ENV:..." 上报，串口1 不再刷。
	 *   把 apu_gas.h 里的 APU_GAS_DEBUG_PRINT 改成 1 重编即可恢复。 */
	(void)r;
#endif
}

/* ---------------------------------------------------------------------------
 *  ★★ 周期任务（**独立调试专用**）★★
 * ---------------------------------------------------------------------------
 *  ⚠️ 2026-10-15 架构调整后，**生产路径不再调用本函数**：
 *      三路的采集调度 + rpmsg 上报统一由 apu_env.c 负责
 *      （它调 apu_gas_read_all() 拿四路值，再发 "ENV:..."）。
 *      本函数保留仅用于单独排查气体这一路（配合 APU_GAS_DEBUG_PRINT=1）。
 *      若误在生产路径上调用它，会与 apu_env 重复收发同一串口。
 *
 *  它同时把两条协议都读一遍做对照：
 *      [GAS4] 兼容协议 0x86 → 四路（CO/H2S/O2/CH4）  ← 实际使用的
 *      [GAS]  Modbus  0x03 → 单路 + 单位/状态/温湿度（仅参考）
 * --------------------------------------------------------------------------- */
#if APU_GAS_DEBUG_PRINT
void apu_gas_poll(void)
{
	static uint32_t last_ms;
	static int      first = 1;
	static apu_gas_result_t r;
	static apu_gas_all_t    r4;

	uint32_t now = apu_time_ms_live();

	/* 没到周期：立刻返回，开销只有一次读计数器 */
	if (!first && (uint32_t)(now - last_ms) < APU_GAS_PERIOD_MS)
		return;
	first   = 0;
	last_ms = now;

	/* ---- ① 兼容协议：四路 ---- */
	compat_read_now(&r4);
	if (r4.res != APU_GAS_E_OK)
		compat_read_now(&r4);       /* 丢帧重试一次 */
	apu_gas_print_all(&r4);

	/* ---- ② Modbus：单路（含单位/状态/温湿度）---- */
	gas_read_now(&r);
	if (r.res != APU_GAS_E_OK)
		gas_read_now(&r);           /* 丢帧重试一次 */
	apu_gas_print(&r);
}
#endif /* APU_GAS_DEBUG_PRINT */
