/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_pca9685.c —— APU1 裸机侧推进器 PWM 实现
 * ===========================================================================
 *  对照传承项目 pca9685.c（158 行）写成，逻辑照抄，I2C 调用换成 apu_i2c_*。
 *
 *  【PCA9685 初始化时序】（原项目 :72-90，标准做法）
 *      ① MODE1 = 0x00                     复位
 *      ② prescale = round(25MHz / 4096 / freq) - 1
 *      ③ 读回 MODE1，置 SLEEP 位（0x10）
 *      ④ 写 prescale（只在 SLEEP 状态下才能写）
 *      ⑤ 恢复 MODE1
 *      ⑥ 延时 5ms
 *      ⑦ MODE1 |= 0xA0（RESTART + AI 自动递增）
 * ===========================================================================
 */

#include "apu_pca9685.h"

#include "al_core.h"

#include "apu_i2c.h"
#include "apu_wdt.h"      /* apu_wdt_delay —— ESC 3s 解锁延时必须分片喂狗 */

#define APU_PCA9685_BUS         APU_I2C_BUS0
#define APU_PCA9685_ADDR        APU_I2C_ADDR_PCA9685   /* 0x40 */

/* PCA9685 寄存器 */
#define PCA9685_REG_MODE1       0x00U
#define PCA9685_REG_PRESCALE    0xFEU
#define PCA9685_REG_LED0_ON_L   0x06U

#define PCA9685_MODE1_SLEEP     0x10U
#define PCA9685_MODE1_AI        0x20U   /* auto-increment */
#define PCA9685_MODE1_RESTART   0x80U

/* 内部振荡器 25MHz（原项目 pca9685.c:74 就是这么算的） */
#define PCA9685_OSC_HZ          25000000.0f
#define PCA9685_STEPS           4096.0f

/* 电调解锁延时（原项目 ESC_Init 里 fsleep_millisec(3000)） */
#define ESC_ARM_DELAY_MS        3000U

/* ---- 标定表（原项目 pca9685.c:147-159，逐通道微调过，别统一） ----
 *  ⚠️ 下标 = `channel - APU_ESC_CH_FIRST`，**不是** channel 本身。 */
typedef struct {
	uint16_t reverse_max;
	uint16_t neutral;
	uint16_t forward_max;
} esc_config_t;

/* 编译期自检：受控的 6 个通道号必须连续（4..9）。
 * 下标换算依赖"连续"这个前提；一旦有人改成不连续，这里立刻编不过，
 * 而不是等到运行期写错通道/数组越界。 */
typedef char apu_esc_channel_map_must_be_contiguous[
	(APU_ESC_CH_FIRST == 4 && APU_ESC_CH_LAST == 9 &&
	 APU_ESC_LEFT_REAR == 4 && APU_ESC_RIGHT_REAR == 5 &&
	 APU_ESC_LEFT_FRONT == 6 && APU_ESC_RIGHT_FRONT == 7 &&
	 APU_ESC_DIVE_A == 8 && APU_ESC_DIVE_B == 9) ? 1 : -1];

static const esc_config_t g_esc_cfg[APU_ESC_CH_COUNT] = {
	{ 228, 330, 433 },   /* ch4 = 左后 */
	{ 226, 328, 431 },   /* ch5 = 右后 */
	{ 228, 330, 433 },   /* ch6 = 左前 */
	{ 228, 330, 433 },   /* ch7 = 右前 */
	/* ★ 2026-10-05 新增两路下潜：同型号电调，首版沿用三路通用的 228/330/433。
	 *  ⚠️ 上面的注释说标定值"逐通道微调过"（不同实体能差 ±2 tick），
	 *     这两只新电调上台架后**必须单独复标**（用 @esc:regs 读实际脉宽核对）。 */
	{ 228, 330, 433 },   /* ch8 = 下潜-A */
	{ 228, 330, 433 },   /* ch9 = 下潜-B */
};

