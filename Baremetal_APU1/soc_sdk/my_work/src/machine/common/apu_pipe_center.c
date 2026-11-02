/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_pipe_center.c —— 管道居中控制实现
 * ===========================================================================
 *  逐行对照传承项目 pipe_center_control.c（241 行）写成，**逻辑零改动**。
 *  只做了改名（PipeCenter_* -> apu_pipe_center_*）和类型名调整。
 * ===========================================================================
 */

#include <math.h>
#include <string.h>

#include "apu_pipe_center.h"

static float clampf(float value, float minimum, float maximum)
{
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return value;
}

float apu_pipe_center_wrap180(float angle_deg)
{
	if (!isfinite(angle_deg))
		return angle_deg;

	while (angle_deg > 180.0f)
		angle_deg -= 360.0f;
	while (angle_deg < -180.0f)
		angle_deg += 360.0f;
	return angle_deg;
}

int apu_pipe_center_params_valid(const apu_pipe_center_params_t *params)
{
	/* 与传承项目逐条一致 —— 少一条都可能让非法参数混进来 */
	return params != 0 &&
	       isfinite(params->kp) && params->kp >= 0.0f && params->kp <= 100.0f &&
	       isfinite(params->ki) && params->ki >= 0.0f && params->ki <= 20.0f &&
	       isfinite(params->kd) && params->kd >= 0.0f && params->kd <= 20.0f &&
	       isfinite(params->yaw_rate_damping) &&
	       params->yaw_rate_damping >= 0.0f && params->yaw_rate_damping <= 2.0f &&
	       isfinite(params->integral_limit) && params->integral_limit >= 0.0f &&
	       isfinite(params->turn_limit) && params->turn_limit > 0.0f &&
	       isfinite(params->base_speed) && params->base_speed >= 0.0f &&
	       isfinite(params->min_output) &&
	       isfinite(params->max_output) &&
	       isfinite(params->heading_kp) &&
	       params->heading_kp >= 0.0f && params->heading_kp <= 2.0f &&
	       isfinite(params->left_trim) &&
	       params->left_trim >= 0.50f && params->left_trim <= 1.00f &&
	       isfinite(params->right_trim) &&
	       params->right_trim >= 0.50f && params->right_trim <= 1.00f &&
	       isfinite(params->front_yaw_share) &&
	       params->front_yaw_share >= 0.0f && params->front_yaw_share <= 1.0f &&
	       isfinite(params->rear_yaw_share) &&
	       params->rear_yaw_share >= 0.0f && params->rear_yaw_share <= 1.0f &&
	       isfinite(params->front_max_output) &&
	       params->front_max_output >= 0.0f && params->front_max_output <= 100.0f &&
	       /* ★ 2026-10-05 扩展字段：下潜两路速度。
	       *  ±100，**0 也算合法**（= 不转）—— 默认值是 0，若把 0 判非法，
	       *  开机那次默认参数自检就会失败，所有 CTRL:/PID: 命令会被整体拒绝。 */
	       isfinite(params->dive_left) &&
	       params->dive_left >= -100.0f && params->dive_left <= 100.0f &&
	       isfinite(params->dive_right) &&
	       params->dive_right >= -100.0f && params->dive_right <= 100.0f &&
	       /* ★ 2026-10-12 扩展字段：横滚稳定五参数。
	       *  范围与 Web 侧 config.py / main.py:ROLL_PARAM_RANGES 严格一致：
	       *      roll_kp 0~20、roll_ki 0~5、roll_kd 0~5、
	       *      roll_limit 0~100、roll_sign -1~1。
	       *  **全 0 也算合法**（默认值就是全 0 = 功能关闭）；若判非法，
	       *  开机那次默认参数自检会失败，所有 CTRL:/PID: 命令被整体拒绝。 */
	       isfinite(params->roll_kp) &&
	       params->roll_kp >= 0.0f && params->roll_kp <= 20.0f &&
	       isfinite(params->roll_ki) &&
	       params->roll_ki >= 0.0f && params->roll_ki <= 5.0f &&
	       isfinite(params->roll_kd) &&
	       params->roll_kd >= 0.0f && params->roll_kd <= 5.0f &&
	       isfinite(params->roll_limit) &&
	       params->roll_limit >= 0.0f && params->roll_limit <= 100.0f &&
	       isfinite(params->roll_sign) &&
	       params->roll_sign >= -1.0f && params->roll_sign <= 1.0f &&
	       /* 前后分配比例必须加起来等于 1 */
	       fabsf(params->front_yaw_share + params->rear_yaw_share - 1.0f) <= 0.001f &&
	       /* 速度包络必须自洽 */
	       params->min_output <= params->base_speed &&
	       params->base_speed <= params->max_output &&
	       params->turn_limit <= params->max_output - params->min_output;
}

void apu_pipe_center_reset(apu_pipe_center_state_t *state)
{
	if (state != 0)
		memset(state, 0, sizeof(*state));
}

