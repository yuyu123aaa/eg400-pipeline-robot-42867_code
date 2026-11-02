/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_gasflow.c —— 气路控制（电磁阀 + 吸气泵）实现
 * ===========================================================================
 *  时序铁律、引脚分配、安全超时的设计说明全部写在 apu_gasflow.h 顶部。
 *  这里只记实现上的三个关键决定：
 *
 *  【1】为什么做成**非阻塞状态机**，而不是在 start() 里直接 sleep 8.5 秒
 *
 *      主循环里 platform_poll()（rpmsg）必须及时跑 —— 2026-10-14 那次
 *      Linux 侧 `Connection reset` 就是主循环被长时间占住（960ms/圈）
 *      把 rpmsg 饿死造成的。
 *      如果在 start() 里直接延时 8.5 秒，等于把整个核冻住 8.5 秒：
 *        · rpmsg 彻底停摆 → Linux 侧必然报错/判死
 *        · 硬件看门狗 4.03 秒就咬（虽然可以在延时循环里喂狗，但那更丑）
 *      ⇒ 所以拆成状态机，每圈只做"看时间 + 该动就动一下"，**零阻塞**。
 *
 *  【2】时序精度说明（要如实知道）
 *
 *      poll() 由主循环驱动，而主循环被 APU0 约 1 秒一次的 SGI 踢着走，
 *      所以状态推进的粒度是**约 1 秒**。
 *      ⇒ 实际时序可能是：
 *            开阀 → (2.0~3.0s) → 开泵 → (6.0~7.0s) → 关泵 → (0.5~1.5s) → 关阀
 *      ⇒ **只会比计划更晚，绝不会更早**（每个阶段都是"到了时间才允许进入
 *        下一阶段"），所以"阀先泵后"的顺序永远成立 —— 这是安全侧。
 *      ⇒ 如果以后要求毫秒级精确时序，需要把 poll() 挂到定时中断上，
 *        而不是主循环。现阶段（安全优先）非阻塞状态机是正确取舍。
 *
 *  【3】每个阶段都有**独立安全超时**
 *
 *      万一状态机因为任何原因卡住（比如时间基异常），超时到点会
 *      强制走"关泵 → 关阀"路径，**绝不让泵无限转下去**。
 *      这是对硬件最坏情况的兜底。
 * ===========================================================================
 */

#include "apu_gasflow.h"

#include <string.h>

#include "al_core.h"
#include "al_gpio_hal.h"

#include "apu_time.h"
#include "apu_console.h"
#include "apu_msg_api.h"    /* apu_send_ack —— 给 Web 按钮回执 */
#include "apu_wdt.h"        /* apu_wdt_feed —— 与 apu_led.c 同一用法 */

/* ---------------------------------------------------------------------------
 *  内部状态
 * --------------------------------------------------------------------------- */
static AL_GPIO_HalStruct *g_gpio;
static int      g_ready;            /* GPIO 控制器是否可用 */
static apu_gasflow_state_t g_state;
static uint32_t g_t0;               /* 本轮流程启动时刻 */
static uint32_t g_phase_ms;         /* 本阶段进入时刻 */

static int g_valve_on;              /* 已写出去的电平（0/1）*/
static int g_pump_on;

/* 运行期统计（给控制台看，便于判断"到底跑了几次、有没有被超时打断"）*/
static uint32_t g_run_count;        /* 成功完整跑完的次数 */
static uint32_t g_timeout_count;    /* 因安全超时被强制收尾的次数 */

/* ---------------------------------------------------------------------------
 *  底层写引脚
 *  ⚠️ AlGpio_Hal_WritePin 内部只改目标 bit（见 apu_led.c 顶部核对结论），
 *     所以写阀不会影响泵、也不会影响 led1/led2/rs485_de。
 * --------------------------------------------------------------------------- */
static void gasflow_write(uint32_t pin, int on)
{
	if (!g_ready)
		return;
	(void)AlGpio_Hal_WritePin(g_gpio, pin, on ? 1U : 0U);
}

static void valve_set(int on)
{
	gasflow_write(APU_GASFLOW_VALVE_PIN, on);
	g_valve_on = on ? 1 : 0;
}

static void pump_set(int on)
{
	gasflow_write(APU_GASFLOW_PUMP_PIN, on);
	g_pump_on = on ? 1 : 0;
}

static void enter_state(apu_gasflow_state_t st, uint32_t now)
{
	g_state    = st;
	g_phase_ms = now;
}

/* ---------------------------------------------------------------------------
 *  初始化：**先保证两路都是关的**
 * --------------------------------------------------------------------------- */