/* ★★ 2026-10-23：逻辑通道 -> PCA9685 物理通道 ★★
 * ---------------------------------------------------------------------------
 *  背景：板上 PCA9685 的**物理 ch4 输出异常**（PWM 乱来）。为避免改上层任何
 *        代码，把**逻辑 ch4（左后）改接到物理 ch10**。
 *
 *  下标 = `channel - APU_ESC_CH_FIRST`，与 g_esc_cfg[] / g_esc_invert[] /
 *         g_off_tick[] **严格同序**（0=左后 1=右后 2=左前 3=右前 4=下潜A 5=下潜B）。
 *
 *  🔴 这张表是**唯一**允许出现"物理通道号"的地方：
 *       · 写硬件走 pca9685_set_pwm(g_esc_phys[idx], ...)
 *       · 外部要算寄存器地址请用 apu_esc_get_phys_channel()，别自己 4*ch
 *
 *  ⚠️ 左边（逻辑号 4..9）是和上层、标定表、取反表、自检命令绑定的契约，
 *     **不要**因为硬件改线去动它；要换脚只改右边这一列。
 *  ⚠️ 物理 ch4 现在没人用，apu_pca9685_init() 末尾会把它显式写成"无脉冲"，
 *     免得它保留上电残留占空比继续乱输出。 */
/* 物理通道号（PCA9685 只有 0..15）。
 * ⚠️ 写成宏而不是直接填进数组：C 语言里 const 数组元素**不是常量表达式**，
 *    用它做数组长度会退化成变长数组（file scope 直接编不过）。 */
#define APU_ESC_PHYS_LEFT_REAR    10U   /* ★ 原 4；该路 PWM 输出异常，2026-10-23 改接 */
#define APU_ESC_PHYS_RIGHT_REAR    5U
#define APU_ESC_PHYS_LEFT_FRONT    6U
#define APU_ESC_PHYS_RIGHT_FRONT   7U
#define APU_ESC_PHYS_DIVE_A        8U
#define APU_ESC_PHYS_DIVE_B        9U

/* 编译期自检①：物理通道号必须都落在 PCA9685 的 0..15 内。 */
typedef char apu_esc_phys_channel_out_of_range[
	(APU_ESC_PHYS_LEFT_REAR  <= 15U && APU_ESC_PHYS_RIGHT_REAR  <= 15U &&
	 APU_ESC_PHYS_LEFT_FRONT <= 15U && APU_ESC_PHYS_RIGHT_FRONT <= 15U &&
	 APU_ESC_PHYS_DIVE_A     <= 15U && APU_ESC_PHYS_DIVE_B     <= 15U) ? 1 : -1];

/* 编译期自检②：不能有两个逻辑通道抢同一个物理通道（否则两路电机会同步动作）。 */
typedef char apu_esc_phys_channel_must_be_unique[
	(APU_ESC_PHYS_LEFT_REAR   != APU_ESC_PHYS_RIGHT_REAR  &&
	 APU_ESC_PHYS_LEFT_REAR   != APU_ESC_PHYS_LEFT_FRONT  &&
	 APU_ESC_PHYS_LEFT_REAR   != APU_ESC_PHYS_RIGHT_FRONT &&
	 APU_ESC_PHYS_LEFT_REAR   != APU_ESC_PHYS_DIVE_A      &&
	 APU_ESC_PHYS_LEFT_REAR   != APU_ESC_PHYS_DIVE_B      &&
	 APU_ESC_PHYS_RIGHT_REAR  != APU_ESC_PHYS_LEFT_FRONT  &&
	 APU_ESC_PHYS_RIGHT_REAR  != APU_ESC_PHYS_RIGHT_FRONT &&
	 APU_ESC_PHYS_RIGHT_REAR  != APU_ESC_PHYS_DIVE_A      &&
	 APU_ESC_PHYS_RIGHT_REAR  != APU_ESC_PHYS_DIVE_B      &&
	 APU_ESC_PHYS_LEFT_FRONT  != APU_ESC_PHYS_RIGHT_FRONT &&
	 APU_ESC_PHYS_LEFT_FRONT  != APU_ESC_PHYS_DIVE_A      &&
	 APU_ESC_PHYS_LEFT_FRONT  != APU_ESC_PHYS_DIVE_B      &&
	 APU_ESC_PHYS_RIGHT_FRONT != APU_ESC_PHYS_DIVE_A      &&
	 APU_ESC_PHYS_RIGHT_FRONT != APU_ESC_PHYS_DIVE_B      &&
	 APU_ESC_PHYS_DIVE_A      != APU_ESC_PHYS_DIVE_B) ? 1 : -1];