/* ---------------------------------------------------------------------------
 * ★ 2026-10-12 新增：横滚稳定 PID（下潜防侧翻）
 * ---------------------------------------------------------------------------
 *  目标：下潜时把艇体横滚角拉回 0（水平），靠 ch8/ch9 两路差动实现：
 *        ch8 = dive_left  + corr
 *        ch9 = dive_right - corr
 *  corr 由横滚 PID 按 IMU 横滚角算出，符号约定：
 *        roll_deg > 0 = 右倾 -> 需要抬高右侧 -> 右侧（ch9）多出力，左侧少出力。
 *  本函数只负责"算出 corr"，限幅、下发方向由调用方决定。
 *
 *  ⚠️ 正负号与实物左右安装强相关：靠 roll_sign 现场翻向，不改接线。
 *  ⚠️ 全 0 增益 = 关闭：直接返回 0 并把积分清零，保证"关了就是真关"。
 *  ⚠️ D 项用"角速度"（本拍角 - 上拍角）/dt；第一拍没有历史，D 项不参与，
 *     避免上电瞬间一个巨大的假脉冲。
 * --------------------------------------------------------------------------- */
void apu_roll_stab_reset(apu_roll_stab_state_t *state)
{
	if (state != 0)
		memset(state, 0, sizeof(*state));
}

float apu_roll_stab_update(apu_roll_stab_state_t *state,
			   const apu_pipe_center_params_t *params,
			   float roll_deg, float target_roll_deg,
			   float dt_s, int enabled)
{
	float p_term, i_term, d_term, corr, limit, rate, error_deg;
	int   gains_zero;

	if (state == 0 || params == 0)
		return 0.0f;

	/* 增益全 0（或调用方显式关闭）= 功能关闭：清状态、返回 0。
	 * 这里用显式"全 0"判定而不是只看 enabled，是为了让"Web 把三个增益都调回 0"
	 * 也能立刻停止修正，而不是残留一个积分量。 */
	gains_zero = (params->roll_kp == 0.0f &&
		      params->roll_ki == 0.0f &&
		      params->roll_kd == 0.0f);

	if (enabled == 0 || gains_zero) {
		state->integral      = 0.0f;
		state->last_roll_deg = roll_deg;
		state->history_valid = 0;
		state->centered      = 1;
		return 0.0f;
	}

	if (!isfinite(roll_deg) || !isfinite(dt_s) || dt_s <= 0.0f) {
		/* 姿态不可信：不修正、不清状态（等数据恢复后接着来），
		 * 但把历史标记打掉，避免数据中断期间 D 项算出一个假的巨大值。 */
		state->history_valid = 0;
		return 0.0f;
	}

	/* 目标角不可信时按 0（水平）处理 —— 与航向那边"target 未标定时为 0"同一口径，
	 * 保证最坏情况下退回"把艇掰水平"，而不是因为一个 NaN 把控制量算成 NaN。 */
	if (!isfinite(target_roll_deg))
		target_roll_deg = 0.0f;

	/* 从"关闭"切到"开启"的第一拍：清一次积分，避免上次的残留 */
	if (state->centered) {
		state->integral      = 0.0f;
		state->history_valid = 0;
		state->centered      = 0;
	}

	/* ★ 2026-10-12：与航向纠正完全同构 —— 控制量吃的是**误差**（当前 − 目标），
	 *  而不是"当前角"。三项必须全部用 error，不能只改 P：
	 *  · I 项若还攒 roll_deg，标定后目标非 0 时会冲着一个错的零点积分，越纠越歪；
	 *  · D 项保持用"当前角差分"（这就是角速度本身），它反映的是晃动快慢，
	 *    与目标在哪无关 —— 所以不动。 */
	error_deg = roll_deg - target_roll_deg;

	/* ---- P：偏离目标的角度成比例 ---- */
	p_term = params->roll_kp * error_deg;

	/* ---- I：累积误差，消掉恒定偏差（限幅 ±roll_limit 防积分饱和） ---- */
	limit = params->roll_limit;
	if (limit < 0.0f)
		limit = 0.0f;
	if (limit > 100.0f)
		limit = 100.0f;

	state->integral += error_deg * dt_s;
	if (state->integral > limit)
		state->integral = limit;
	if (state->integral < -limit)
		state->integral = -limit;
	i_term = params->roll_ki * state->integral;

	/* ---- D：角速度阻尼（用横滚角差分近似 —— 与目标无关） ---- */
	d_term = 0.0f;
	if (state->history_valid) {
		rate   = (roll_deg - state->last_roll_deg) / dt_s;
		d_term = params->roll_kd * rate;
	}
	state->last_roll_deg = roll_deg;
	state->history_valid = 1;

	/* ---- 合成 + 限幅 + 方向 ---- */
	corr = p_term + i_term + d_term;

	/* roll_sign：负值 = 反向。用 (sign < 0 ? -1 : +1) 而不是直接乘，
	 * 是为了让 sign=0 也当"正向"，与 Web 侧注释 "其它=正向" 一致。 */
	if (params->roll_sign < 0.0f)
		corr = -corr;

	if (corr > limit)
		corr = limit;
	if (corr < -limit)
		corr = -limit;

	return corr;
}