int apu_gasflow_init(void)
{
	AL_S32 ret;

	ret = AlGpio_Hal_Init(&g_gpio, 0U, AL_NULL);
	if (ret != AL_OK) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][gasflow] AlGpio_Hal_Init(DevId=0) failed, ret=0x%x",
		       (unsigned int)ret);
		g_ready = 0;
		g_state = APU_GASFLOW_IDLE;
		return -1;
	}

	g_ready  = 1;
	g_state  = APU_GASFLOW_IDLE;
	g_t0     = 0U;
	g_phase_ms = 0U;

	/* ★ 上电安全：两路都写 0（阀关、泵关）。
	 *   写 0 同时也把方向设成输出（AlGpio_Hal_WritePin 的隐式行为）。
	 *   PS 的 GPIO 输出寄存器复位值本来就是 0，这里再显式写一次是为了
	 *   防御热复位残留 —— 绝不能让上电瞬间泵误转。 */
	valve_set(APU_GASFLOW_OFF);
	pump_set(APU_GASFLOW_OFF);

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][gasflow] ready. valve=pin%u(bit3/CEP12/V7)  pump=pin%u(bit4/CEP10/W6)"
	       "  both OFF at boot",
	       (unsigned int)APU_GASFLOW_VALVE_PIN,
	       (unsigned int)APU_GASFLOW_PUMP_PIN);
	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][gasflow] sequence: valve ON -> %ums settle -> pump ON"
	       " -> %ums suck -> pump OFF -> %ums -> valve OFF  (total %ums)",
	       (unsigned int)APU_GASFLOW_VALVE_SETTLE_MS,
	       (unsigned int)APU_GASFLOW_PUMP_RUN_MS,
	       (unsigned int)APU_GASFLOW_PUMP_OFF_DELAY_MS,
	       (unsigned int)(APU_GASFLOW_VALVE_SETTLE_MS +
			      APU_GASFLOW_PUMP_RUN_MS +
			      APU_GASFLOW_PUMP_OFF_DELAY_MS));

	return 0;
}

/* ---------------------------------------------------------------------------
 *  启动一次流程
 * --------------------------------------------------------------------------- */
int apu_gasflow_start(void)
{
	uint32_t now;

	if (!g_ready) {
		apu_console_print("[GASFLOW] start REJECTED: GPIO not ready");
		return -1;
	}

	if (g_state != APU_GASFLOW_IDLE) {
		apu_console_print("[GASFLOW] busy (%s), start ignored - not re-entrant",
				  apu_gasflow_state_name(g_state));
		return 1;
	}

	now = apu_time_ms_live();
	g_t0 = now;

	/* ★ 第一步：只开阀，泵保持关。
	 *   顺序铁律 —— 绝不在这里同时开泵。 */
	valve_set(APU_GASFLOW_ON);
	pump_set(APU_GASFLOW_OFF);
	enter_state(APU_GASFLOW_VALVE_OPEN, now);

	apu_console_print("[GASFLOW] START: valve=ON (pump stays OFF), "
			  "waiting %ums for valve to settle...",
			  (unsigned int)APU_GASFLOW_VALVE_SETTLE_MS);
	return 0;
}

/* ---------------------------------------------------------------------------
 *  周期推进（非阻塞）
 * --------------------------------------------------------------------------- */