static const uint8_t g_esc_phys[APU_ESC_CH_COUNT] = {
	APU_ESC_PHYS_LEFT_REAR,   /* idx0 逻辑 ch4 左后   -> 物理 ch10 */
	APU_ESC_PHYS_RIGHT_REAR,  /* idx1 逻辑 ch5 右后   -> 物理 ch5  */
	APU_ESC_PHYS_LEFT_FRONT,  /* idx2 逻辑 ch6 左前   -> 物理 ch6  */
	APU_ESC_PHYS_RIGHT_FRONT, /* idx3 逻辑 ch7 右前   -> 物理 ch7  */
	APU_ESC_PHYS_DIVE_A,      /* idx4 逻辑 ch8 下潜-A -> 物理 ch8  */
	APU_ESC_PHYS_DIVE_B,      /* idx5 逻辑 ch9 下潜-B -> 物理 ch9  */
};

/* ★ 2026-10-12：逐通道输出取反表。
 * ---------------------------------------------------------------------------
 *  用途：某个电机**三相线接反了**（正转指令变成反转），在固件里把这一路
 *        的输出取反即可，**不用拆线**。
 *
 *  1 = 该通道取反（下发前 speed_percent 取负）
 *  0 = 正常（默认，六个通道全 0）
 *
 *  ⚠️ 为什么放在 apu_esc_set_speed() 这一个点：
 *     它是**唯一**真正换算 tick 并写 PCA9685 的地方 —— 自动居中、手动混控、
 *     下潜、所有"停"都从它走。在这里取反，等价于"把这路电机的正负定义翻过来"，
 *     上层所有混控/限幅/PID 的语义完全不用动，也不会有"某个调用点忘了反"的漏洞。
 *
 *  ⚠️ 取反放在**限幅之前**：这样 ±100 的对称限幅仍然成立，
 *     不会出现"-(-100) = 100 越界"这类问题。
 *
 *  ⚠️ 中立点(0%)不受影响：0 的相反数还是 0，停车依旧是停车。
 *
 *  下标顺序与 g_esc_cfg[] 一致：0=左后 1=右后 2=左前 3=右前 4=下潜A 5=下潜B */
static const uint8_t g_esc_invert[APU_ESC_CH_COUNT] = {
	1,   /* ch4 = 左后 —— 2026-10-24 实测反转，本路取反(物理 ch10) */
	1,   /* ch5 = 右后 —— ★ 三相线装反，本路取反 */
	0,   /* ch6 = 左前 */
	0,   /* ch7 = 右前 */
	0,   /* ch8 = 下潜-A */
	0,   /* ch9 = 下潜-B */
};

static uint16_t g_off_tick[APU_ESC_CH_COUNT];  /* 各通道当前 off_tick（读回/验证用） */

/* ★ 2026-10-05：下潜当前状态（**唯一真值**）。
 *  ⚠️ 这三个是"已经写到芯片上的值"，不是"参数里写的值"（参数可能刚被改过、
 *     而这里还没重下发）。所有"停"都会经 apu_esc_stop_all() 把这三个一起清零，
 *     所以 @ctrl 显示的状态永远不会和实际输出不一致。 */
