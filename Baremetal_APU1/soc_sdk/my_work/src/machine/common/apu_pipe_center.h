/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_pipe_center.h —— 管道居中控制（10Hz）+ 参数校验
 * ===========================================================================
 *  【来源】传承项目 pipe_center_control.h/c（241 行），**零改动照抄**（纯逻辑）。
 *
 *  【它干什么】
 *  让车在管道里走中间、并且保持航向。每 100ms 算一次：
 *
 *      ① center_error = (左 - 右) / (左 + 右)
 *           左比右远 -> 车偏右 -> error 为正 -> 该往左打
 *      ② PID：kp*error + ki*∫error + kd*d(error)/dt
 *      ③ 加上航向修正：heading_kp * (目标航向 - 当前航向)
 *         减去阻尼项：yaw_rate_damping * 偏航角速度
 *      ④ 得到 turn_output，按 front_yaw_share / rear_yaw_share 分配给前后
 *      ⑤ 后推进器：base_speed ∓ rear_turn，再乘 left_trim / right_trim（补偿左右推力差）
 *         前推进器：只做转向（∓ front_turn），受 front_max_output 限制
 *
 *  ⚠️ 时间单位：调度用 **10ms tick**（PIPE_CENTER_CONTROL_PERIOD_TICKS=10 → 100ms）
 *     所以直接套用 apu_tick_10ms()，常量一个都不用改。
 * ===========================================================================
 */

#ifndef APU_PIPE_CENTER_H
#define APU_PIPE_CENTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 调度周期：10 个 10ms tick = 100ms = 10Hz（与传承项目一致） */
#define PIPE_CENTER_CONTROL_PERIOD_TICKS 10U
#define PIPE_CENTER_CONTROL_DT_S         0.1f

/* ---- 参数（**22** 个，可由 CTRL: 命令下发） ----
 *  ⚠️ 前 15 个 = 传承项目原样的字段与**顺序**（Linux/Web 按位置解析，不许动）。
 *     ★ 2026-10-05：**末尾追加 2 个扩展字段**（下潜两路速度），跟着同一条
 *     CTRL: 帧一起原子下发。追加在末尾是为了：老 Web 发 15 个仍然完全可用。
 *     ★ 2026-10-12：再在末尾追加 **5 个横滚稳定字段**（roll_*），共 22 个，
 *     对齐 Web 侧 eg400_linux_app 的 _motion_params_command()（那里固定发 22 个）。
 *     逐级兼容：15 / 17 / 22 个都能接受，其它个数整帧拒绝。 */
typedef struct {
	float kp;                 /* 居中 P */
	float ki;                 /* 居中 I */
	float kd;                 /* 居中 D */
	float yaw_rate_damping;   /* 偏航角速度阻尼 */
	float integral_limit;     /* 积分限幅 */
	float turn_limit;         /* 转向输出限幅 */
	float base_speed;         /* 巡航基础速度 */
	float min_output;         /* 推进器输出下限（电调死区补偿） */
	float max_output;         /* 推进器输出上限 */
	float heading_kp;         /* 航向保持 P */
	float left_trim;          /* 左侧推力修正 0.50..1.00 */
	float right_trim;         /* 右侧推力修正 0.50..1.00 */
	float front_yaw_share;    /* 转向里前推进器占的比例 */
	float rear_yaw_share;     /* 转向里后推进器占的比例 */
	float front_max_output;   /* 前推进器限幅 */

	/* ---- ↓ 本工程扩展字段（2026-10-05）----
	 *  ⚠️ 管道居中控制算法（apu_pipe_center_update）**自己不使用**这两个字段，
	 *     它们只是搭同一条 CTRL: 帧一起下发/一起校验；真正取用的是 apu_app.c 的
	 *     CMD:DIVE 分支。放在这里是为了"一次 CTRL: 原子生效 + 只有一处校验"。 */
	float dive_left;          /* 下潜时 ch8(下潜-A) 的输出速度 %，±100，0 = 不转（原样下发） */
	float dive_right;         /* 下潜时 ch9(下潜-B) 的输出速度 %，±100，0 = 不转（原样下发） */

	/* ---- ↓ 本工程扩展字段（2026-10-12）横滚稳定（防侧翻）----
	 *  下潜时 ch8 = dive_left + corr、ch9 = dive_right - corr 差动纠平艇体；
	 *  corr 由横滚 PID（roll_kp/ki/kd）按 IMU 横滚角算出，受 roll_limit 限幅，
	 *  roll_sign 决定纠偏方向（实物左右装反时改这里，不用改接线）。
	 *  ⚠️ 增益 kp/ki/kd 全 0 = 功能关闭（上电默认）—— 与 Web 侧 config.py 一致。 */
	float roll_kp;            /* 横滚 P（倾角 -> 差动 %），0~20 */
	float roll_ki;            /* 横滚 I（消恒定倾斜），0~5 */
	float roll_kd;            /* 横滚 D（角速度阻尼），0~5 */
	float roll_limit;         /* 单路修正上限 %，0~100 */
	float roll_sign;          /* 修正方向：负值=反向，其它=正向，-1~1 */
} apu_pipe_center_params_t;

