/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_app.h —— APU1 裸机侧"业务层"对外接口
 * ===========================================================================
 *  【分层位置】
 *
 *      main.c  ──>  apu_app_periodic()  ──>  驱动层 (dr1m90/: led/sonar/imu/pca9685/wdt)
 *         │              │
 *         │              └──>  纯逻辑 (common/: nav_safety / manual / pipe_center / filter)
 *         └──回调──>  apu_dispatch()  ──命令──>  apu_app_handle_command()
 *
 *  【分工】
 *      apu_msg_impl.c  = 传输层：收发、解析、心跳应答、分发（不认识业务）
 *      apu_app.c       = 业务层：命令分支、安全闸门、周期任务、状态变量
 *
 *  【规矩】
 *      · 本文件对应的 .c 里【不许 include 任何 al_*.h】——业务层不碰硬件寄存器
 *      · 但可以调 apu_msg_impl 的发送接口 和 apu_time 的时间接口
 * ===========================================================================
 */

#ifndef APU_APP_H
#define APU_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 一、生命周期
 * =========================================================================== */

/* 业务层初始化：清状态变量、把外设置到安全初态。
 *
 * 位置：main.c 里，rpmsg endpoint 创建**之后**（要能发 ACK 了）再调。
 * P1：只清状态 + 打印自检。
 * P2~P7：往这里加 apu_i2c_init() / apu_led_init() / apu_imu_init() / apu_wdt_init() ...
 *
 * 返回：0 成功；负值表示有外设初始化失败（P1 阶段不会失败）。 */
int apu_app_init(void);

/* 每轮主循环调一次（由 main.c 在 platform_poll() 之后调用）。
 *
 * P1：心跳灯 + 10Hz tick 自检打印 + （临时）演示用上报。
 * P4~P8：往这里加 apu_imu_poll() / apu_sonar_periodic() / 自动控制流水线。 */
void apu_app_periodic(void);

/* 开机自检打印：资源分配表 + 平台配置体检（判断 BOOT.bin 是否生效）。
 *
 * 位置：main.c 里，platform_init() 之后调。**不需要 rpmsg 端点**，
 *       因为它只读寄存器和打印，不发核间消息。 */
void apu_app_selfcheck(void);

/* ===========================================================================
 * 二、命令入口（由 apu_msg_impl.c 的 apu_dispatch() 调用）
 * =========================================================================== */

/* 处理一条文本命令（DEVICE_CORE_CHECK 的载荷）。
 * 返回 1 = 已处理（含"未知指令"）；0 = 不认识，交给上层兜底。
 *
 * 【实现范围】P0~P8 已完成：24 条命令全部接通**真实外设动作**
 *   · 灯   -> apu_led_*          （真 PL GPIO）
 *   · 电机 -> apu_esc_*          （真 PCA9685）
 *   · 闸门 -> apu_nav_evaluate() （真导航安全评估）
 *   ACK 文案与传承项目逐字一致（Linux 侧 comm.py 会做字符串匹配）。 */
int apu_app_handle_command(const char *cmd);

/* 收到 DevId 心跳（PING）时由传输层回调通知，用来喂"主机心跳看门狗"。
 * 返回：1 = 这是启动后第一次收到心跳；0 = 之前已经收到过。 */
int apu_app_note_host_heartbeat(void);

/* ===========================================================================
 * 三、安全动作（所有"停"的场景统一走这里，别各写各的）
 * =========================================================================== */

/* 安全停车：停电机 + 关灯 + 退出手动/自动模式。
 * P1：只打印。
 * P3/P7：加 apu_led_all_off() / apu_esc_stop_all()。 */
void apu_app_safe_stop(const char *reason);

/* 收到 DEVICE_CORE_STOP（关整个裸机核）时的收尾钩子。
 * 由 apu_msg_impl.c 调用，之后 main.c 主循环会退出。 */
void apu_app_on_shutdown(void);

/* ===========================================================================
 * 四、状态查询（自检打印 / 安全闸门用）
 * =========================================================================== */

/* bit0 = 照明灯1，bit1 = 照明灯2（1 = 开） */
uint8_t apu_app_led_state(void);

/* 超声波故障位掩码，见 apu_msg.h 的 APU_SONAR_FAULT_* */
uint32_t apu_app_sonar_damage(void);