static int      g_dive_on;       /* 1 = 下潜在开（CMD:DIVE 之后），0 = 停 */
static float    g_dive_left;     /* 当前真正写到 ch8 的百分比 */
static float    g_dive_right;    /* 当前真正写到 ch9 的百分比 */
static int      g_ready;
static uint32_t g_wr_count;
static uint32_t g_err_count;

/* ---------------------------------------------------------------------------
 * 底层寄存器访问
 * --------------------------------------------------------------------------- */
int apu_pca9685_write_reg(uint8_t reg, uint8_t val)
{
	int ret = apu_i2c_write_reg(APU_PCA9685_BUS, (uint8_t)APU_PCA9685_ADDR,
				    reg, val);

	if (ret == 0)
		g_wr_count++;
	else
		g_err_count++;
	return ret;
}

int apu_pca9685_read_reg(uint8_t reg, uint8_t *val)
{
	return apu_i2c_read_reg(APU_PCA9685_BUS, (uint8_t)APU_PCA9685_ADDR,
				reg, val);
}

/* 4 字节 PWM 寄存器（原项目 PCA9685_SetPWM） */
static void pca9685_set_pwm(uint8_t channel, uint16_t on_tick, uint16_t off_tick)
{
	uint8_t data[4];
	uint8_t reg_start;

	if (channel > 15U)
		return;

	reg_start = (uint8_t)(PCA9685_REG_LED0_ON_L + 4U * channel);

	/* 小端：ON_L, ON_H, OFF_L, OFF_H */
	data[0] = (uint8_t)(on_tick & 0xFFU);
	data[1] = (uint8_t)((on_tick >> 8) & 0xFFU);
	data[2] = (uint8_t)(off_tick & 0xFFU);
	data[3] = (uint8_t)((off_tick >> 8) & 0xFFU);

	if (apu_i2c_write_regs(APU_PCA9685_BUS, (uint8_t)APU_PCA9685_ADDR,
			       reg_start, data, 4U) == 0)
		g_wr_count++;
	else
		g_err_count++;
}

/* ---------------------------------------------------------------------------
 * 初始化
 * --------------------------------------------------------------------------- */
