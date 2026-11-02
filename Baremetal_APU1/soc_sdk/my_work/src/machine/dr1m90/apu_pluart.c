/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_pluart.c —— 四路 PL 串口（AXI UART Lite）底层原语实现
 * ===========================================================================
 *  设计说明、物理引脚、以及"为什么不用 HAL 层"全部写在 apu_pluart.h 顶部。
 *  这里只列实现层面的几个关键决定：
 *
 *  · 只用 DEV 层做一次性安全初始化（关中断 + 复位两个 FIFO），
 *    其余收发全部用 LL 内联函数按 BaseAddress 直接读写寄存器。
 *  · 所有等待都带超时，任何一个环节超时都立刻返回并把原因报告给调用方，
 *    绝不把主循环卡死。
 *
 *  【2026-10-13 删除了什么】
 *    回环自检 / @pluart 控制台命令 / RS485 示波器测试帧 / 连续发送模式 /
 *    上电自动发 0x55 的启动期测试流 —— 这些是"没有真外设时验通路"用的台架
 *    手段，实测已完成，现在由真传感器驱动取代。留下的只有下面这些原语。
 * ===========================================================================
 */

#include "apu_pluart.h"

#include "al_core.h"

#include "al_axi_uart_lite_ll.h"
#include "al_axi_uart_lite_dev.h"
#include "al_gpio_hal.h"        /* RS485 的 DE（PL GPIO bit2） */

#include "apu_time.h"

/* ---------------------------------------------------------------------------
 *  ⚠️ SDK 的 al_reg_io.h 在 **ARM 分支**里只给下面这些起了名字：
 *        AL_PL_REG8/16/32/64_READ、..._GET_BITS/GET_BIT、..._SET_BITS/SET_BIT
 *     唯独**没有** `AL_PL_REG32_WRITE`（只有 riscv 分支才有）。
 *     所以写 CR 寄存器这里统一用 AL_REG32_WRITE —— 在 ARM 上它俩本来就
 *     是同一个宏（`*(volatile AL_U32 *)(addr) = val;`），不存在差异。
 *     （这个坑是编译时报 "implicit declaration of AL_PL_REG32_WRITE" 才发现的。）
 * --------------------------------------------------------------------------- */
#define APU_PLUART_CR_WRITE(base, val) \
	AL_REG32_WRITE((base) + AL_AXI_UART_LITE_CR_OFFSET, (val))

/* ---------------------------------------------------------------------------
 *  RS485 方向脚（DE）的内部状态。
 *  de_init() 是幂等的：只有第一次真正去初始化 GPIO 控制器。
 * --------------------------------------------------------------------------- */
static AL_GPIO_HalStruct *g_de_gpio;
static int      g_de_ready;

/* ---------------------------------------------------------------------------
 *  串口原语
 * --------------------------------------------------------------------------- */

uint32_t apu_pluart_base(int dev_id)
{
	AlAxiUartLite_HwConfigStruct *cfg;

	if (dev_id < 0 || dev_id >= APU_PLUART_NUM)
		return 0U;

	/* LookupConfig 内部按 AL_AXI_UART_LITE_NUM_INSTANCE(=4) 遍历，
	 * 四个 DevId 都查得到；它只读常量表，不需要先 Init。 */
	cfg = AlAxiUartLite_Dev_LookupConfig((AL_U32)dev_id);
	if (cfg == AL_NULL)
		return 0U;

	return (uint32_t)cfg->BaseAddress;
}

int apu_pluart_getc(uint32_t base, uint8_t *c)
{
	if (c == AL_NULL || base == 0U)
		return 0;

	if (!(AlAxiUartLite_GetStatus(base) &
	      BIT(AL_AXI_UART_LITE_SR_RXFIFO_HAS_DATA_SHIFT)))
		return 0;

	*c = (uint8_t)AlAxiUartLite_RecvData(base);
	return 1;
}

void apu_pluart_rx_flush(uint32_t base)
{
	uint32_t guard;

	if (base == 0U)
		return;

	AlAxiUartLite_ResetRxFifo(base);
	AlSys_UDelay(50U);          /* 复位位需要几个 ACLK 才生效 */

	guard = 0U;
	while ((AlAxiUartLite_GetStatus(base) &
		BIT(AL_AXI_UART_LITE_SR_RXFIFO_HAS_DATA_SHIFT)) && guard < 256U) {
		(void)AlAxiUartLite_RecvData(base);
		guard++;
	}
}

uint32_t apu_pluart_write(uint32_t base, const uint8_t *buf, uint32_t n,
			  int *stuck)
{
	uint32_t i, sent = 0U, tw;

	if (stuck != AL_NULL)
		*stuck = 0;

	if (buf == AL_NULL || base == 0U || n == 0U) {
		if (stuck != AL_NULL && base == 0U)
			*stuck = 1;     /* 基地址无效：等同于"这个 IP 不在" */
		return 0U;
	}

	for (i = 0U; i < n; i++) {
		tw = apu_time_ms_live();

		while (AlAxiUartLite_GetStatus(base) &
		       BIT(AL_AXI_UART_LITE_SR_TXFIFO_FULL_SHIFT)) {
			if ((uint32_t)(apu_time_ms_live() - tw) >
			    APU_PLUART_TX_TIMEOUT_MS) {
				if (stuck != AL_NULL)
					*stuck = 1;
				break;
			}
		}
		if (stuck != AL_NULL && *stuck)
			break;

		AlAxiUartLite_SendData(base, buf[i]);
		sent++;
	}

	if (!(stuck != AL_NULL && *stuck)) {
		uint32_t td = apu_time_ms_live();

		while (!(AlAxiUartLite_GetStatus(base) &
			 BIT(AL_AXI_UART_LITE_SR_TXFIFO_EMPTY_SHIFT))) {
			if ((uint32_t)(apu_time_ms_live() - td) >
			    APU_PLUART_TX_TIMEOUT_MS)
				break;
		}
		AlSys_UDelay(APU_PLUART_485_DRAIN_US);
	}

	return sent;
}

/* ---------------------------------------------------------------------------
 *  RS485 方向控制（UART-B / SP3485 的 DE）
 * --------------------------------------------------------------------------- */

/* GPIO 控制器初始化（幂等，失败也不会把 g_de_ready 置成半吊子状态） */
static int de_init(void)
{
	if (g_de_ready)
		return 0;

	if (AlGpio_Hal_Init(&g_de_gpio, 0U, AL_NULL) != AL_OK) {
		g_de_gpio  = AL_NULL;
		g_de_ready = 0;
		return -1;
	}
	g_de_ready = 1;

	/* 默认态必须是"接收"，这是最安全的：写 0 会顺手把方向设成输出 */
	(void)AlGpio_Hal_WritePin(g_de_gpio, APU_PLUART_DE_PIN, 0U);
	return 0;
}

void apu_pluart_rs485_de(int on)
{
	if (de_init() != 0)
		return;

	(void)AlGpio_Hal_WritePin(g_de_gpio, APU_PLUART_DE_PIN, on ? 1U : 0U);
}

int apu_pluart_rs485_de_readback(int *out, int *pin)
{
	if (de_init() != 0)
		return -1;

	if (out != AL_NULL)
		*out = (int)AlGpio_Hal_ReadPinOutput(g_de_gpio, APU_PLUART_DE_PIN);
	if (pin != AL_NULL)
		*pin = (int)AlGpio_Hal_ReadPinInput(g_de_gpio, APU_PLUART_DE_PIN);

	return 0;
}