void apu_gasflow_poll(void)
{
	uint32_t now;
	uint32_t in_phase;

	if (!g_ready || g_state == APU_GASFLOW_IDLE)
		return;

	now      = apu_time_ms_live();
	in_phase = (uint32_t)(now - g_phase_ms);

	switch (g_state) {

	/* ---- 阶段①：阀已开，等稳定，到点开泵 ---- */
	case APU_GASFLOW_VALVE_OPEN:
		if (in_phase >= APU_GASFLOW_VALVE_SETTLE_MS) {
			/* ★ 第二步：这时候才允许开泵。泵开之前阀已经开了
			 *   至少 VALVE_SETTLE_MS，气路导通，不会有负压冲击。 */
			pump_set(APU_GASFLOW_ON);
			enter_state(APU_GASFLOW_PUMPING, now);
			apu_console_print("[GASFLOW] valve settled (%ums) -> pump=ON, "
					  "sucking for %ums",
					  (unsigned int)in_phase,
					  (unsigned int)APU_GASFLOW_PUMP_RUN_MS);
		} else if (in_phase >= APU_GASFLOW_VALVE_TIMEOUT_MS) {
			/* 异常：等待开泵的时间窗都没到？逻辑上不可能，
			 * 但兜底一律走安全收尾。 */
			g_timeout_count++;
			apu_console_print("[GASFLOW] !! valve-settle TIMEOUT (%ums) "
					  "- forcing safe shutdown", (unsigned int)in_phase);
			apu_gasflow_force_off("valve settle timeout");
		}
		break;

	/* ---- 阶段②：吸气中，到点关泵 ---- */
	case APU_GASFLOW_PUMPING:
		if (in_phase >= APU_GASFLOW_PUMP_RUN_MS) {
			/* ★ 第三步：先关泵（阀**保持开**，等管路余气排净） */
			pump_set(APU_GASFLOW_OFF);
			enter_state(APU_GASFLOW_CLOSING, now);
			apu_console_print("[GASFLOW] suck done (%ums) -> pump=OFF "
					  "(valve still ON), waiting %ums before closing valve",
					  (unsigned int)in_phase,
					  (unsigned int)APU_GASFLOW_PUMP_OFF_DELAY_MS);
		} else if (in_phase >= APU_GASFLOW_PUMP_TIMEOUT_MS) {
			g_timeout_count++;
			apu_console_print("[GASFLOW] !! pump-run TIMEOUT (%ums, planned %ums) "
					  "- forcing safe shutdown",
					  (unsigned int)in_phase,
					  (unsigned int)APU_GASFLOW_PUMP_RUN_MS);
			apu_gasflow_force_off("pump run timeout");
		}
		break;

	/* ---- 阶段③：泵已关，等延迟，到点关阀 ---- */
	case APU_GASFLOW_CLOSING:
		if (in_phase >= APU_GASFLOW_PUMP_OFF_DELAY_MS) {
			/* ★ 第四步：最后才关阀。此时泵已停，
			 *   不会出现"阀关了泵还在转"的憋压。 */
			valve_set(APU_GASFLOW_OFF);
			g_run_count++;
			enter_state(APU_GASFLOW_IDLE, now);
			apu_console_print("[GASFLOW] DONE: valve=OFF, pump=OFF. "
					  "total=%ums (run #%u)",
					  (unsigned int)(now - g_t0),
					  (unsigned int)g_run_count);
		} else if (in_phase >= APU_GASFLOW_CLOSE_TIMEOUT_MS) {
			g_timeout_count++;
			apu_console_print("[GASFLOW] !! close-delay TIMEOUT (%ums) "
					  "- forcing safe shutdown", (unsigned int)in_phase);
			apu_gasflow_force_off("close delay timeout");
		}
		break;

	default:
		break;
	}
}

/* ---------------------------------------------------------------------------
 *  急停：两路立即关，状态机回空闲
 * ---------------------------------------------------------------------------
 *  ⚠️ 语义说明：**这里刻意不遵守"泵先阀后"的延迟**。
 *     因为调用它的场合都是"必须立刻断气"：
 *       · 收到紧急停止命令
 *       · 状态机超时兜底
 *       · 关机
 *     两路几乎同时写 0（相差几个微秒，继电器机械动作时间 ms 级，
 *     实际可视为同时）。对"关"来说，即使同时关也不会造成损坏 ——
 *     损坏场景是"阀关了泵还在转"，而这里泵也一起关了。
 * --------------------------------------------------------------------------- */
void apu_gasflow_force_off(const char *reason)
{
	apu_gasflow_state_t was = g_state;

	pump_set(APU_GASFLOW_OFF);      /* 先泵 */
	valve_set(APU_GASFLOW_OFF);     /* 后阀（同一函数内，间隔微秒级）*/
	enter_state(APU_GASFLOW_IDLE, apu_time_ms_live());

	if (was != APU_GASFLOW_IDLE) {
		apu_console_print("[GASFLOW] FORCE OFF (was %s): %s",
				  apu_gasflow_state_name(was),
				  (reason != AL_NULL) ? reason : "-");
	}
}

/* ---------------------------------------------------------------------------
 *  状态查询
 * --------------------------------------------------------------------------- */
apu_gasflow_state_t apu_gasflow_state(void)
{
	return g_state;
}

const char *apu_gasflow_state_name(apu_gasflow_state_t st)
{
	switch (st) {
	case APU_GASFLOW_IDLE:       return "IDLE(阀关泵关)";
	case APU_GASFLOW_VALVE_OPEN: return "VALVE_OPEN(开阀等待,泵关)";
	case APU_GASFLOW_PUMPING:    return "PUMPING(吸气中)";
	case APU_GASFLOW_CLOSING:    return "CLOSING(泵已关,等关阀)";
	default:                     return "?";
	}
}

int apu_gasflow_busy(void)
{
	return (g_state != APU_GASFLOW_IDLE) ? 1 : 0;
}