int apu_pca9685_init(void)
{
	uint8_t oldmode, newmode, prescale;
	float prescaleval;
	int ret;
	uint32_t i;

	g_wr_count = 0U;
	g_err_count = 0U;
	g_ready = 0;

	for (i = 0U; i < (uint32_t)APU_ESC_CH_COUNT; i++)
		g_off_tick[i] = 0U;
	g_dive_on = 0;
	g_dive_left = 0.0f;
	g_dive_right = 0.0f;

	/* ---- 先确认 PCA9685 应答（读 MODE1）---- */
	{
		uint8_t probe = 0U;

		ret = apu_pca9685_read_reg(PCA9685_REG_MODE1, &probe);
		if (ret != 0) {
			AL_LOG(AL_LOG_LEVEL_ERROR,
			       "[APU1][esc] PCA9685 NOT responding on bus%d addr=0x%02X "
			       "(ret=%d). Check CEP 3/4 (Y12/Y13) wiring, 3.3V, GND, pull-ups.",
			       (int)APU_PCA9685_BUS, (unsigned int)APU_PCA9685_ADDR, ret);
			apu_i2c_dump_last_abort(APU_PCA9685_BUS);
			return -1;
		}
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][esc] PCA9685 found, MODE1=0x%02X", (unsigned int)probe);
	}

	/* ---- ① 复位 ---- */
	(void)apu_pca9685_write_reg(PCA9685_REG_MODE1, 0x00U);

	/* ---- ② 算 prescale（50Hz）---- */
	prescaleval = PCA9685_OSC_HZ / PCA9685_STEPS;    /* 6103.5 */
	prescaleval /= 50.0f;                            /* / freq_hz */
	prescaleval -= 1.0f;
	prescale = (uint8_t)(prescaleval + 0.5f);

	/* ---- ③ 进 SLEEP 才能写 prescale ---- */
	if (apu_pca9685_read_reg(PCA9685_REG_MODE1, &oldmode) != 0)
		oldmode = 0x00U;
	newmode = (uint8_t)((oldmode & 0x7FU) | PCA9685_MODE1_SLEEP);

	(void)apu_pca9685_write_reg(PCA9685_REG_MODE1, newmode);
	(void)apu_pca9685_write_reg(PCA9685_REG_PRESCALE, prescale);
	(void)apu_pca9685_write_reg(PCA9685_REG_MODE1, oldmode);

	AlSys_MDelay(5U);

	/* ---- ④ RESTART + 自动递增 ---- */
	(void)apu_pca9685_write_reg(PCA9685_REG_MODE1,
				    (uint8_t)(oldmode | PCA9685_MODE1_RESTART |
					      PCA9685_MODE1_AI));

	g_ready = 1;

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][esc] PCA9685 init done: 50Hz, prescale=0x%02X (%.1f -> %u)",
	       (unsigned int)prescale, prescaleval, (unsigned int)prescale);

	/* ---- ⑤ 上电安全：**6 路**先给 neutral，再等电调解锁（ch8/ch9 下潜两路也在内）---- */
	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][esc] arming sequence: all channels -> neutral, wait %u ms "
	       "(ESC unlock window). MOTORS MUST NOT MOVE.",
	       (unsigned int)ESC_ARM_DELAY_MS);

	apu_esc_stop_all();

	/* ★ 2026-10-23：把"逻辑区间内、但已不再被 g_esc_phys 映射"的物理通道
	 *  显式写成无脉冲（ON=0/OFF=0 对该路等价于信号丢失）。
	 *  当前只会命中**物理 ch4**（左后已改接到物理 ch10）——
	 *  这样即使芯片里残留了上电默认之外的占空比，那路也不会继续乱输出。
	 *  ⚠️ 写成泛化循环：以后再有通道改接，不用回来手工补通道号。 */
	{
		uint8_t phys, j;

		for (phys = (uint8_t)APU_ESC_CH_FIRST;
		     phys <= (uint8_t)APU_ESC_CH_LAST; phys++) {
			int used = 0;

			for (j = 0U; j < (uint8_t)APU_ESC_CH_COUNT; j++) {
				if (g_esc_phys[j] == phys) {
					used = 1;
					break;
				}
			}
			if (!used) {
				pca9685_set_pwm(phys, 0U, 0U);
				AL_LOG(AL_LOG_LEVEL_INFO,
				       "[APU1][esc] unused phys ch%u -> no pulse (abandoned)",
				       (unsigned int)phys);
			}
		}
	}

	/* ★★ 2026-09-25：原为 AlSys_MDelay(ESC_ARM_DELAY_MS) —— 3 秒裸延时 ★★
	 *
	 *  这是全工程余量最小的一步：
	 *      硬件狗超时 4030ms，这里一口气阻塞 3000ms，余量只有约 1 秒。
	 *      而这一步前面紧挨着好几笔 I2C 通信（现场首次通信会偏慢），
	 *      一旦累计超过 1 秒，狗就咬 → 整板复位。
	 *      上板日志里的 `history reset reason: 00000060`(SWDT0) 已经证明咬过。
	 *
	 *  改用 apu_wdt_delay()：切成 1 秒一片，每片之前喂狗。
	 *  行为完全相同（仍是阻塞等待 3 秒，电机仍然静默），
	 *  只是喂狗间隔不再超过 1 秒。
	 *
	 *  ⚠️ 这不是"缩短解锁窗口"——ESC 的 3 秒解锁窗口必须完整保留，
	 *     否则电调不会解锁。只是把"这一整段"变成"可被狗观察的分片"。 */
	apu_wdt_delay(ESC_ARM_DELAY_MS);

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][esc] armed (6ch). neutral tick  L-R/R-R/L-F/R-F/dive-A/dive-B = "
	       "%u/%u/%u/%u/%u/%u",
	       (unsigned int)g_esc_cfg[0].neutral, (unsigned int)g_esc_cfg[1].neutral,
	       (unsigned int)g_esc_cfg[2].neutral, (unsigned int)g_esc_cfg[3].neutral,
	       (unsigned int)g_esc_cfg[4].neutral, (unsigned int)g_esc_cfg[5].neutral);

	return 0;
}