/* 主机心跳是否健康（启动宽限 7.5s，之后 2.5s 没收到就判失败） */
int apu_app_host_heartbeat_healthy(void);

/* ===========================================================================
* 五、传感器数据入口（★ **只有真实驱动会调**）
 * ===========================================================================
 *  ★★ 2026-09-21 改动：以前这里是"真驱动 + 串口注入"共用同一个入口，
 *     现在**注入命令已经删掉**，业务层的传感器数据**只能来自真外设** ——
 *     结构与传承项目一致（slaver_00_example.c 里就是 Uart2_Proc / 超声 proc
 *     解析完直接写共享状态，没有任何"注入"概念）。
 *
 *     调用者只有两个：
 *        apu_imu.c   （解析完一行 "angle:r p y"）→ apu_app_update_attitude()
 *        apu_sonar.c （走完 读→交换→判损坏→有效性→滤波）→ apu_app_update_sonar()
 * =========================================================================== */

/* ★ 2026-09-21：**真驱动专用**姿态入口。调用者只有 apu_imu.c。
 *  ⚠️ 传进来的 yaw **必须已经 wrap 到 ±180**（驱动那边做了，见 apu_imu.c）；
 *     这里不再重复 wrap，和传承一致。 */
void apu_app_update_attitude(float roll, float pitch, float yaw);

/* ★ 2026-09-20：按**传承代码的做法**上报姿态（slaver_00_example.c:993-1000）。
 *  传承是在 Uart2_Proc() 里「每收满 10 行有效数据就 send_str_to_linux(
 *  DEVICE_CORE_SENSOR, "R:%.2f,P:%.2f,Y:%.2f")」，所以这里也改成**由解析方主动调**，
 *  而不再由 apu_app_periodic() 按固定 10Hz 定时发。 */
void apu_app_report_attitude(float roll, float pitch, float yaw);

/* ★★ 2026-09-21（**以传承为准**）：超声数据要**分两路**用，不能混。
 *
 *  传承项目就是这么分的（slaver_00_example.c）：
 *    · **安全门** NavSafety_BuildInput()：.front_mm = sonar_raw_front_mm   ← **原始值**
 *      三路有效性标志也都在 Sonar_Proc() 里从**原始**读数算出来（:1051-1056）；
 *      回归测试 tests/test_manual_control_integration.py 专门钉死了这一点
 *      （test_sonar_health_is_based_on_raw_values_not_filtered_fallbacks）。
 *    · **上报 + 居中控制**：sonar_data.*（**滤波值**）
 *      —— 上报 slaver:1078-1087；居中控制 slaver:1134-1135。
 *
 *  ⚠️ **为什么安全门必须用原始值**：滤波器带"跳变抑制"（差值 >400mm 时先用旧值顶住，
 *     连续 3 次才接受）和 5 点中值。1000mm 处突然出现 40mm 的墙时，滤波值要 3 个周期
 *     才接受、再等中值窗口更新 —— 最坏 ~400ms 才反映出来。**安全门等不起**：
 *     用原始值当次就能判出 FRONT_TOO_CLOSE。
 *     另外原始值无效（<20 或 >2000）时驱动**不更新**滤波器，滤波值会"留着旧的有效值"，
 *     拿它判有效性会**该报警时不报警**。
 *
 *  ⚠️ 调用者只有 apu_sonar.c。damage_mask != 0 时**只有掩码有效**，其余字段被忽略
 *     （对应传承"判到损坏就 return，后面的赋值全不执行"）。 */
typedef struct {
	int      front_raw;     /* 原始前向距离 mm（已交换 front/right）-> 安全门 front_mm */
	int      left_raw;      /* 原始左 mm（仅供显示/诊断） */
	int      right_raw;     /* 原始右 mm（仅供显示/诊断） */
	int      front_valid;   /* 三路有效性（从**原始**值算：20..2000mm 为 1） */
	int      left_valid;
	int      right_valid;
	int      ctrl_left;     /* 滤波后左 mm -> 居中控制 */
	int      ctrl_right;    /* 滤波后右 mm -> 居中控制 */
	uint32_t damage_mask;   /* !=0 时只更新故障掩码 */
} apu_sonar_update_t;

void apu_app_update_sonar(const apu_sonar_update_t *u);

