/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_pca9685.h —— APU1 裸机侧推进器 PWM（PCA9685 on I2C0）
 * ===========================================================================
 *  【硬件】
 *      PCA9685 挂在 **I2C0**（apu_i2c 的 BUS0），从机地址 **0x40**
 *      -> 和超声波共用 0x40 地址，但**在另一条总线上**，天然不冲突
 *
 *  【通道分配】
 *      代码通道 4 = 左后    5 = 右后    6 = 左前    7 = 右前
 *          （来自传承项目 pca9685.c:147-159 的标定表。注释里说板载丝印是 5/6/7/8，
 *            差 1 —— 那**只是注释**，代码里没有任何依据，实际接线以实测为准）
 *      ★ 2026-10-05 新增：8 = 下潜-A   9 = 下潜-B（同型号电调，单独命令控制）
 *          寄存器偏移 = 0x06 + 4*ch  ->  ch8: 0x26，ch9: 0x2A
 *
 *  ★★ 2026-10-23：**逻辑通道号 与 PCA9685 物理通道号 从此是两张表** ★★
 *      原因：板上 PCA9685 的**物理 ch4 输出异常**（PWM 乱来），开环测不出是
 *            芯片该路损坏还是外围走线问题。为了不改上层任何代码，把
 *            **逻辑 ch4（左后）改到物理 ch10**，其余五路不变：
 *
 *                逻辑(API/上层一直用)   ->   物理(PCA9685 寄存器)
 *                4 = 左后                     10   ← 本次改接
 *                5 = 右后                     5
 *                6 = 左前                     6
 *                7 = 右前                     7
 *                8 = 下潜-A                   8
 *                9 = 下潜-B                   9
 *
 *      ⚠️ 上层（apu_app.c / apu_manual.c / apu_testlink.c）继续用 4..9，
 *         **不要**因为硬件改接去改它们 —— 映射只在 apu_pca9685.c 的
 *         g_esc_phys[] 这一张表里维护。
 *      ⚠️ 任何需要"算寄存器地址"的地方（如 @esc:regs 自检）**必须**先用
 *         apu_esc_get_phys_channel() 换算，不能拿逻辑号直接算 0x06+4*ch。
 *      ⚠️ 废弃的物理 ch4 会在初始化时被显式写成"无脉冲"，避免它保留上电
 *         残留占空比继续乱输出。
 *
 *  【电调标定值】每个通道 {reverse_max, neutral, forward_max}，单位 1/4096
 *      neutral ≈ 330, forward ≈ 433, reverse ≈ 228
 *      -> 对应脉宽：neutral 1.5ms、+100% ≈ 2.1ms、-100% ≈ 1.1ms（50Hz）
 *
 *  ⚠️ 标定表**逐通道微调过**（228/330/433 vs 226/328/431），别"统一"成一个值。
 * ===========================================================================
 */

#ifndef APU_PCA9685_H
#define APU_PCA9685_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 通道号（4..7 与传承项目一致；8/9 是 2026-10-05 新增的下潜两路） */
#define APU_ESC_LEFT_REAR     4
#define APU_ESC_RIGHT_REAR    5
#define APU_ESC_LEFT_FRONT    6
#define APU_ESC_RIGHT_FRONT   7
#define APU_ESC_DIVE_A        8     /* 下潜第 1 路 */
#define APU_ESC_DIVE_B        9     /* 下潜第 2 路 */

/* 受控通道范围：ch4..ch9 连续 6 路。
 *  ⚠️ 标定表 / off_tick 的下标一律是 `channel - APU_ESC_CH_FIRST`，
 *     **不是** channel 本身；这 6 个编号必须连续（apu_pca9685.c 有编译期自检）。 */
#define APU_ESC_CH_FIRST      APU_ESC_LEFT_REAR    /* 4 */
#define APU_ESC_CH_LAST       APU_ESC_DIVE_B       /* 9 */
#define APU_ESC_CH_COUNT      (APU_ESC_CH_LAST - APU_ESC_CH_FIRST + 1)   /* 6 */

/* ---------------------------------------------------------------------------
 * 初始化 / 安全
 * --------------------------------------------------------------------------- */

/* 初始化 PCA9685（50Hz）+ 电调解锁时序（给 0% 并延时约 3 秒）。
 *
 * ⚠️ 上电安全：**先上裸机电、再上电调电**，上电瞬间电机不得有动作。
 *
 * 返回：0 成功；<0 失败（I2C 不通或 PCA9685 不应答）。 */
int apu_pca9685_init(void);

/* ★ 统一停车路径：所有"停"的场景都调它（安全闸门 / CMD:STOP / 心跳超时 / 关核）
 * **6 路**全部回到 neutral（含下潜 ch8/ch9），并把下潜状态清零。 */
void apu_esc_stop_all(void);