void apu_pca9685_deinit(void)
{
	uint8_t i;

	if (!g_ready)
		return;

	/* ON=0 / OFF=0 = 该路恒低（完全没有脉冲）—— 对电调等价于"信号丢失"，
	 * 比给 neutral 更彻底，用于关核收尾。★ 2026-10-05：扩到 6 路。
	 * ★ 2026-10-23：按**物理**通道号关（g_esc_phys[i]），与写入端保持同一张表。 */
	for (i = 0U; i < (uint32_t)APU_ESC_CH_COUNT; i++)
		pca9685_set_pwm(g_esc_phys[i], 0U, 0U);
	g_dive_on = 0;
	g_dive_left = 0.0f;
	g_dive_right = 0.0f;

	/* SLEEP 位 -> 关闭输出 */
	(void)apu_pca9685_write_reg(PCA9685_REG_MODE1, PCA9685_MODE1_SLEEP);
	AlSys_MDelay(2U);

	g_ready = 0;
	AL_LOG(AL_LOG_LEVEL_INFO, "[APU1][esc] PCA9685 stopped, outputs off");
}

/* ---------------------------------------------------------------------------
 * 调速
 * --------------------------------------------------------------------------- */
void apu_esc_set_speed(uint8_t channel, float speed_percent)
{
	uint8_t idx;
	uint16_t n_tick, f_tick, r_tick, off_tick;

	/* ★ 2026-10-05：通道范围由 4..7 放宽到 **4..9**（新增 ch8/ch9 下潜）。
	 *  🔴 这是"新通道写了没反应"的头号坑：不改这一行，对 ch8/ch9 的调用会被
	 *     静默丢弃（不报错、不打印，看起来像 I2C 没写进去）。 */
	if (channel < (uint8_t)APU_ESC_CH_FIRST || channel > (uint8_t)APU_ESC_CH_LAST)
		return;

	idx = (uint8_t)(channel - (uint8_t)APU_ESC_CH_FIRST);

	/* ★ 2026-10-12：该通道若"三相线接反"，在这里把指令取反。
	 *  ⚠️ 必须在限幅**之前**做：否则 -100 取反成 +100 时可能越界。 */
	if (g_esc_invert[idx] != 0U)
		speed_percent = -speed_percent;

	if (speed_percent < -100.0f)
		speed_percent = -100.0f;
	if (speed_percent > 100.0f)
		speed_percent = 100.0f;

	n_tick = g_esc_cfg[idx].neutral;
	f_tick = g_esc_cfg[idx].forward_max;
	r_tick = g_esc_cfg[idx].reverse_max;

	if (speed_percent >= 0.0f)
		off_tick = (uint16_t)(n_tick +
				      (uint16_t)(speed_percent * (float)(f_tick - n_tick) / 100.0f));
	else
		off_tick = (uint16_t)(n_tick -
				      (uint16_t)((-speed_percent) * (float)(n_tick - r_tick) / 100.0f));

	g_off_tick[idx] = off_tick;

	/* ★ 2026-10-23：这里必须走**物理通道**映射表。
	 *  逻辑号（channel，4..9）是给上层/标定/取反/自检用的契约；
	 *  真正落到 PCA9685 寄存器的是 g_esc_phys[idx]（左后已改到物理 ch10）。
	 *  ⚠️ 别写回 channel —— 那会又去踩物理 ch4（该路 PWM 输出异常）。 */
	pca9685_set_pwm(g_esc_phys[idx], 0U, off_tick);
}