apu_pipe_center_output_t apu_pipe_center_update(
	apu_pipe_center_state_t *state,
	const apu_pipe_center_params_t *params,
	const apu_pipe_center_input_t *input,
	float dt_s)
{
	apu_pipe_center_output_t output;
	float distance_sum;
	float center_output;
	float front_turn;
	float rear_turn;

	memset(&output, 0, sizeof(output));

	/* ---- 入口检查（任何一项不合法就返回 valid=0，上层会当故障处理） ---- */
	if (state == 0 || input == 0 ||
	    !apu_pipe_center_params_valid(params) ||
	    !isfinite(input->left_mm) ||
	    !isfinite(input->right_mm) ||
	    !isfinite(input->yaw_deg) ||
	    !isfinite(input->target_yaw_deg) ||
	    input->left_mm <= 0.0f ||
	    input->right_mm <= 0.0f ||
	    !isfinite(dt_s) || dt_s <= 0.0f)
		return output;

	distance_sum = input->left_mm + input->right_mm;
	if (!isfinite(distance_sum) || distance_sum <= 0.0f)
		return output;

	/* ---- ① 居中误差：正 = 偏右（左边更远） ---- */
	output.center_error = (input->left_mm - input->right_mm) / distance_sum;

	if (state->center_history_valid)
		output.center_rate =
			(output.center_error - state->last_center_error) / dt_s;
	state->last_center_error = output.center_error;
	state->center_history_valid = 1;

	/* ---- ② 偏航角速度（用 Wrap180 防跨 ±180 跳变） ---- */
	if (state->yaw_history_valid)
		output.yaw_rate_dps =
			apu_pipe_center_wrap180(input->yaw_deg - state->last_yaw_deg) / dt_s;
	state->last_yaw_deg = input->yaw_deg;
	state->yaw_history_valid = 1;

	/* ---- ③ 积分（带限幅，防止积分饱和） ---- */
	state->integral += output.center_error * dt_s;
	state->integral = clampf(state->integral,
				 -params->integral_limit, params->integral_limit);

	center_output = params->kp * output.center_error +
			params->ki * state->integral +
			params->kd * output.center_rate;

	/* ---- ④ 航向保持 + 阻尼 ---- */
	output.heading_error_deg =
		apu_pipe_center_wrap180(input->target_yaw_deg - input->yaw_deg);

	output.turn_output = center_output +
			     params->heading_kp * output.heading_error_deg -
			     params->yaw_rate_damping * output.yaw_rate_dps;
	output.turn_output = clampf(output.turn_output,
				    -params->turn_limit, params->turn_limit);

	/* ---- ⑤ 前后分配 ---- */
	front_turn = output.turn_output * params->front_yaw_share;
	rear_turn = output.turn_output * params->rear_yaw_share;

	/* 前推进器：只负责转向（差动），带限幅 */
	output.left_front = clampf((-front_turn) * 2,
				   -params->front_max_output, params->front_max_output);
	output.right_front = clampf(front_turn * 2,
				    -params->front_max_output, params->front_max_output);

	/* 后推进器：基础速度 ∓ 转向量，再乘左右 trim（补推力不一致） */
	output.left_rear = clampf((params->base_speed - rear_turn) * params->left_trim,
				  params->min_output, params->max_output);
	output.right_rear = clampf((params->base_speed + rear_turn) * params->right_trim,
				   params->min_output, params->max_output);

	output.valid = 1;
	return output;
}

/* ---------------------------------------------------------------------------
 * 10Hz 调度
 * --------------------------------------------------------------------------- */
void apu_pipe_center_schedule_reset(apu_pipe_center_schedule_t *schedule,
				    uint32_t now_tick,
				    int run_immediately)
{
	if (schedule == 0)
		return;

	/* 想"立刻跑一次"就把 last_tick 往前推一个周期 */
	schedule->last_tick = run_immediately
				      ? (now_tick - PIPE_CENTER_CONTROL_PERIOD_TICKS)
				      : now_tick;
	schedule->initialized = 1;
}

int apu_pipe_center_schedule_is_due(apu_pipe_center_schedule_t *schedule,
				    uint32_t now_tick)
{
	uint32_t elapsed;
	uint32_t elapsed_periods;

	if (schedule == 0)
		return 0;

	if (!schedule->initialized) {
		apu_pipe_center_schedule_reset(schedule, now_tick, 0);
		return 0;
	}

	elapsed = now_tick - schedule->last_tick;
	if (elapsed < PIPE_CENTER_CONTROL_PERIOD_TICKS)
		return 0;

	/* ★ 对齐到整周期，而不是直接 last_tick = now_tick
	 *   —— 这样即使某轮晚了几毫秒，长期也不会累积漂移（原项目就是这么做的） */
	elapsed_periods = elapsed / PIPE_CENTER_CONTROL_PERIOD_TICKS;
	schedule->last_tick += elapsed_periods * PIPE_CENTER_CONTROL_PERIOD_TICKS;
	return 1;
}
