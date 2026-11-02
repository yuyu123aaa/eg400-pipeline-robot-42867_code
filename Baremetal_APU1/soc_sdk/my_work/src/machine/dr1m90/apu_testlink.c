/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_testlink.c —— 本机串口控制台实现（排障用，**不是消息出口**）
 * ===========================================================================
 *  【串口控制台命令表】
 *
 *    ---- 本地指令（@ 开头，只影响本机状态）----
 *      @help                打印这份帮助
 *      @mode                打印 tick/时间/串口收字节数/收发统计
 *      @stat                打印 LED/I2C/超声损坏 等状态
 *      @hb                  模拟收到一次 Linux 心跳（PING）—— 喂心跳门
 *      @nav  @ctrl          安全门 / 控制与 ESC 状态
 *      @wdt  @wdt:regs      看门狗状态 / dump WDT+PMU 寄存器
 *      @i2c:<0|1>           重扫一条 I2C 总线
 *      @i2cprobe:<bus>,<addr>,<reg>
 *      @sonar:read  @sonar:stat  @sonar:reset
 *      @imu:stat  @imu:raw  @imu:loop
 *      @esc:stop  @esc:regs
 *      @rep:on @rep:off @rep   周期上报开关（上电默认开）
 *      @turn  @turn:<ms>       弯道模式状态 / 设置持续时间（默认 4000ms）
 *      @shutdown            模拟 DEVICE_CORE_STOP（关核：停电机+关灯+退出主循环）
 *
 *    ---- 远程指令（原样交给业务层 apu_app_handle_command()，
 *         与 rpmsg 收到 DEVICE_CORE_CHECK 时是同一个函数）----
 *      CMD:LED1ON / CMD:LED1OFF / CMD:LED2ON / CMD:LED2OFF
 *      CMD:START / CMD:STOP / CMD:MOTOR_START / CMD:MOTOR_STOP
 *      CMD:CALIBRATE / CMD:MANUAL_START / CMD:WDT_TEST
 *      CMD:YAW:LEFT / CMD:YAW:RIGHT   航向基准 ±76°（仅自动巡航中有效，进入弯道模式）
 *      CMD:DIVE / CMD:DIVE:STOP
 *                                     下潜 ch8/ch9（**命令不带数据**！
 *                                     两路速度是参数：CTRL: 第16/17个 = dive_left/dive_right）
 *                                     仅运行态(自动巡航或手动)有效；STOP 永远放行；
 *                                     任何停车命令都会把这两路一起停
 *      SPEED:<油门>,<转向>
 *      CTRL: / PID: / YAWD: / TRIM: / HEADKP: / YAWMIX: / bs= / CPID: / aw= / mode=
 *      PING:<n>             等价于收到一条心跳（会经 rpmsg 回 PONG）
 *
 *  ⚠️ 为什么 `@` 开头：这样本地指令永远不可能和 Linux 真命令撞名。
 *
 *  ⚠️ 本控制台**不发送**任何上行消息：上行一律走 rpmsg（见 apu_link.h）。
 *     这里只是"把人手敲的命令喂进业务层"+"打印本机状态"。
 *
 *  ⚠️ 2026-09-25 删除：`@link:test` / `@link:prod`（链路模式切换）、
 *     `@hb:off`、`@esc:<ch>,<pct>`（台架单通道强驱）、以及传感器注入类命令。
 * ===========================================================================
 */

#include "apu_testlink.h"

#include <stdio.h>
#include <string.h>

#include "al_core.h"

#include "apu_app.h"
#include "apu_console.h"
#include "apu_i2c.h"
#include "apu_imu.h"
#include "apu_led.h"
#include "apu_link.h"
#include "apu_msg.h"
#include "apu_msg_api.h"     /* @rep:on / @rep:off 周期上报开关 */
#include "apu_nav_safety.h"
#include "apu_pca9685.h"
#include "apu_sonar.h"
#include "apu_gasflow.h"     /* @gas / @gas:start / @gas:stop 气路控制 */
#include "apu_time.h"
#include "apu_wdt.h"

/* ===========================================================================
 * 帮助
 * =========================================================================== */