void apu_esc_set_all(float left_front, float right_front,
		     float left_rear, float right_rear)
{
	apu_esc_set_speed(APU_ESC_LEFT_FRONT, left_front);
	apu_esc_set_speed(APU_ESC_RIGHT_FRONT, right_front);
	apu_esc_set_speed(APU_ESC_LEFT_REAR, left_rear);
	apu_esc_set_speed(APU_ESC_RIGHT_REAR, right_rear);
}

/* ★ 2026-10-05 新增：只停**四路水平推进器**（ch4~ch7），**不动**下潜两路（ch8/ch9）。
 *
 *  ⚠️ 为什么要单独有这么一个函数：
 *     未点「向前」时（!motor_enabled），apu_esc_proc_auto_nav() **每轮主循环**都要
 *     把水平推进器压回中位（防"停不住"）。原来它调的是 6 路的 apu_esc_stop_all()，
 *     结果把下潜那两路也一起抹掉了 —— 表现就是"必须先点『向前』才能下潜"。
 *     现在这里只停四路水平，下潜作为独立通道可以在"待命"状态下单独工作。
 *
 *  🔴 真正的全停 / 急停（CMD:STOP / 心跳超时 / 导航故障 / 超声损坏 / 关核 / @esc:stop）
 *     走的仍然是 6 路的 apu_esc_stop_all() —— "停命令对六个电机都适用"没有变。 */
void apu_esc_stop_drive(void)
{
	uint8_t i;

	/* ch4~ch7 编号连续，有编译期断言保证（apu_esc_channel_map_must_be_contiguous） */
	for (i = 0U; i < 4U; i++)
		apu_esc_set_speed((uint8_t)(APU_ESC_LEFT_REAR + i), 0.0f);
}

/* ★ 统一停车路径 —— 所有"停"的场景都走它
 *
 * ★ 2026-10-05：由 4 路扩到 **6 路**（含 ch8/ch9 下潜）。
 *   需求明确："使这些电机停止的命令六个电机都适用" —— 所以这里必须写全。
 *   调用者（全都要覆盖，别漏）：
 *     apu_app_safe_stop()       CMD:STOP / CMD:WDT_TEST / 心跳超时 / 导航故障 / 关核
 *     apu_remote_mode_start()   CMD:MANUAL_START（进手动前先全停）
 *     apu_remote_mode_stop()    CMD:MOTOR_STOP
 *     apu_control_pipeline()    超声损坏时停
 *     （⚪ 原 apu_esc_proc_auto_nav() 的"未启动就停"已改调 apu_esc_stop_drive()，
 *        只停四路水平 —— 因为下潜要能在"待命"状态下独立工作）
 *     apu_pca9685_init()        上电 arm 时序（先给 neutral 再等 3s 解锁）
 *   ⚠️ 顺带的好处：把下潜两路加进来，这两只新电调**自动进入了上电 arm 窗口** ——
 *     同型号电调必须走这一步，否则上电不解锁。 */
void apu_esc_stop_all(void)
{
	uint8_t i;

	/* 用循环而不是逐个列举：以后再加通道时不可能漏掉一路。
	 * 顺序 = ch4 左后 -> ch5 右后 -> ch6 左前 -> ch7 右前 -> ch8 -> ch9 */
	for (i = 0U; i < (uint8_t)APU_ESC_CH_COUNT; i++)
		apu_esc_set_speed((uint8_t)(APU_ESC_CH_FIRST + i), 0.0f);

	/* 状态也清零：停完必须"固件里的状态 == 实际输出"，
	 * 否则 @ctrl / apu_esc_get_dive() 会显示一个早就没生效的旧状态。
	 * ⚠️ 上面那个循环已经把 ch8/ch9 写成 neutral 了，这里**只改状态**，
	 *    不要再调一次 apu_esc_set_dive()（那会多两笔无谓的 I2C）。 */
	g_dive_on = 0;
	g_dive_left = 0.0f;
	g_dive_right = 0.0f;
}

