/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_led.h —— APU1 裸机侧照明灯驱动（PL GPIO / EMIO）
 * ===========================================================================
 *  【硬件真相 —— 和旧文档不一样，务必看这里】
 *
 *  平台配置实测（soc_base\platform\inc\soc_plat.h）：
 *      #define GPIOPL_ENABLE        <- 开
 *      // #define GPIOPS_ENABLE     <- 已注释！
 *
 *  意思是：**GPIO 走 PL（EMIO）**，不是 PS 的 MIO。
 *  所以引脚号**不是**官方例程里的 PS_LED=51，而是：
 *
 *      led1 (CEP pin9  / 球号 V6) -> PL GPIO 全局号 54 = bank2 bit0
 *      led2 (CEP pin11 / 球号 U7) -> PL GPIO 全局号 55 = bank2 bit1
 *
 *  推算依据（实读 SDK）：
 *      al_gpio_hal.h:88-91   bank 边界 31 / 53 / 85 / 117
 *                            => MIO  0..53  = bank0+1
 *                               EMIO 54..   = bank2 起（PL）
 *      al_gpio_hal.c:139-154 GetBankPin()：Pin - MAX_PIN_NUMBER_IN_BANK_1 - 1
 *                            => 54 -> bank2, bit0 ;  55 -> bank2, bit1
 *      官方例程 gpio_ctl_led.c 里 `#define PL_LED 54` 也印证了 54 = PL bit0
 *
 *  【电气】
 *  高电平点亮：PS 侧写 1 -> pl_gpio_out=1 且 oe=1 -> 引脚输出 3.3V -> 灯亮
 *  （顶层 system_top.v 的写法见文件内注释；若实测"写 1 反而 0V"，
 *    说明 oe 或 LED 极性反了，改 system_top.v:114/116）
 *
 *  ⚠️ 引脚驱动只有 8mA，**只够点普通 LED**。
 *     要点"真照明灯"必须经 MOSFET 驱动板，且灯单独供电。
 * ===========================================================================
 */

#ifndef APU_LED_H
#define APU_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 引脚号（★ 别再写 51 了）
 * ---------------------------------------------------------------------------
 *  【物理接线 → 引脚号（这一层是 RTL 定的，不要改）】
 *      PL GPIO bit0 = 全局号 54 → CEP pin9  → 球号 V6
 *      PL GPIO bit1 = 全局号 55 → CEP pin11 → 球号 U7
 *
 *  ★★ 2026-10-17 实测修正：左右灯**接反了** ★★
 *      现象：Web 上点"开左灯"，实际亮的是 **V6** 那盏。
 *      ⇒ 真实接线是：  V6 (pin54 / bit0) = **右灯**
 *                      U7 (pin55 / bit1) = **左灯**
 *
 *      原代码假设 V6=左、U7=右，所以把两个宏的**数值对调**即可，
 *      这样下面所有语义一次性同时正确（不用分散改多处）：
 *          apu_led_left_on()  -> 55 (U7) = 左灯 ✔
 *          apu_led_right_on() -> 54 (V6) = 右灯 ✔
 *          apu_led_read_back(1) -> 55 = 左灯 ✔（"1" 始终代表左灯）
 *          apu_app 的 g_led_state bit0（灯1） = 左灯 ✔
 *
 *  ⚠️ 只改这里。**不要**去改 CEP 引脚约束（io.adc）——
 *     那要重新综合位流，而本问题是"逻辑编号与物理灯的对应"，
 *     在软件层交换即可，RTL / BOOT.bin 都不用动。
 * --------------------------------------------------------------------------- */
#define APU_LED1_PIN    55U   /* 左灯 = PL GPIO bit1 -> CEP pin11 -> U7 */
#define APU_LED2_PIN    54U   /* 右灯 = PL GPIO bit0 -> CEP pin9  -> V6 */

/* ---------------------------------------------------------------------------
 * 接口（命名与传承项目 Led.h 对应，便于对照）
 * --------------------------------------------------------------------------- */

/* 初始化：配置两盏灯为输出、先熄灭，然后**各闪 3 次**做肉眼自检。
 * 返回：0 成功；-1 GPIO 控制器初始化失败。 */
int  apu_led_init(void);

/* 传承项目对应关系（★ 2026-10-17 已按实测接线修正）：
 *      LED_LEFT_ON/OFF  -> 左灯（pin 55 / U7 / CEP pin11）
 *      LED_RIGHT_ON/OFF -> 右灯（pin 54 / V6 / CEP pin9） */
void apu_led_left_on(void);
void apu_led_left_off(void);
void apu_led_right_on(void);
void apu_led_right_off(void);

/* 统一熄灭（所有"停"的场景都调它，别各写各的） */
void apu_led_all_off(void);

/* 读回引脚电平（1/0）—— 用来验证"写进去的到底有没有出来" */
int  apu_led_read_back(uint32_t which);   /* which: 1=灯1, 2=灯2 */

/* 最近一次的软件状态：bit0=灯1, bit1=灯2（1=开） */
uint8_t apu_led_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_LED_H */