/* ---- 控制器内部状态（复位用 memset 清零） ---- */
typedef struct {
	float integral;
	float last_center_error;
	float last_yaw_deg;
	int   center_history_valid;
	int   yaw_history_valid;
} apu_pipe_center_state_t;

typedef struct {
	float left_mm;
	float right_mm;
	float yaw_deg;
	float target_yaw_deg;
} apu_pipe_center_input_t;

typedef struct {
	float center_error;
	float center_rate;
	float heading_error_deg;
	float yaw_rate_dps;
	float turn_output;
	float left_front;
	float right_front;
	float left_rear;
	float right_rear;
	int   valid;              /* 1 = 本次输出可用 */
} apu_pipe_center_output_t;

typedef struct {
	uint32_t last_tick;
	int      initialized;
} apu_pipe_center_schedule_t;

/* 角度归一化到 -180..+180 */
float apu_pipe_center_wrap180(float angle_deg);

/* 参数合法性校验（CTRL:/PID:/YAWD:/TRIM:/HEADKP:/YAWMIX:/bs= 命令都用它） */
int apu_pipe_center_params_valid(const apu_pipe_center_params_t *params);

/* ---- 横滚稳定（2026-10-12）------------------------------------------------
 *  下潜时按 IMU 横滚角做 PID，算出一个差动修正量 corr（%），
 *  由调用方写成 ch8 = left + corr / ch9 = right - corr。
 *  增益全 0 时返回 0（功能关闭），调用方就等于原样下发 dive 速度。 */

/* 横滚 PID 内部状态（复位用 memset 清零 / 或调 apu_roll_stab_reset） */
typedef struct {
	float integral;           /* 积分累积 */
	float last_roll_deg;      /* 上次横滚角（算 D 项） */
	int   history_valid;      /* 0 = 还没有上一拍，D 项先不参与 */
	int   centered;           /* 0 = 还没在"关闭"状态下清零过积分 */
} apu_roll_stab_state_t;

void apu_roll_stab_reset(apu_roll_stab_state_t *state);

/* 算一拍修正量（返回 % ，已按 roll_limit 限幅）。
 *  roll_deg        : 当前横滚角（度，正右倾）
 *  target_roll_deg : 横滚目标角（度，正右倾）。**与航向纠正同构** ——
 *                    按「重新标定」时把当时的横滚角存进来当基准；
 *                    传 0 = 要求水平（不标定时的默认）。
 *  params          : 参数（用 roll_kp/ki/kd/limit/sign）
 *  state           : 内部状态
 *  dt_s            : 控制周期（秒）
 *  enabled         : 0 = 全关，直接返回 0 并清积分
 *  返回值          : 差动修正量，已限幅到 ±roll_limit */
float apu_roll_stab_update(apu_roll_stab_state_t *state,
			   const apu_pipe_center_params_t *params,
			   float roll_deg, float target_roll_deg,
			   float dt_s, int enabled);

/* 复位控制器状态 */
void apu_pipe_center_reset(apu_pipe_center_state_t *state);

/* 算一次输出 */
apu_pipe_center_output_t apu_pipe_center_update(
	apu_pipe_center_state_t *state,
	const apu_pipe_center_params_t *params,
	const apu_pipe_center_input_t *input,
	float dt_s);

/* ---- 10Hz 调度 ---- */

/* 复位调度器。run_immediately=1 -> 下一次 IsDue 立刻返回 1 */
void apu_pipe_center_schedule_reset(apu_pipe_center_schedule_t *schedule,
				    uint32_t now_tick,
				    int run_immediately);

/* 到点了吗？（内部会把 last_tick 对齐到整周期，避免累积漂移） */
int apu_pipe_center_schedule_is_due(apu_pipe_center_schedule_t *schedule,
				    uint32_t now_tick);

#ifdef __cplusplus
}
#endif

#endif /* APU_PIPE_CENTER_H */