void apu_testlink_print_help(void)
{
	apu_console_print("----- APU1 本机串口控制台 命令表（上行仍走 rpmsg）-----");
	apu_console_print("  本地: @help @mode @stat @hb");
	apu_console_print("        @imu:stat 姿态状态   @imu:loop UART0回环自检(T5/U5短接)");
	apu_console_print("        @imu:raw 看线上在收什么   @sonar:read @sonar:stat @sonar:reset");
	apu_console_print("        ⚠️ 没有注入命令：传感器数据只来自真外设（与传承一致）");
	apu_console_print("        @i2c:<0|1>  @i2cprobe:<bus>,<addr>,<reg>");
	apu_console_print("        @nav  安全门状态        @ctrl  控制/ESC 状态");
	apu_console_print("        @wdt  看门狗状态  @wdt:regs dump WDT/PMU 寄存器");
	apu_console_print("        @esc:stop  直接停四路推进器（纯停车）");
	apu_console_print("        @esc:regs  把 PCA9685 寄存器读回来(判断PWM有没有真写进芯片)");
	apu_console_print("                   带 INV 标记的通道 = 输出已取反(三相线接反的软件补偿)");
	apu_console_print("        ⚪ 台架单通道强驱 @esc:<ch>,<pct> 已于 2026-09-25 删除；");
	apu_console_print("           要单路动作请走正规路径 CMD:MANUAL_START 然后 SPEED:<油门>,<转向>");
	apu_console_print("        @rep:on @rep:off 周期上报开/关(上电默认开)  @rep 状态");
	apu_console_print("        @turn 弯道模式状态   @turn:<ms> 设置持续时间(500~15000ms)");
	apu_console_print("              CMD:YAW:LEFT/RIGHT 后进入: 居中PID关/降速/降限幅/加阻尼");
	apu_console_print("        ★ @gas 气路状态   @gas:start 开始吸气   @gas:stop 急停");
	apu_console_print("          时序: 开阀 → 2.0s → 开泵 → 吸 6.0s → 关泵 → 0.5s → 关阀");
	apu_console_print("          ⚠️ 顺序铁律: 开=阀先泵后, 关=泵先阀后 (由状态机保证)");
	apu_console_print("          ⚠️ 会真实动作继电器和泵, 按前确认液路接好");
	apu_console_print("        @shutdown 模拟 DEVICE_CORE_STOP（关核: 停电机+关灯+退出主循环）");
	apu_console_print("  远程: CMD:LED1ON CMD:LED1OFF CMD:LED2ON CMD:LED2OFF");
	apu_console_print("        CMD:GAS_SAMPLE_START / CMD:GAS_STOP   气路吸样开始/急停");
	apu_console_print("        CMD:START CMD:STOP CMD:MOTOR_START CMD:MOTOR_STOP");
	apu_console_print("        CMD:CALIBRATE CMD:MANUAL_START CMD:WDT_TEST");
	apu_console_print("        CMD:YAW:LEFT CMD:YAW:RIGHT  航向基准±76°(仅自动巡航中; 进入弯道模式; 回 ACK: YAW TARGET <角度>)");
	apu_console_print("        CMD:DIVE / CMD:DIVE:STOP  下潜ch8/ch9(不带数据; 速度=CTRL:第16/17个)");
	apu_console_print("        SPEED:<油门>,<转向>            例: SPEED:30,-15");
	apu_console_print("        PING:<n>                      例: PING:1  (会回 PONG)");
	apu_console_print("  ⚠️ 本控制台**不是**第二条消息出口：它只是把命令喂给业务层");
	apu_console_print("     （和 rpmsg 收到 DEVICE_CORE_CHECK 时是同一个函数）。");
	apu_console_print("     上行消息一律走 rpmsg，不会打到串口。");
	apu_console_print("-------------------------------");
}

/* ===========================================================================
 * 本地指令处理
 * =========================================================================== */