/* ⚪ 2026-09-25 删除的测试期接口（不要再恢复）：
 *    · apu_app_test_expire_heartbeat()  —— 只服务控制台 @hb:off
 *    · apu_app_esc_test() / _stop() / _active() —— P7 台架单通道认电机位置
 *  验证"心跳丢失会制动"请直接停掉 Linux 侧心跳线程；
 *  验证"哪个电机是哪个位置"请用 CMD:MANUAL_START + SPEED:（本就不绕过安全门）。 */

/* ===========================================================================
 * 六、安全门输入（P6）
 * ===========================================================================
 *  IMU / 超声波驱动的"最后更新时间"由驱动层提供，业务层组装成
 *  NavSafety 的输入结构并评估。
 * =========================================================================== */

/* 当前导航安全故障（已应用 NAV_SAFETY_BYPASS_FOR_TUNING 旁路开关） */
int apu_app_nav_fault(void);

/* 当前导航安全故障的名字（可读字符串） */
const char *apu_app_nav_fault_name(void);

/* 当前最近一次导航故障（边沿触发上报用），0 = OK */
int apu_app_last_reported_nav_fault(void);

/* ===========================================================================
 * 七、控制状态查询（自检 / 测试控制台用）
 * =========================================================================== */

/* 1 = 自动居中控制已启动（CMD:MOTOR_START 成功） */
int apu_app_motor_enabled(void);

/* 1 = 手动遥控模式已启动（CMD:MANUAL_START 成功） */
int apu_app_remote_mode(void);

/* 当前遥控摇杆值（手动模式下有效，SPEED:x,y 写入） */
void apu_app_get_remote_input(int *throttle_percent, int *turn_percent);

/* 当前航向保持的目标角（度）。设定来源：
 *   CMD:CALIBRATE / CMD:MOTOR_START -> 直接取当前 yaw（IMU 给的 ±180 内）
 *   CMD:YAW:LEFT / CMD:YAW:RIGHT   -> 就地 ±76°（见 apu_app.c 的 APU_YAW_STEP_DEG），
 *                                     并按 §2.4 归一化到 [-180, +180) */
float apu_app_target_yaw(void);

/* ---------------------------------------------------------------------------
 * 弯道模式（2026-10-26）
 * ---------------------------------------------------------------------------
 *  CMD:YAW:LEFT / CMD:YAW:RIGHT 后自动进入，默认 4000 ms：
 *  关闭管道居中 PID、基础速度 ×0.60、转向限幅 ×0.70、偏航阻尼 ×2.0。
 *  到时或航向稳定后自动退出。
 *
 *  apu_app_set_turn_mode_ms()：在线改持续时间（500~15000 ms，越界钳位）；
 *                              正在弯道中改会从当前时刻重新计时。
 *  apu_app_turn_mode_ms()    ：读当前持续时间。
 *  apu_app_turn_mode_active()：1 = 弯道模式进行中。
 * --------------------------------------------------------------------------- */
void apu_app_set_turn_mode_ms(uint32_t ms);
uint32_t apu_app_turn_mode_ms(void);
int apu_app_turn_mode_active(void);

/* ★ 2026-10-12：当前横滚保持的目标角（度，正右倾）。与 apu_app_target_yaw 对称。
 *   设定来源：CMD:CALIBRATE / CMD:MOTOR_START -> 直接取当前 roll；
 *   未标定过时为 0.0（= 要求水平）。 */
float apu_app_target_roll(void);

/* ★ 2026-10-05：下潜两路速度参数（CTRL: 第 16/17 个字段，±100）。
 * 下潜时 ch8/ch9 各自输出的百分比（原样下发，不取反）。给自检/控制台显示用。 */
void apu_app_get_dive_params(float *left_pct, float *right_pct);

/* ★ 2026-10-12：当前横滚稳定修正量（%，正 = ch8 加、ch9 减）。
 * 增益全 0 或下潜没开时恒为 0。诊断用（@ctrl 会打出来）。 */
float apu_app_get_roll_corr(void);

/* 当前超声波三路读数（滤波后 / 已注入，业务层口径） */
void apu_app_get_sonar(int *front_mm, int *left_mm, int *right_mm);

/* 当前姿态角（业务层口径，含测试注入） */
void apu_app_get_attitude(float *roll, float *pitch, float *yaw);

/* IMU 数据是否收到过 */
int apu_app_imu_seen(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_APP_H */