int apu_gasflow_valve_on(void)
{
	return g_valve_on;
}

int apu_gasflow_pump_on(void)
{
	return g_pump_on;
}

uint32_t apu_gasflow_elapsed_ms(void)
{
	if (g_state == APU_GASFLOW_IDLE)
		return 0U;
	return (uint32_t)(apu_time_ms_live() - g_t0);
}

/* ---------------------------------------------------------------------------
 *  打印状态（控制台 @gas 用）
 * --------------------------------------------------------------------------- */
void apu_gasflow_dump(const char *tag)
{
	uint32_t now = apu_time_ms_live();

	apu_console_print("[GASFLOW]%s state=%s  valve=%d pump=%d  elapsed=%ums  "
			  "总跑完=%u 次, 超时收尾=%u 次",
			  (tag != AL_NULL) ? tag : "",
			  apu_gasflow_state_name(g_state),
			  g_valve_on, g_pump_on,
			  (unsigned int)((g_state == APU_GASFLOW_IDLE)
					 ? 0U : (now - g_t0)),
			  (unsigned int)g_run_count,
			  (unsigned int)g_timeout_count);

	if (g_state != APU_GASFLOW_IDLE) {
		uint32_t in_phase = (uint32_t)(now - g_phase_ms);

		switch (g_state) {
		case APU_GASFLOW_VALVE_OPEN:
			apu_console_print("           本阶段已 %ums / 目标 %ums"
					  "（到点即开泵）",
					  (unsigned int)in_phase,
					  (unsigned int)APU_GASFLOW_VALVE_SETTLE_MS);
			break;
		case APU_GASFLOW_PUMPING:
			apu_console_print("           本阶段已 %ums / 目标 %ums"
					  "（到点即关泵）",
					  (unsigned int)in_phase,
					  (unsigned int)APU_GASFLOW_PUMP_RUN_MS);
			break;
		case APU_GASFLOW_CLOSING:
			apu_console_print("           本阶段已 %ums / 目标 %ums"
					  "（到点即关阀）",
					  (unsigned int)in_phase,
					  (unsigned int)APU_GASFLOW_PUMP_OFF_DELAY_MS);
			break;
		default:
			break;
		}
	}
}

/* ---------------------------------------------------------------------------
 *  命令处理（控制台 / rpmsg 共用同一条路径）
 * ---------------------------------------------------------------------------
 *  回执设计（给 Web 按钮用）：
 *      启动成功 -> "ACK: GAS SAMPLE STARTED"
 *      重复启动 -> "ACK: ERR: GAS BUSY"
 *      GPIO 坏  -> "ACK: ERR: GAS GPIO NOT READY"
 *      停止     -> "ACK: GAS STOPPED"
 *  （apu_send_ack 会自动加 "ACK: " 前缀，见 apu_msg_api.h）
 *
 *  ⚠️ 这些字符串是**给 Web 做字符串匹配**的，改文案必须同步改交接文档。
 *  ⚠️ 用 apu_send_ack() 会带上 "ACK: "；对"错误"也要用同一接口，
 *     因为 Linux 侧 status_from_packet() 把 0x0003 整条文本放进 control_ack，
 *     前端本来就是按子串匹配的（见 comm.py:apply_control_feedback）。
 * --------------------------------------------------------------------------- */
int apu_gasflow_handle_command(const char *cmd)
{
	int rc;

	if (cmd == AL_NULL)
		return 0;

	/* ---- 启动 ---- */
	if ((strcmp(cmd, "CMD:GAS_SAMPLE_START") == 0) ||
	    (strcmp(cmd, "CMD:GAS_START") == 0)) {
		rc = apu_gasflow_start();

		if (rc == 0)
			(void)apu_send_ack("GAS SAMPLE STARTED");
		else if (rc == 1)
			(void)apu_send_ack("ERR: GAS BUSY");
		else
			(void)apu_send_ack("ERR: GAS GPIO NOT READY");
		return 1;
	}

	/* ---- 停止（急停）---- */
	if ((strcmp(cmd, "CMD:GAS_STOP") == 0) ||
	    (strcmp(cmd, "CMD:GAS_SAMPLE_STOP") == 0)) {
		if (g_state == APU_GASFLOW_IDLE) {
			apu_console_print("[GASFLOW] STOP: already idle (valve/pump both OFF)");
			apu_gasflow_dump(" stop");
		} else {
			apu_gasflow_force_off("CMD:GAS_STOP");
			apu_gasflow_dump(" stopped");
		}
		(void)apu_send_ack("GAS STOPPED");
		return 1;
	}

	return 0;
}
