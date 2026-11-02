/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_msg_api.h —— APU1 裸机侧"传输层"对外接口声明
 * ===========================================================================
 *  实现全部在 apu_msg_impl.c。
 *
 *  【为什么单独一个头文件，而不是写进 apu_msg.h】
 *      apu_msg.h 的身份是"两侧共用的协议**格式**定义"（Linux 侧也有同一份，
 *      sha256 必须一致）。往里加裸机侧的函数声明会破坏这个身份。
 *      所以传输层的函数声明单独放这里。
 *
 *  【分层】
 *      apu_msg.h      协议格式 + 命令码 + 解析辅助（两侧共用，不许改）
 *      apu_msg_api.h  传输层函数声明（本文件）
 *      apu_msg_impl.c 传输层实现
 *      apu_app.c/.h   业务层（调用本文件声明的 apu_send_* 往外发消息）
 * ===========================================================================
 */

#ifndef APU_MSG_API_H
#define APU_MSG_API_H

#include <stdint.h>
#include <stddef.h>

#include "apu_msg.h"          /* data_packet / DEVICE_CORE_* / MAX_DATA_LENGTH */

#ifdef __cplusplus
extern "C" {
#endif

/* openamp 的端点结构，这里只能前置声明（避免业务层被 openamp 头文件污染） */
struct rpmsg_endpoint;

/* ===========================================================================
 * 一、端点登记
 * ===========================================================================
 *  endpoint 创建后必须调 apu_set_endpoint()，否则什么都发不出去
 *  （apu_send 里会检查 ept->dest_addr）。
 * =========================================================================== */
void apu_set_endpoint(struct rpmsg_endpoint *ept);
struct rpmsg_endpoint *apu_get_endpoint(void);

/* ===========================================================================
 * 二、发送（printf 风格，自动按 "6 + length" 语义打包）
 * =========================================================================== */

/* 通用发送。cmd 取 DEVICE_CORE_*；返回 0 成功，-1 失败。 */
int apu_send(struct rpmsg_endpoint *ept, uint32_t cmd, const char *fmt, ...);

/* 回执：自动加 "ACK: " 前缀（那个空格必须保留，Linux 侧靠它识别）。
 * ★ 文案必须与传承项目逐字一致 —— Linux 的 comm.py 会做字符串匹配。 */
int apu_send_ack(const char *fmt, ...);

/* 传感器上报：command = DEVICE_CORE_SENSOR，原样发 fmt。 */
int apu_send_sensor(const char *fmt, ...);

/* 姿态上报 -> "R:%.2f,P:%.2f,Y:%.2f" */
int apu_send_attitude(float roll, float pitch, float yaw);

/* 超声波距离上报 -> "SONAR:<前>,<左>,<右>"
 * （简单版，P1 占位用；正式采集请用下面的 _full 版） */
int apu_send_sonar(int front, int left, int right);

/* ★ 超声波距离上报（完整版，与传承项目逐字一致）：
 *      "SONAR:%d,%d,%d,V:%d%d%d"   —— 前,左,右 + 三路各自的有效性(0/1)
 *      来源 slaver_00_example.c:1078-1087 */
int apu_send_sonar_full(int front, int left, int right,
			int front_valid, int left_valid, int right_valid);

/* 超声波故障上报 -> "FAULT:SONAR:<位掩码>"（APU_SONAR_FAULT_*） */
int apu_send_sonar_fault(unsigned int mask);

/* 超声波恢复正常 -> "RECOVER:SONAR" */
int apu_send_sonar_recover(void);

/* 导航故障上报 -> "FAULT:NAV:<描述>" */
int apu_send_nav_fault(const char *desc);

/* ★ 导航故障上报（完整版，与传承项目逐字一致）：
 *      "FAULT:NAV:%d,%s"   —— 故障码 + 故障名
 *      来源 slaver_00_example.c:1100-1105 */
int apu_send_nav_fault_fmt(int fault_code, const char *fault_name);

/* ===========================================================================
 *  ★★ 环境传感器上报（下潜深度 + 浊度 + 四路气体）★★
 * ===========================================================================
 *  一块消息把 6 个值一起发，command = DEVICE_CORE_SENSOR。
 *
 *  载荷格式（ASCII，与 "SONAR:..." 同一风格）：
 *
 *      ENV:DEPTH:<深度mm>,TURB:<浊度>,GAS:<CO>,<H2S>,<O2>,<CH4>
 *
 *  例（全部有效）：
 *      ENV:DEPTH:172,TURB:3,GAS:0,0,209,0
 *  例（深度和气体无效、只有浊度有效）：
 *      ENV:DEPTH:0,TURB:3,GAS:0,0,0,0
 *
 *  【★ 本次重点：无效数据不上报，用哨兵 0 表示"无数据"】
 *    · 固件层已经把无效值**全部丢弃**（不滤波、不进缓存、不上报）：
 *        - 深度：不在 20~2000mm 内 → 丢弃（含 65533/65535 无回波哨兵）
 *        - 浊度：不在 0~1000 UNT 内 → 丢弃
 *        - 气体：O2 读不到 → 整组气体丢弃
 *      ⇒ 所以**上报出来的深度一定是真值**，绝不会再出现 65533 这种垃圾。
 *
 *    · "无数据"统一用 **0** 表示（而不是旧版那个 `*V` 标志位）：
 *        - `DEPTH:0`  → 本次没测到有效深度（0 在物理上不可能，安全）
 *        - `GAS:0,0,0,0` → 气体整组没读到（O2=0 在物理上不可能，安全）
 *        - `TURB:0`   → ⚠️ **有歧义**：清水浊度本来就是 0。
 *                       要精确区分请结合 Web 侧读到的 `turbidity_valid`
 *                       历史（见下方"给 Web 侧的约定"）。
 *
 *  【滤波】深度和浊度都做了与前三路超声**完全相同**的滤波：
 *      5 点滑动窗口取中值 + 跳变抑制（相邻差 >400 需连续 3 次才认可）。
 *      ⇒ 上报值已经是滤波后的稳定值。
 *
 *  【刷新率】每圈上报一次，约 **1Hz**。
 *      这是主循环的物理上限：APU1 主循环被 APU0 约 1 秒一次的 SGI 踢着走
 *      （platform_poll 等 kick），循环体本身没有延时。
 *      想再快必须改跨核握手节奏，不在传感器这一层。
 *
 *  【单位约定（Web 侧按这个换算显示）】
 *      DEPTH  毫米(mm)          0 = 本次无有效读数
 *      TURB   浊度原始值(UNT)   倍率待水样标定，当前 1:1
 *      GAS co     ppm
 *      GAS h2s    ppm
 *      GAS o2     **0.1 %VOL**，209 表示 20.9 %VOL ← 显示要 ÷10
 *      GAS ch4    %LEL
 * ===========================================================================
 */
int apu_send_env(int depth_mm, int turb_unt,
		 int co, int h2s, int o2, int ch4);

/* ===========================================================================
 * 二·补、周期上报开关（★ 2026-09-25 简化：上电默认开启）
 * ===========================================================================
 *  为什么需要：排障时 10Hz 的姿态/超声上报 + 1Hz 的 tick 自检行会把日志串口刷满，
 *  命令回显和 ACK 全被埋掉，人没法交互输入。
 *
 *  置 0 只静音这三类**周期性**输出：
 *      · "R:%.2f,P:%.2f,Y:%.2f"   姿态上报（apu_imu.c 每 10 行触发）
 *      · "SONAR:..."              超声上报（apu_sonar.c 真实采集路径）
 *      · 1Hz tick 自检行          （apu_app.c 的 ① 时间基自检）
 *  **不影响**：ACK 回执、PONG、FAULT:SONAR / RECOVER:SONAR / FAULT:NAV
 *             —— 这些是事件型、频次低，而且是验收判据，必须留着。
 *
 *  ⚠️ **上电默认是 1（开启）** —— Linux 侧就靠这些上报工作。
 *     只有手工敲 `@rep:off` 才会变 0，复位后自动恢复 1
 *     （故意不做持久化，免得谁静音完忘了开回来）。
 * =========================================================================== */
void apu_msg_set_periodic_report(int on);
int  apu_msg_periodic_report_enabled(void);

/* ===========================================================================
 * 三、分发与周期
 * =========================================================================== */

/* 端点回调里调这个：解析 + 分发（文本命令转向 apu_app_handle_command） */
int apu_dispatch(struct rpmsg_endpoint *ept, void *data, size_t len);

/* 传输层周期动作（由 main.c 主循环每轮调用；当前为空钩子） */
void apu_periodic(void);

/* ===========================================================================
 * 四、真停机
 * ===========================================================================
 *  DEVICE_CORE_STOP(0x0002) 收到后由传输层调用 apu_request_shutdown()，
 *  main.c 主循环靠 apu_shutdown_requested() 退出。
 *  ⚠️ 这是"关整个裸机核"，和文本命令 "CMD:STOP"（只停电机）是两回事。
 * =========================================================================== */
void apu_request_shutdown(void);
int  apu_shutdown_requested(void);

/* ===========================================================================
 * 五、统计（调试用，可传 NULL 忽略某个）
 * =========================================================================== */
void apu_get_stats(uint32_t *rx, uint32_t *tx, uint32_t *err,
		   uint32_t *hb_rx, uint32_t *hb_tx);

#ifdef __cplusplus
}
#endif

#endif /* APU_MSG_API_H */