/* ★ 2026-10-05 新增：只停**四路水平推进器**（ch4~ch7），**不动**下潜两路（ch8/ch9）。
 *  用途：还没点「向前」时（!motor_enabled），主循环每轮都要保证水平推进器不动，
 *        但下潜是独立通道，必须能在"系统已启动、还没开始走"的状态下单独工作。
 *  🔴 真正的全停 / 急停**必须**用 apu_esc_stop_all()（6 路）——
 *     别拿本函数当急停，它**不会**停下潜那两路。 */
void apu_esc_stop_drive(void);

/* ---------------------------------------------------------------------------
 * 调速
 * --------------------------------------------------------------------------- */

/* 设置一个通道的速度，speed_percent ∈ [-100, +100]（0 = neutral）
 * channel 只接受 **4~9**（4/5/6/7 水平 + 8/9 下潜），其它值直接忽略。 */
void apu_esc_set_speed(uint8_t channel, float speed_percent);

/* 一次设置四个通道（-100..+100） */
void apu_esc_set_all(float left_front, float right_front,
		     float left_rear, float right_rear);

/* 读回某个通道当前的 off_tick（调试/验证脉宽用） */
uint16_t apu_esc_get_off_tick(uint8_t channel);

/* ★ 2026-10-12：查某通道是否被"输出取反"（电机三相线接反的软件补偿）。
 *  返回 1 = 已取反，0 = 正常（含越界）。 */
int apu_esc_get_invert(uint8_t channel);

/* ★ 2026-10-23：把**逻辑通道号**（4..9）换算成 **PCA9685 物理通道号**（0..15）。
 *
 *  用途：任何要直接访问 PCA9685 寄存器（0x06 + 4*phys）的代码都必须先过这里，
 *        否则会踩"逻辑号 != 物理号"的坑（左后已从物理 4 改到物理 10）。
 *
 *  入参越界（不在 4..9）返回 0xFF 表示无效。 */
uint8_t apu_esc_get_phys_channel(uint8_t channel);

/* ---------------------------------------------------------------------------
 * ★ 2026-10-05 新增：下潜两路（ch8 / ch9）
 * ---------------------------------------------------------------------------
 *  【分工】开关由命令表达（CMD:DIVE / CMD:DIVE:STOP），**速度是参数**
 *          （CTRL: 帧末两位 -> apu_pipe_center_params_t 的 dive_left/dive_right）。
 *
 *      on != 0 ：把 (left_pct, right_pct) **原样**写到 ch8 / ch9，并记住"下潜在开"
 *      on == 0 ：两路回 neutral，并清掉"下潜在开"
 *
 *  ⚠️ **没有"上浮"、也不做任何取反**：本艇正浮力，把这两路速度**减小/停掉**，
 *     船自然就上浮。所以参数就是"下潜时这两路各自出多少力"，原样下发。
 *     哪一路实物装反/接线反，把**它的参数**写成负值即可，不用改接线。
 *
 *  标度与 SPEED: / apu_esc_set_speed() 完全一样（±100 = 电调标定表满量程）。
 *
 *  ⚠️ 与四路水平推进器**互不干扰**：10Hz 自动居中（apu_esc_set_all）和手动混控
 *     都只写 ch4~7，这里下发的值会一直保持；直到下一条 CMD:DIVE / CMD:DIVE:STOP，
 *     或者任何一条"停"走到 apu_esc_stop_all()（6 路全部回 neutral 并把状态清零）。
 *  ⚠️ 内部先限幅再存，所以 get 回来的 == 真正写到芯片上的值。
 *  ⚠️ 写一次就够：PCA9685 自己会持续输出，不需要周期刷新。 */
void apu_esc_set_dive(int on, float left_pct, float right_pct);

/* 读回当前下潜状态（三个出参都可传 NULL）。
 * 任何"停"之后：on=0，left_pct=right_pct=0。 */
void apu_esc_get_dive(int *on, float *left_pct, float *right_pct);

/* ⚪ 2026-09-25：原 `apu_esc_set_raw_tick()` 声明已删除 ——
 *  它是 P7 台架期"绕过标定表直写 off_tick"的脉宽扫描工具，实现已一并删除，
 *  与传承工程无关（传承只有 ESC_SetSpeed() 一条路）。
 *  要看芯片里真实寄存器内容请用控制台 @esc:regs。 */

/* ---------------------------------------------------------------------------
 * 底层寄存器访问（给自检和读写一致性测试用）
 * --------------------------------------------------------------------------- */

/* 写寄存器 = [reg][data...] */
int apu_pca9685_write_reg(uint8_t reg, uint8_t val);
/* 读寄存器（复合读） */
int apu_pca9685_read_reg(uint8_t reg, uint8_t *val);

/* 关闭输出（把所有通道设成 OFF），用于 deinit */
void apu_pca9685_deinit(void);

/* 统计 */
uint32_t apu_pca9685_write_count(void);
uint32_t apu_pca9685_err_count(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_PCA9685_H */