static int handle_at_command(const char *line)
{
	/* ---- @help ---- */
	if (strcmp(line, "@help") == 0) {
		apu_testlink_print_help();
		return 1;
	}

	/* ---- @mode ---- */
	if (strcmp(line, "@mode") == 0) {
		uint32_t tx_ok = 0U, tx_fail = 0U;

		apu_link_get_stats(&tx_ok, &tx_fail);
		apu_console_print("@mode: link=RPMsg-only (no switch)  tx_ready=%d  "
				  "tick_10ms=%u  ms=%u  rx_bytes=%u  ovf=%u",
				  apu_link_tx_ready(),
				  (unsigned int)apu_tick_10ms(),
				  (unsigned int)apu_time_ms(),
				  (unsigned int)apu_console_rx_bytes(),
				  (unsigned int)apu_console_overflow_count());
		apu_console_print("       tx ok=%u fail=%u   (fail 在涨 = Linux 端点没绑定或 rpmsg_send 失败)",
				  (unsigned int)tx_ok, (unsigned int)tx_fail);
		return 1;
	}

	/* ---- @stat ---- */
	if (strcmp(line, "@stat") == 0) {
		uint32_t ok = 0U, fail = 0U;

		apu_link_get_stats(&ok, &fail);
		apu_console_print("@stat: link tx ok=%u fail=%u  hb_healthy=%d",
				  (unsigned int)ok, (unsigned int)fail,
				  apu_app_host_heartbeat_healthy());
		apu_console_print("       led: cmd_state=0x%x  actual led1=%d led2=%d",
				  (unsigned int)apu_app_led_state(),
				  apu_led_read_back(1), apu_led_read_back(2));
		apu_console_print("       sonar_damage=0x%x  i2c_last_abort bus0=0x%08x bus1=0x%08x",
				  (unsigned int)apu_app_sonar_damage(),
				  (unsigned int)apu_i2c_last_abort_source(APU_I2C_BUS0),
				  (unsigned int)apu_i2c_last_abort_source(APU_I2C_BUS1));
		return 1;
	}

	/* ---- @i2c:<0|1>  重新扫描一条 I2C 总线 ---- */
	if (strncmp(line, "@i2c:", 5) == 0) {
		int bus = -1;

		if (sscanf(line, "@i2c:%d", &bus) == 1 &&
		    bus >= 0 && bus < APU_I2C_NUM_BUS) {
			int n = apu_i2c_scan((apu_i2c_bus_t)bus);

			apu_console_print("@i2c: bus%d scan returned %d device(s)", bus, n);
		} else {
			apu_console_print("@i2c: bad bus, expect @i2c:0 or @i2c:1");
		}
		return 1;
	}

	/* ---- @i2cprobe:<bus>,<addr>,<reg>  读一个寄存器（验证读写一致性） ---- */
	if (strncmp(line, "@i2cprobe:", 10) == 0) {
		int bus = -1, addr = -1, reg = -1;
		uint8_t val = 0U;

		if (sscanf(line, "@i2cprobe:%d,%d,%d", &bus, &addr, &reg) == 3 &&
		    bus >= 0 && bus < APU_I2C_NUM_BUS &&
		    addr >= 0 && addr <= 0x7F && reg >= 0 && reg <= 0xFF) {
			int ret = apu_i2c_read_reg((apu_i2c_bus_t)bus,
						   (uint8_t)addr, (uint8_t)reg, &val);

			if (ret == 0) {
				apu_console_print("@i2cprobe: bus%d dev0x%02X reg0x%02X = 0x%02X (%u)",
						  bus, addr, reg, (unsigned int)val,
						  (unsigned int)val);
			} else {
				apu_console_print("@i2cprobe: FAILED ret=%d (bus%d dev0x%02X reg0x%02X)",
						  ret, bus, addr, reg);
				apu_i2c_dump_last_abort((apu_i2c_bus_t)bus);
			}
		} else {
			apu_console_print("@i2cprobe: bad format, expect @i2cprobe:<bus>,<addr>,<reg>");
		}
		return 1;
	}

	/* ---- @rep:on / @rep:off / @rep  周期上报开关（排障时静音用）---- */
	if (strcmp(line, "@rep:on") == 0) {
		apu_msg_set_periodic_report(1);
		apu_console_print("@rep:on: periodic reports ENABLED (10Hz sensor + 1Hz tick)");
		return 1;
	}
	if (strcmp(line, "@rep:off") == 0) {
		apu_msg_set_periodic_report(0);
		apu_console_print("@rep:off: periodic reports MUTED - 只是让串口清静，"
				  "**不影响 rpmsg 上报**（事件型 ACK/PONG/FAULT 照发）");
		return 1;
	}
	if (strcmp(line, "@rep") == 0) {
		apu_console_print("@rep: periodic reports %s  (上电默认 ENABLED；复位即恢复)",
				  apu_msg_periodic_report_enabled() ? "ENABLED" : "MUTED");
		return 1;
	}

	/* ---- @turn / @turn:<ms> 弯道模式（2026-10-26）---- */
	if (strcmp(line, "@turn") == 0) {
		apu_console_print("@turn: %s, duration=%u ms",
				  apu_app_turn_mode_active() ? "ACTIVE" : "idle",
				  (unsigned)apu_app_turn_mode_ms());
		return 1;
	}
	if (strncmp(line, "@turn:", 6) == 0) {
		unsigned int ms = 0U;

		if (sscanf(line, "@turn:%u", &ms) == 1) {
			apu_app_set_turn_mode_ms((uint32_t)ms);
			apu_console_print("@turn: duration set to %u ms (clamped 500~15000)",
					  (unsigned)apu_app_turn_mode_ms());
		} else {
			apu_console_print("@turn: bad format, expect @turn:<ms>");
		}
		return 1;
	}

	/* ⚪ 2026-09-25：`@link:test` / `@link:prod` **已删除**。
	 *  串口替代出口整段移除后不再有"链路模式"这个概念（见 apu_link.h 顶部）：
	 *  本固件只有一个出口 —— rpmsg_send。 */

	/* ---- @hb：模拟收到一次主机心跳 ---- */
	if (strcmp(line, "@hb") == 0) {
		(void)apu_app_note_host_heartbeat();
		apu_console_print("@hb: injected one host heartbeat (watchdog fed)");
		return 1;
	}

	/* ---- @hb:off  ★ 2026-09-25 **已删除** ----
	 *  原因：它调用的 apu_app_test_expire_heartbeat() 是测试期脚手架
	 *  （人工把"最后心跳时刻"推老、逼看门狗触发），与传承工程无关，已删。
	 *  要验"心跳丢失会制动"：敲完 @hb 之后**不要再敲**，2.5s 后看门狗自会触发。 */

	/* ---- @imu:<r> <p> <y>  ★ 2026-09-21 **已删除** ----
	 *  原因：业务层的姿态数据**只能来自真外设**（与传承一致）。
	 *  要造非法值试安全门，请用假数据源或改驱动，不要再加注入命令。 */

	/* ---- @imu:stat ---- */
	if (strcmp(line, "@imu:stat") == 0) {
		float r = 0.0f, p = 0.0f, y = 0.0f;

		apu_imu_get_angles(&r, &p, &y);
		apu_console_print("@imu:stat seen=%d stale=%d tick=%u  R=%.2f P=%.2f Y=%.2f  "
				  "rx_bytes=%u lines_ok=%u lines_bad=%u ovf=%u",
				  apu_imu_seen(), apu_imu_is_stale(),
				  (unsigned int)apu_imu_last_tick(), r, p, y,
				  (unsigned int)apu_imu_rx_bytes(),
				  (unsigned int)apu_imu_lines_ok(),
				  (unsigned int)apu_imu_lines_bad(),
				  (unsigned int)apu_imu_overflow());
		return 1;
	}

	/* ---- @nav  导航安全门状态（P6）---- */
	/* ---- @imu:loop  ★ 2026-09-20  UART0 回环自检 ----
	 * ---------------------------------------------------------------------------
	 *  目的：把"安路这一侧"从"IMU 外设那一侧"里**完全摘出来**单独验。
	 *  做法：在 UART0 TX(Cep pin7 = T5) 上直接发一行标准 angle: 文本；
	 *        用杜邦线把 T5 和 U5(Cep pin8 = uart0_rx) 短接，这行就会从自己的
	 *        RX 回来，走完整的 中断 -> 环形缓冲 -> 攒行 -> 解析 -> 上报 链路。
	 *  ⚠️ 做之前先把 T5/U5 跟外部（IMU 板）断开，别让别人的输出跟本板对打。
	 *  判读：
	 *    回环后 @imu:stat 的 seen 变 1、lines_ok +1、rx_bytes +25
	 *      -> UART0 收发/中断/解析/上报 **全是好的**，问题在外部（接线/波特率/协议）
	 *    回环后什么都没变
	 *      -> 问题就在安路这一侧，与外设无关
	 * --------------------------------------------------------------------------- */
	if (strcmp(line, "@imu:loop") == 0) {
		int n = apu_imu_selftest_loopback();

		if (n < 0) {
			apu_console_print("@imu:loop: TX FAILED - UART0 not up, or TX never empty (bounded, no hang)");
		} else {
			apu_console_print("@imu:loop: TX done, %d bytes total (%u lines x 25)",
					n, (unsigned int)APU_IMU_REPORT_DIV);
			apu_console_print("  loopback: jumper Cep pin7(T5=uart0_tx) to pin8(U5=uart0_rx)");
			apu_console_print("  then @imu:stat -> expect rx_bytes +%d, lines_ok +%u, seen=1",
					n, (unsigned int)APU_IMU_REPORT_DIV);
			apu_console_print("  with @rep:on this fits exactly one report period");
			apu_console_print("  ⚠️ 上报走 rpmsg，**不会**打在串口上；判据看 @imu:stat 的");
			apu_console_print("     seen=1 / lines_ok 增加，以及 Linux 侧收到 R:1.23,P:-0.45,Y:178.90");
		}
		return 1;
	}

	/* ---- @imu:raw  ★ 2026-09-20  看线上到底在收什么 ----
	 *  判读：
	 *    "其它"占绝大多数        -> 波特率错配，或对方发的是二进制协议
	 *    可打印很多但没有 CR/LF  -> 波特率对，但对方没发换行
	 *    可打印 + CR/LF 都有     -> 数据本身是对的，再去查解析/上报
	 * --------------------------------------------------------------------------- */
	if (strcmp(line, "@imu:raw") == 0) {
		static const char kHexDig[] = "0123456789ABCDEF";
		uint32_t np = 0U, nc = 0U, no = 0U;
		uint8_t  snap[32];
		uint32_t sn, i, hn = 0U;
		char hex[32 * 3 + 1];
		char asc[32 + 1];

		apu_imu_byte_classes(&np, &nc, &no);
		sn = apu_imu_raw_snapshot(snap, (uint32_t)sizeof(snap));

		apu_console_print("@imu:raw: 字节分类  可打印ASCII=%u  CR/LF=%u  其它(控制/高位)=%u",
					(unsigned int)np, (unsigned int)nc, (unsigned int)no);

		for (i = 0U; i < sn; i++) {
			hex[hn++] = kHexDig[(snap[i] >> 4) & 0x0FU];
			hex[hn++] = kHexDig[snap[i] & 0x0FU];
			hex[hn++] = ' ';
			asc[i] = (snap[i] >= 0x20U && snap[i] <= 0x7EU) ? (char)snap[i] : ' ';
		}
		hex[hn] = '\0';
		asc[sn] = '\0';

		apu_console_print("@imu:raw: 最近 %u 字节 hex: %s", (unsigned int)sn, hex);
		apu_console_print("@imu:raw: 同上 ascii    : %s", asc);
		return 1;
	}

	if (strcmp(line, "@nav") == 0) {
		int f = 0, l = 0, r = 0;
		float rl = 0.0f, pt = 0.0f, yw = 0.0f;

		apu_app_get_sonar(&f, &l, &r);
		apu_app_get_attitude(&rl, &pt, &yw);

		apu_console_print("@nav: fault=%d (%s)  bypass=%s  last_reported=%d",
				  apu_app_nav_fault(),
				  apu_app_nav_fault_name(),
				  NAV_SAFETY_BYPASS_FOR_TUNING ? "ON(!!)" : "off",
				  apu_app_last_reported_nav_fault());
		apu_console_print("      imu_seen=%d imu_stale=%d  sonar F=%d L=%d R=%d  "
				  "yaw=%.2f target_yaw=%.2f",
				  apu_app_imu_seen(), apu_imu_is_stale(),
				  f, l, r, yw, apu_app_target_yaw());
		return 1;
	}

	/* ---- @ctrl  控制状态（P8）---- */
	if (strcmp(line, "@ctrl") == 0) {
		int th = 0, tn = 0;

		apu_app_get_remote_input(&th, &tn);
		apu_console_print("@ctrl: motor_enabled=%d remote_mode=%d  "
				  "remote throttle=%d turn=%d  target_yaw=%.2f",
				  apu_app_motor_enabled(), apu_app_remote_mode(),
				  th, tn, apu_app_target_yaw());
		apu_console_print("       ESC off_tick: L-rear=%u R-rear=%u L-front=%u R-front=%u "
				  "dive-A=%u dive-B=%u",
				  (unsigned int)apu_esc_get_off_tick(APU_ESC_LEFT_REAR),
				  (unsigned int)apu_esc_get_off_tick(APU_ESC_RIGHT_REAR),
				  (unsigned int)apu_esc_get_off_tick(APU_ESC_LEFT_FRONT),
				  (unsigned int)apu_esc_get_off_tick(APU_ESC_RIGHT_FRONT),
				  (unsigned int)apu_esc_get_off_tick(APU_ESC_DIVE_A),
				  (unsigned int)apu_esc_get_off_tick(APU_ESC_DIVE_B));
		{
			int   dd = 0;
			float dl = 0.0f, dr = 0.0f;
			float pl = 0.0f, pr = 0.0f;

			apu_esc_get_dive(&dd, &dl, &dr);
			apu_app_get_dive_params(&pl, &pr);
			apu_console_print("       下潜电机: %s  ch8(A)=%+.1f%% ch9(B)=%+.1f%%   "
					  "速度参数(CTRL: 第16/17个)=%+.1f/%+.1f",
					  (dd != 0) ? "开(ON)" : "停(STOP)",
					  dl, dr, pl, pr);
		}
		return 1;
	}

	/* ---- @shutdown  模拟 Linux 发来的 DEVICE_CORE_STOP（★ 关整个核！）----
	 * ---------------------------------------------------------------------------
	 *  为什么需要它：
	 *    DEVICE_CORE_STOP(0x0002) 是**协议命令码**，由**传输层** apu_dispatch()
	 *    （apu_msg_impl.c）分派，**不经过** apu_app_handle_command()；
	 *    而本控制台是**直接调 apu_app_handle_command()** 的
	 *    （本文件 apu_testlink_poll），**绕过了 apu_dispatch**。
	 *    => 所以只敲控制台命令**验不到** 0x0002 那条分支（断点也不会命中）。
	 *
	 *  这条命令**调用与 apu_dispatch 里完全相同的两步**，所以验的是同一份代码：
	 *      apu_app_on_shutdown()   → 停电机 + 关灯 + 业务层标志
	 *      apu_request_shutdown()  → 置传输层标志
	 *  之后 main.c 的测试主循环会 break，打印 "Stopping application..." 收尾。
	 *
	 *  ⚠️ 和 CMD:STOP 的区别（很容易混）：
	 *      CMD:STOP  = 只停电机（命令码 0x0003 的文本命令），**核还在跑**
	 *      @shutdown = 关整个核（对应命令码 0x0002），**跑完就退出主循环了**
	 * --------------------------------------------------------------------------- */
	if (strcmp(line, "@shutdown") == 0) {
		apu_console_print("@shutdown: 模拟 DEVICE_CORE_STOP -> 停电机+关灯，然后退出主循环");
		apu_console_print("  ⚠️ 从这之后串口不再响应任何命令（核已停）；要恢复请重新点 Debug 下载运行");
		apu_app_on_shutdown();
		apu_request_shutdown();
		return 1;
	}

	/* ---- @esc:stop  直接停**全部 6 路**（不经过业务层，用于安全排查）----
	 *  ⚠️ 这两条（@esc:stop / @esc:regs）**保留**：
	 *     @esc:stop 是纯停车（安全价值高），@esc:regs 只读 PCA9685 寄存器
	 *     （判断"PWM 到底写进芯片没有"的排障刚需）。
	 *     被删掉的是"单通道强驱 @esc:<ch>,<pct>"那个台架直通口。 */
	if (strcmp(line, "@esc:stop") == 0) {
		apu_esc_stop_all();
		apu_console_print("@esc:stop: all 6 channels (ch4~ch9) -> neutral "
				  "(L-rear off_tick=%u, dive-A off_tick=%u)",
				  (unsigned int)apu_esc_get_off_tick(APU_ESC_LEFT_REAR),
				  (unsigned int)apu_esc_get_off_tick(APU_ESC_DIVE_A));
		return 1;
	}

	/* ⚪ 2026-09-25：原 `@esc:<通道>,<百分比>`（P7 台架单通道强驱）整段已删除。
	 *  它故意绕过心跳门/手动模式/安全门，且只在"桨叶有保护"的台架场景成立；
	 *  与传承工程无关（传承只有 ESC_SetSpeed 一条正常控制路径）。
	 *  顺带避掉了老注释里提到的 F-26 前缀匹配陷阱（那段通配分支已不存在）。
	 *  需要单路动作：CMD:MANUAL_START -> SPEED:<油门>,<转向>（会过安全门）。 */

	/* ---- ★ @esc:regs  把 PCA9685 的寄存器**读回来**（判断写到底有没有生效）----
	 * ---------------------------------------------------------------------------
	 *  为什么需要它：`@esc:` 打出来的 off_tick 只是**软件里记的值**，
	 *  即使 I2C 写失败它也照打不误（pca9685_set_pwm 的返回值被丢掉了）。
	 *  这个命令把芯片里的真实内容读回来，才是"写进去了没"的铁证。
	 *
	 *  判读：
	 *    MODE1 读回 0x20 左右（bit4 SLEEP=0、bit5 AI=1）-> 初始化写进去了
	 *    MODE1 读回 0x11（上电默认：SLEEP=1）      -> **写压根没生效**，芯片还在睡
	 *    某路 OFF 读回 0 -> 那一路的 PWM 没写进去（或被清了）
	 *    wr/err 的 err 在涨 -> I2C 写失败
	 *  ⚠️ 必须放在 @esc:<ch>,<pct> 通配分支**之前**，否则会被它吃掉（F-26）。
	 * --------------------------------------------------------------------------- */
	if (strcmp(line, "@esc:regs") == 0) {
		uint8_t m1 = 0U, m2 = 0U, ps = 0U;
		int r0, r1, r2, i;

		apu_console_print("@esc:regs BEGIN (I2C0 dev0x40 读回；寄存器地址十六进制)");
		r0 = apu_pca9685_read_reg(0x00U, &m1);
		r1 = apu_pca9685_read_reg(0x01U, &m2);
		r2 = apu_pca9685_read_reg(0xFEU, &ps);
		apu_console_print("  MODE1(0x00)=0x%02X ret=%d   MODE2(0x01)=0x%02X ret=%d   "
				  "PRESCALE(0xFE)=0x%02X ret=%d",
				  m1, r0, m2, r1, ps, r2);
		if (m1 == 0x11U) {
			apu_console_print("  !! MODE1 还是上电默认 0x11(SLEEP=1) -> "
					  "I2C 写【没生效】，芯片没被唤醒，输出全部关闭");
		} else if ((m1 & 0x10U) != 0U) {
			apu_console_print("  !! MODE1 bit4(SLEEP)=1 -> 芯片在睡眠，输出关闭");
		} else {
			apu_console_print("  OK: MODE1 bit4(SLEEP)=0 -> 芯片已唤醒，输出使能");
		}

		/* ★ 2026-10-05：由 4 路扩到 6 路（ch4~ch9）——
		 *  不扩的话新引出的 ch8/ch9 在芯片侧根本无法验证。
		 * ★ 2026-10-23：**必须经 apu_esc_get_phys_channel() 换算物理通道**。
		 *  左后已从物理 ch4 改接到物理 ch10；再拿逻辑号直接算 0x06+4*ch，
		 *  读到的会是那路废弃的坏通道，自检结论全是错的。 */
		for (i = 0; i < APU_ESC_CH_COUNT; i++) {
			uint8_t ch   = (uint8_t)(APU_ESC_CH_FIRST + i);
			uint8_t phys = apu_esc_get_phys_channel(ch);
			uint8_t reg;
			uint8_t onl = 0U, onh = 0U, offl = 0U, offh = 0U;
			int a, b, c, d;
			unsigned int off;

			if (phys == 0xFFU) {
				apu_console_print("  ch%d: 物理通道映射无效(phys=0xFF) -> 跳过",
						  ch);
				continue;
			}

			reg = (uint8_t)(0x06U + 4U * phys);

			a = apu_pca9685_read_reg(reg, &onl);
			b = apu_pca9685_read_reg((uint8_t)(reg + 1U), &onh);
			c = apu_pca9685_read_reg((uint8_t)(reg + 2U), &offl);
			d = apu_pca9685_read_reg((uint8_t)(reg + 3U), &offh);

			off = (unsigned int)offl | ((unsigned int)offh << 8);

			apu_console_print("  ch%d->phys%d(silk%d) reg0x%02X: ON=%u,%u "
					  "OFF=(0x%02X,0x%02X)=%u -> %.3f ms   sw=%u%s  "
					  "ret=%d/%d/%d/%d",
					  ch, phys, phys + 1, reg, onl, onh, offh, offl, off,
					  (double)off * 4.883 / 1000.0,
					  (unsigned int)apu_esc_get_off_tick(ch),
					  apu_esc_get_invert(ch) ? " INV" : "",
					  a, b, c, d);
		}

		apu_console_print("  wr=%u err=%u  (err 在涨 = I2C 写失败)",
				  (unsigned int)apu_pca9685_write_count(),
				  (unsigned int)apu_pca9685_err_count());
		apu_console_print("  中位应为 逻辑ch4~ch9 = 330/328/330/330/330/330 (约 1.61 ms)；"
				  "全部读 0 = PWM 从未写进芯片");
		apu_console_print("  注意: 逻辑ch4(左后) 已改接->物理ch10 (原物理ch4 输出异常,"
				  " 已废弃并写无脉冲)");
		return 1;
	}

	/* ⚪ 2026-09-25：原 `@esc:off` / `@esc` 与 `@esc:<ch 4..7>,<pct>` **整段删除**。
	 *  它们是"绕过心跳门/手动模式/安全门、单通道强驱推进器"的台架工具，
	 *  与传承工程无关（传承只有 ESC_SetSpeed() 一条正常控制路径），
	 *  而且会掩盖真实控制链路的问题。
	 *  需要单独转某个电机时请走正规路径：CMD:MANUAL_START -> SPEED:<油门>,<转向>。
	 *  纯停车仍可用 @esc:stop；读芯片寄存器仍可用 @esc:regs。 */

	/* ---- @wdt 硬件看门狗状态（P6）----
	 *  ★ 2026-09-21：加 PMU 回读。block_up=1 只说明寄存器写下去了；
	 *  can_reset 才是"超时真的会不会复位芯片"。 */
	if (strcmp(line, "@wdt") == 0) {
		apu_wdt_pmu_t pmu;

		apu_wdt_pmu_read(&pmu);
		apu_console_print("@wdt: block_up=%d can_reset=%d selftest_armed=%d feeds=%u",
				  apu_wdt_block_up(), apu_wdt_can_reset(),
				  apu_wdt_selftest_armed(),
				  (unsigned int)apu_wdt_feed_count());
		apu_console_print("@wdt: PMU hw_en=%d int_en=%d int_state=%d raw_his=%d",
				  pmu.hw_en, pmu.int_en, pmu.int_state, pmu.raw_his);
	/* ★ 2026-09-21：上次复位原因（硬件记录，跨复位保留）*/
	{
		apu_wdt_rst_reason_t rr;

		if (apu_wdt_last_rst_reason(&rr)) {
			apu_console_print("@wdt: 本次开机读到 RST_REASON=0x%08X "
					  "SWDT0=%d SWDT1=%d SWDT2=%d PMU_ERR=%d JTAG=%d "
					  "SRSTsoft=%d SRSTpin=%d",
					  (unsigned int)rr.raw, rr.swdt0, rr.swdt1, rr.swdt2,
					  rr.pmu_err, rr.jtag_bsu, rr.srst_soft, rr.srst_b);
			if (rr.swdt0 || rr.swdt1 || rr.swdt2) {
				apu_console_print("@wdt: ★ 上次复位【是看门狗干的】-> 硬兜底确实咬过");
			} else if (rr.pmu_err) {
				apu_console_print("@wdt: ★ 上次复位由 PMU 错误触发");
			} else if (rr.raw != 0U) {
				apu_console_print("@wdt: 上次复位不是看门狗（JTAG/软件/复位引脚）");
			}
	} else {
			apu_console_print("@wdt: RST_REASON 还没读到（init 没跑过？）");
	}
	}

	if (!apu_wdt_block_up()) {
			apu_console_print("@wdt: WDT 寄存器块都没起来 -> 只有软件卡死检测。");
		} else if (!apu_wdt_can_reset()) {
			apu_console_print("@wdt: !! 狗在跑但 PMU 没放行复位 (hw_en=0) -> 超时也不会复位芯片!");
		} else {
			apu_console_print("@wdt: OK - 超时会真复位芯片。@wdt:regs 看寄存器");
		}
		return 1;
	}

	/* ---- @sonar:<前>,<左>,<右>  ★ 2026-09-21 **已删除** ----
	 *  原因：业务层的超声数据**只能来自真外设**（与传承一致）。 */

	/* ---- @wdt:regs  直接 dump WDT + PMU 寄存器 ---- */
	if (strcmp(line, "@wdt:regs") == 0) {
		apu_wdt_dump_regs();
		return 1;
	}

	/* ---- @sonar:reset  复位滤波器 ---- */
	if (strcmp(line, "@sonar:reset") == 0) {
		apu_sonar_reset_filters();
		apu_console_print("@sonar:reset: filters cleared");
		return 1;
	}

	/* ---- @sonar:read  读一次真硬件（绕过 10Hz 节流），用于接线排查 ---- */
	if (strcmp(line, "@sonar:read") == 0) {
		apu_sonar_raw_t raw;

		if (apu_sonar_read_raw(&raw) == 0) {
			apu_console_print("@sonar:read OK -> raw L=%u F=%u R=%u mm "
					  "(交换前；真驱动会把 F/R 对调)",
					  (unsigned int)raw.left, (unsigned int)raw.front,
					  (unsigned int)raw.right);
		} else {
			apu_console_print("@sonar:read FAILED (I2C1 dev 0x40)");
			apu_i2c_dump_last_abort(APU_I2C_BUS1);
		}
		return 1;
	}

	/* ---- @sonar:stat ---- */
	if (strcmp(line, "@sonar:stat") == 0) {
		apu_sonar_raw_t filt;

		apu_sonar_get_filtered(&filt);
		apu_console_print("@sonar:stat ok=%u err=%u  filtered F=%u L=%u R=%u mm  damage=0x%x",
				  (unsigned int)apu_sonar_read_ok_count(),
				  (unsigned int)apu_sonar_read_err_count(),
				  (unsigned int)filt.front, (unsigned int)filt.left,
				  (unsigned int)filt.right,
				  (unsigned int)apu_app_sonar_damage());
		apu_console_print("            breaker=%s tripped=%u consecutive_fails=%u",
				  apu_sonar_breaker_open() ? "OPEN(skipping)" : "closed",
				  (unsigned int)apu_sonar_breaker_tripped_count(),
				  (unsigned int)apu_sonar_consecutive_fails());
		return 1;
	}

	/* ---- 气路控制：@gas / @gas:start / @gas:stop ----------------------
	 *  队友（Web/Linux 侧）暂不在，先用本机串口1 手动触发。
	 *  时序铁律由 apu_gasflow.c 的状态机保证：
	 *      开：阀 → 2s → 泵        关：泵 → 0.5s → 阀
	 *  ⚠️ 这会**真实动作硬件**（继电器吸合、泵转动），按之前请确认液路接好。 */
	if ((strcmp(line, "@gas") == 0) ||
	    (strcmp(line, "@gas:stat") == 0)) {
		apu_gasflow_dump(NULL);
		apu_console_print("           命令: @gas:start 开始吸气   "
				  "@gas:stop 急停（立即关泵关阀）");
		return 1;
	}
	if (strcmp(line, "@gas:start") == 0) {
		(void)apu_gasflow_start();
		apu_gasflow_dump(" started");
		return 1;
	}
	if (strcmp(line, "@gas:stop") == 0) {
		apu_gasflow_force_off("@gas:stop");
		apu_gasflow_dump(" stopped");
		return 1;
	}

	/* 其它 @ 开头：明确报错，不要静默 */
	if (line[0] == '@') {
		apu_console_print("unknown local command: %s   (try @help)", line);
		return 1;
	}

	return 0;
}
/* ===========================================================================
 * 初始化
 * =========================================================================== */
