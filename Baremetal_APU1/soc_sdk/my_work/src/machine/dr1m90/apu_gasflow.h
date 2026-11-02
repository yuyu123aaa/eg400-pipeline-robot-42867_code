/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_gasflow.h —— 气路控制（电磁阀 + 吸气泵）
 * ===========================================================================
 *  【硬件】
 *      电磁阀（进气阀）  PL_GPIO bit3  →  全局引脚号 57  →  CEP pin12 (V7)
 *      吸气泵（隔膜泵）  PL_GPIO bit4  →  全局引脚号 58  →  CEP pin10 (W6)
 *      两者都是**继电器、高电平有效**：输出 1 = 吸合（阀开 / 泵转），0 = 断开
 *
 *      ⚠️ 引脚号换算：EMIO PL_GPIO bit N 的 AlGpio 全局号 = 54 + N
 *         （54=led1 55=led2 56=rs485_de 57=gas_valve 58=gas_pump）
 *         依据 apu_led.h:15-23 与 al_gpio_hal.h:88-91 的 bank 边界
 *
 *  【★★★ 时序铁律 —— 顺序不可颠倒，这是本模块存在的唯一理由 ★★★】
 *
 *      开：  阀先开  →  等 2 秒  →  泵后开
 *      关：  泵先关  →  等 0.5 秒 →  阀后关
 *
 *      为什么"必须先开阀、等 2 秒才能开泵"：
 *          阀还没完全打开就启动泵 → 管路负压冲击 → 可能损坏传感器膜片
 *          或把液体吸进气路。
 *      为什么"必须先关泵、再关阀"：
 *          阀先关而泵还在转 → 泵端憋压 → 同样损坏泵和管路。
 *
 *      ⇒ 代码里用**状态机**保证顺序，绝不出现"同时开关"或"颠倒"。
 *      ⇒ 每个阶段都有**安全超时上限**，防止状态机卡死时泵一直转。
 *
 *  【完整时序】
 *      t0        开阀(1), 泵保持关(0)
 *      t0+2000   开泵(1)
 *      t0+8000   关泵(0)          ← 吸气 6 秒
 *      t0+8500   关阀(0)          ← 泵停后延迟 0.5 秒
 *      总时长 8.5 秒
 *
 *  【上电安全】
 *      初始化时先写 0（阀关、泵关）。PS 的 GPIO 输出寄存器复位值本来就是 0，
 *      这里再显式写一次，防御热复位残留。硬件上 N-MOS 栅极还有 10k 下拉，
 *      软件默认态 + 硬件下拉 = 双保险。
 * ===========================================================================
 */

#ifndef APU_GASFLOW_H
#define APU_GASFLOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 *  GPIO 引脚号（EMIO PL_GPIO bit3/bit4）
 * --------------------------------------------------------------------------- */
#define APU_GASFLOW_VALVE_PIN   57U   /* 电磁阀 V7（CEP pin12）*/
#define APU_GASFLOW_PUMP_PIN    58U   /* 吸气泵 W6（CEP pin10）*/

/* 电平语义：继电器高电平有效 */
#define APU_GASFLOW_ON          1U
#define APU_GASFLOW_OFF         0U

/* ---------------------------------------------------------------------------
 *  ★ 时序参数（想调整只改这里，改完重编即可）
 * --------------------------------------------------------------------------- */
#define APU_GASFLOW_VALVE_SETTLE_MS   2000U  /* 开阀后稳定等待，再开泵（铁律） */
#define APU_GASFLOW_PUMP_RUN_MS       6000U  /* 吸气时长 */
#define APU_GASFLOW_PUMP_OFF_DELAY_MS  500U  /* 关泵后延迟，再关阀（铁律） */

/* 各阶段安全超时：状态机卡死时强制停机，绝不让泵无限转下去。
 * 取值比计划时长留足余量（主循环约 1 秒一圈）。 */
#define APU_GASFLOW_VALVE_TIMEOUT_MS  8000U   /* 开阀→开泵 阶段上限 */
#define APU_GASFLOW_PUMP_TIMEOUT_MS  30000U   /* 吸气阶段上限（6s 计划 + 余量）*/
#define APU_GASFLOW_CLOSE_TIMEOUT_MS  5000U   /* 关泵→关阀 阶段上限 */

/* ---------------------------------------------------------------------------
 *  状态机状态
 * --------------------------------------------------------------------------- */
typedef enum {
	APU_GASFLOW_IDLE = 0,      /* 空闲：阀关、泵关 */
	APU_GASFLOW_VALVE_OPEN,    /* 已开阀，等稳定（泵仍关）*/
	APU_GASFLOW_PUMPING,       /* 阀开 + 泵开，吸气中 */
	APU_GASFLOW_CLOSING        /* 已关泵，等延迟（阀仍开）*/
} apu_gasflow_state_t;

/* ---------------------------------------------------------------------------
 *  接口
 * --------------------------------------------------------------------------- */

/* 初始化：点灯之前先保证阀/泵都是关的。
 * 返回 0 = 成功；-1 = GPIO 控制器起不来（此时**不会有任何输出动作**）*/
int  apu_gasflow_init(void);

/* 周期任务：挂主循环，每圈调一次。
 * ⚠️ **非阻塞** —— 内部只看时间推进状态机，不做任何等待/延时。
 *    这一点是刻意的：主循环还要跑 rpmsg，绝不能在这里阻塞。
 *    时序精度受主循环周期（约 1 秒）限制，见 .c 顶部说明。 */
void apu_gasflow_poll(void);

/* ★ 启动一次吸气流程（阀→2s→泵→6s→泵关→0.5s→阀关）。
 * 返回 0 = 已启动；-1 = GPIO 未就绪；1 = 已经在流程中（忽略本次，不重入）*/
int  apu_gasflow_start(void);

/* 立即全部关闭（阀关、泵关、状态机回空闲）。
 * 用于：收到停止命令、关机、异常兜底。
 * ⚠️ 这是**急停**语义，不遵守"泵先阀后"的延迟；只在真正需要立刻断气时用。 */
void apu_gasflow_force_off(const char *reason);

/* 状态查询 */
apu_gasflow_state_t apu_gasflow_state(void);
const char *apu_gasflow_state_name(apu_gasflow_state_t st);
int  apu_gasflow_busy(void);          /* 1 = 流程进行中 */
int  apu_gasflow_valve_on(void);      /* 当前阀输出电平 */
int  apu_gasflow_pump_on(void);       /* 当前泵输出电平 */
uint32_t apu_gasflow_elapsed_ms(void);/* 本轮已运行时长 */

/* 打印当前状态一行（给控制台 @gas 用）*/
void apu_gasflow_dump(const char *tag);

/* ★ 远程/控制台命令处理：识别 "CMD:GAS_SAMPLE_START" / "CMD:GAS_STOP"
 *   （以及别名 CMD:GAS_START / CMD:GAS_SAMPLE_STOP）
 * 返回 1 = 本条命令归我处理（已处理或已拒绝）；0 = 不是我的命令。 */
int  apu_gasflow_handle_command(const char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* APU_GASFLOW_H */
