/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_led.c —— APU1 裸机侧照明灯驱动实现（PL GPIO / EMIO）
 * ===========================================================================
 *  【SDK 用法要点】（实读 al_gpio_hal.c 确认）
 *
 *  · 只有 1 个 GPIO 控制器：AlGpio_Hal_Init(&h, 0, NULL)
 *  · **没有公开的"设方向"API**（SetPinDirection/SetBankDirection 都是 static）
 *    -> 靠 AlGpio_Hal_WritePin() / ReadPinInput() **隐式设方向**
 *  · AlGpio_Hal_WritePin() 内部是 SetDirection(BIT(pin)) / ClrDirection(BIT(pin))，
 *    **只改目标位，不动同 bank 其它 pin**（已核对 al_gpio_hal.c:325-350）
 *    -> 所以先点灯1 再点灯2，不会互相干扰
 * ===========================================================================
 */

#include "apu_led.h"

#include "al_core.h"
#include "al_gpio_hal.h"
#include "apu_wdt.h"        /* apu_wdt_feed —— 闪灯自检循环里补喂狗 */

static AL_GPIO_HalStruct *g_gpio;
static uint8_t g_led_state;         /* bit0=灯1, bit1=灯2 */
static int g_led_ready;

#define APU_LED1_BIT    0x01U
#define APU_LED2_BIT    0x02U

/* 闪灯自检的参数 */
#define APU_LED_BLINK_TIMES     3U
#define APU_LED_BLINK_MS        150U

/* ---------------------------------------------------------------------------
 * 内部：真正写引脚
 * --------------------------------------------------------------------------- */
static void led_write(uint32_t pin, uint32_t on)
{
	if (!g_led_ready)
		return;
	(void)AlGpio_Hal_WritePin(g_gpio, pin, on ? 1U : 0U);
}

/* ---------------------------------------------------------------------------
 * 初始化
 * --------------------------------------------------------------------------- */
int apu_led_init(void)
{
	AL_S32 ret;

	ret = AlGpio_Hal_Init(&g_gpio, 0U, AL_NULL);
	if (ret != AL_OK) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][led] AlGpio_Hal_Init(DevId=0) failed, ret=0x%x",
		       (unsigned int)ret);
		g_led_ready = 0;
		return -1;
	}

	g_led_ready = 1;
	g_led_state = 0U;

	/* 先确保两盏都灭（写 0 会顺便把方向设成输出） */
	led_write(APU_LED1_PIN, 0U);
	led_write(APU_LED2_PIN, 0U);

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][led] PL GPIO ready. LEFT=pin%u(bit1/CEP11/U7)  RIGHT=pin%u(bit0/CEP9/V6)"
	       "  [2026-10-17 swapped per field test]",
	       (unsigned int)APU_LED1_PIN, (unsigned int)APU_LED2_PIN);

	/* ---- 肉眼自检：两盏各闪 3 次 ----
	 * 这一步是"GPIO 通路到底通不通"的最直接证据 */
	{
		uint32_t i;

		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][led] blink self-test: %u times each (watch the LEDs!)",
		       (unsigned int)APU_LED_BLINK_TIMES);

		for (i = 0U; i < APU_LED_BLINK_TIMES; i++) {
			led_write(APU_LED1_PIN, 1U);
			led_write(APU_LED2_PIN, 1U);
			AlSys_MDelay(APU_LED_BLINK_MS);

			led_write(APU_LED1_PIN, 0U);
			led_write(APU_LED2_PIN, 0U);
			AlSys_MDelay(APU_LED_BLINK_MS);

			/* ★ 2026-09-25：每轮补喂一次狗。
			 *   当前参数（3 次 × 2 × 150ms = 900ms）离 4030ms 超时还很远，
			 *   但这里是**循环里的纯阻塞**：一旦有人把 APU_LED_BLINK_TIMES
			 *   或 APU_LED_BLINK_MS 调大，整段就会变成"长时间不喂狗"。
			 *   在这里补喂，喂狗间隔上限就是一轮 2×BLINK_MS，
			 *   与次数无关 —— 以后调参数不会再踩狗。 */
			apu_wdt_feed();
		}
	}

	/* 读回一次，把结果打出来 —— 便于判断"是没写出去还是灯没接对" */
	{
		int rb1 = (int)AlGpio_Hal_ReadPinOutput(g_gpio, APU_LED1_PIN);
		int rb2 = (int)AlGpio_Hal_ReadPinOutput(g_gpio, APU_LED2_PIN);

		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][led] readback after blink: led1_out=%d led2_out=%d (expect 0 0)",
		       rb1, rb2);
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][led] if the LEDs never blinked: check CEP pin9/pin11 wiring, "
		       "LED polarity, and whether FSBL/BOOT.bin took effect");
	}

	return 0;
}

/*
 * ---------------------------------------------------------------------------
 * 开关（与传承项目 Led.c 的 LED_LEFT_ON / LED_RIGHT_ON 等对应）
 * ---------------------------------------------------------------------------
 * 两个写注释的坑（本人刚踩过，记下来）：
 *   1. 注释里不要出现反斜杠：C 的行拼接发生在注释处理之前，
 *      反斜杠会把下一行接进来，可能顺手吃掉注释结束符。
 *   2. 注释里不要出现注释结束符本身：它会**提前**结束注释，
 *      后面的普通代码就暴露出来当源码编译了。
 */
void apu_led_left_on(void)
{
	led_write(APU_LED1_PIN, 1U);
	g_led_state |= APU_LED1_BIT;
}

void apu_led_left_off(void)
{
	led_write(APU_LED1_PIN, 0U);
	g_led_state &= (uint8_t)~APU_LED1_BIT;
}

void apu_led_right_on(void)
{
	led_write(APU_LED2_PIN, 1U);
	g_led_state |= APU_LED2_BIT;
}

void apu_led_right_off(void)
{
	led_write(APU_LED2_PIN, 0U);
	g_led_state &= (uint8_t)~APU_LED2_BIT;
}

void apu_led_all_off(void)
{
	led_write(APU_LED1_PIN, 0U);
	led_write(APU_LED2_PIN, 0U);
	g_led_state = 0U;
}

/* ---------------------------------------------------------------------------
 * 诊断
 * --------------------------------------------------------------------------- */
int apu_led_read_back(uint32_t which)
{
	if (!g_led_ready)
		return -1;

	if (which == 1U)
		return (int)AlGpio_Hal_ReadPinOutput(g_gpio, APU_LED1_PIN);
	if (which == 2U)
		return (int)AlGpio_Hal_ReadPinOutput(g_gpio, APU_LED2_PIN);
	return -1;
}

uint8_t apu_led_get_state(void)
{
	return g_led_state;
}