int apu_testlink_init(void)
{
	int ret = apu_console_init();

	if (ret != 0)
		return ret;

	apu_console_print("");
	apu_console_print("==================================================");
	apu_console_print(" APU1 本机串口控制台（排障用）");
	apu_console_print(" ⚠️ 本固件上行消息只有一条路：rpmsg。");
	apu_console_print("    这个控制台只把命令喂给业务层 + 打印本机状态，");
	apu_console_print("    不会把任何消息发给 Linux。");
	apu_console_print("==================================================");
	apu_testlink_print_help();
	apu_console_print("");

	return 0;
}

/* ===========================================================================
 * 主循环调用
 * =========================================================================== */
int apu_testlink_poll(void)
{
	char line[APU_CONSOLE_LINE_MAX];

	if (!apu_console_poll(line, sizeof(line)))
		return 0;

	/* ---- 本地指令（@ 开头）---- */
	if (line[0] == '@') {
		(void)handle_at_command(line);
		return 1;
	}

	/* ---- PING:<n>：等价于收到一条 Linux 心跳 ---- */
	if (strncmp(line, APU_HEARTBEAT_PING_PREFIX,
		    strlen(APU_HEARTBEAT_PING_PREFIX)) == 0) {
		unsigned int seq = 0U;

		(void)apu_app_note_host_heartbeat();

		if (sscanf(line, APU_HEARTBEAT_PING_PREFIX "%u", &seq) == 1) {
			/* 走和传输层完全相同的回复路径 */
			apu_link_send(DEVICE_CORE_HEARTBEAT,
				      APU_HEARTBEAT_PONG_PREFIX "%u", seq);
		} else {
			apu_console_print("bad heartbeat format, expect: PING:<n>");
		}
		return 1;
	}

	/* ---- 其它一律当作远程命令，交给业务层（和 RPMsg 路径同一个函数）---- */
	apu_console_print("[CONSOLE] >> %s", line);
	if (!apu_app_handle_command(line))
		apu_console_print("(business layer did not handle this command)");

	return 1;
}
