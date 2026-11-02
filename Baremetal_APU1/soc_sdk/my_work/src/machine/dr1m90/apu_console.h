/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_console.h —— APU1 裸机侧"串口控制台"（本机排障用，**不是消息出口**）
 * ===========================================================================
 *  【为什么需要它】
 *
 *  本固件上行消息**只有一条路**：rpmsg（见 apu_link.h 顶部）。
 *  但实际调试时经常遇到"Linux 侧还没起来 / 起不来"的情形，那时：
 *      · rpmsg 发不出去，看不到任何上报；
 *      · 也没法用 Linux 侧的命令去驱动外设。
 *  于是留一个**纯本机**的串口控制台：从日志 UART 读一行命令，
 *  直接交给业务层的 apu_app_handle_command() —— 和 rpmsg 收到
 *  DEVICE_CORE_CHECK 时**是同一个函数**。
 *
 *  【它不做什么（很重要）】
 *      ✗ 不把任何消息发给 Linux（上行永远走 rpmsg）
 *      ✗ 不接收 Linux 的命令
 *      ✗ 不是"第二条链路"、也没有"链路模式"可切
 *  它只是"把人手敲的命令喂进业务层"，以及打印外设/寄存器状态。
 *
 *  【硬件约定】
 *  复用**日志用的那一个 UART**（`LOG_DEV = AL_LOG_UART1`，即 MIO48/49 → CH342K）。
 *  好处：不用再初始化第二个 UART 对象，也不怕和日志抢硬件。
 *  实现方式：直接用 SDK 已经初始化好的全局句柄 `extern AL_UART_HalStruct *AlLog`
 *  （`al_log.h:22` 明确 extern 出来了）。
 *
 *  ⚠️ 如果你把 LOG_DEV 改成 UART0，控制台会跟着跑到 UART0 上去
 *     （但 UART0 现在是 IMU/姿态板在用，别改）。
 *  ⚠️ 接管 ISR 依赖 GIC 已就绪：`apu_testlink_init()` 必须排在
 *     `platform_init()`（内部 init_system -> AlIntr_SetLocalInterrupt）之后。
 * ===========================================================================
 */

#ifndef APU_CONSOLE_H
#define APU_CONSOLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 命令行最大长度（含结尾 '\0'） */
#define APU_CONSOLE_LINE_MAX    96

/* 初始化控制台：接管日志 UART 的接收中断。
 *
 * ⚠️ 顺序要求：必须在 SDK 的 AlLog_Init() 之后调用
 *    （components_init() 里已经调过了，所以放在 main() 里就是安全的）。
 * 返回：0 成功；-1 失败（日志 UART 没起来）。 */
int apu_console_init(void);

/* 每轮主循环调一次。
 * 返回：1 = line 里有一条**完整命令**（已去掉 \r\n 和首尾空格）；
 *       0 = 还没有完整的一行。 */
int apu_console_poll(char *line, uint32_t max_len);

/* 往控制台打一行（自动补 \r\n，用 AL_LOG）。 */
void apu_console_print(const char *fmt, ...);

/* 往控制台打一段不带换行的原文（用于 `[REPORT] ` 这类前缀）。 */
void apu_console_write_raw(const char *s);

/* 统计：收到的字节数 / 因行太长被丢弃的次数（排查"命令没反应"时有用） */
uint32_t apu_console_rx_bytes(void);
uint32_t apu_console_overflow_count(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_CONSOLE_H */