uint16_t apu_esc_get_off_tick(uint8_t channel)
{
	if (channel < (uint8_t)APU_ESC_CH_FIRST || channel > (uint8_t)APU_ESC_CH_LAST)
		return 0U;
	return g_off_tick[channel - (uint8_t)APU_ESC_CH_FIRST];
}

/* ★ 2026-10-12：查某个通道是否被"输出取反"（三相线接反的软件补偿）。
 *  返回 1 = 已取反，0 = 正常，越界也返回 0。供 @esc:regs 等自检显示用。 */
int apu_esc_get_invert(uint8_t channel)
{
	if (channel < (uint8_t)APU_ESC_CH_FIRST || channel > (uint8_t)APU_ESC_CH_LAST)
		return 0;
	return (g_esc_invert[channel - (uint8_t)APU_ESC_CH_FIRST] != 0U) ? 1 : 0;
}

/* ★ 2026-10-23：逻辑通道号 -> PCA9685 物理通道号。
 *  任何"要算寄存器地址"的外部代码（@esc:regs 自检）都必须走这里，
 *  否则会拿逻辑号直接算 0x06+4*ch，读到的是废弃的物理 ch4 而不是真正的 ch10。
 *  入参越界返回 0xFF。 */
uint8_t apu_esc_get_phys_channel(uint8_t channel)
{
	if (channel < (uint8_t)APU_ESC_CH_FIRST || channel > (uint8_t)APU_ESC_CH_LAST)
		return 0xFFU;
	return g_esc_phys[channel - (uint8_t)APU_ESC_CH_FIRST];
}

/* ---------------------------------------------------------------------------
 * ★ 2026-10-05 新增：下潜两路（ch8 / ch9）
 * ---------------------------------------------------------------------------
 *  开关来自命令（CMD:DIVE / CMD:DIVE:STOP），速度来自参数（CTRL: 末两位）。
 *
 *  ⚠️ **不做任何取反**：本艇正浮力，"上浮"是靠把速度减小/停掉自然发生的，
 *     不是靠反向推力。哪一路实物装反就把它的参数写成负值。
 *
 *  ⚠️ 这里自己先限幅，再存 g_dive_* —— apu_esc_set_speed() 内部也会限幅，
 *     若不在这里先夹一次，存下来的就是**没夹过的原始值**，
 *     和真正写到芯片上的 tick 对不上（@ctrl 会显示一个假的百分比）。
 *  ⚠️ 写一次即可：PCA9685 会持续输出该占空比，不需要周期刷新。 */
void apu_esc_set_dive(int on, float left_pct, float right_pct)
{
	float l = (on != 0) ? left_pct  : 0.0f;
	float r = (on != 0) ? right_pct : 0.0f;

	if (l < -100.0f) l = -100.0f;
	if (l >  100.0f) l =  100.0f;
	if (r < -100.0f) r = -100.0f;
	if (r >  100.0f) r =  100.0f;

	g_dive_on    = (on != 0) ? 1 : 0;
	g_dive_left  = l;
	g_dive_right = r;

	apu_esc_set_speed(APU_ESC_DIVE_A, l);
	apu_esc_set_speed(APU_ESC_DIVE_B, r);
}

void apu_esc_get_dive(int *on, float *left_pct, float *right_pct)
{
	if (on)        *on        = g_dive_on;
	if (left_pct)  *left_pct  = g_dive_left;
	if (right_pct) *right_pct = g_dive_right;
}

/* ⚪ 2026-09-25：原 `apu_esc_set_raw_tick()`（绕过标定表直接写 off_tick，
 *  用于脉宽扫描找电调解锁点）已删除。它是 P7 台架期工具，与传承工程无关；
 *  脉宽标定请走 PCA9685_SetPWM 之外的正常路径（ESC_SetSpeed 的标定表），
 *  要看芯片里真实寄存器内容用控制台 @esc:regs。 */

uint32_t apu_pca9685_write_count(void)
{
	return g_wr_count;
}

uint32_t apu_pca9685_err_count(void)
{
	return g_err_count;
}
