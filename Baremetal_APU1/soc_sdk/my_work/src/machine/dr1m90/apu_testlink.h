/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_testlink.h —— 本机串口控制台（排障用，**不是消息出口**）
 * ===========================================================================
 *  【它是什么】
 *
 *  一个**纯本机**的命令控制台：从日志 UART 读一行，直接交给
 *  apu_app_handle_command()（= rpmsg 收到 DEVICE_CORE_CHECK 时同一个函数），
 *  以及一组 `@` 开头的状态查询/自检命令。
 *
 *  【它不是什么】（★ 2026-09-25 起）
 *
 *  ✗ 不是"第二条消息出口"：上行消息一律走 rpmsg，**不会**打到串口。
 *    原先那个会把消息打成串口行的串口替代出口、以及 `@link:test` /
 *    `@link:prod` 运行期切换，**已整段删除**。
 *    理由：一份固件不能有两个互斥出口，否则"上电在跑哪条路"要靠猜。
 *  ✗ 没有"传感器注入"：业务层数据只来自真驱动（与传承工程一致）。
 *
 *  【两边对照，便于理解它替代了什么】
 *
 *      实际联调时                       本控制台能做的
 *      ------------------------------   ------------------------------------
 *      命令 "CMD:xxx"  ← Linux rpmsg     手打同样的 "CMD:xxx"（同一个业务函数）
 *      心跳 "PING:n"   ← Linux 每 500ms  敲 `@hb` 手动喂一次
 *      姿态 / 超声波                     只能看（@imu:stat / @sonar:stat），
 *                                        数据只来自真外设，无法注入
 *      外设/寄存器状态                   只能在本机看
 *                                        （@nav @ctrl @wdt @i2c @esc:regs ...）
 * ===========================================================================
 */

#ifndef APU_TESTLINK_H
#define APU_TESTLINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化控制台（内部会 apu_console_init()）。
 * ⚠️ 必须在 SDK 的 AlLog_Init() 之后调用。返回 0 成功。
 * ⚠️ 必须在 platform_init() 之后调用（它会接管 UART ISR，而让 CPU 真正
 *    接受 IRQ 的是 platform_init -> init_system -> AlIntr_SetLocalInterrupt）。 */
int apu_testlink_init(void);

/* 每轮主循环调一次：从控制台取命令并处理。
 * 返回：1 = 处理了一条命令；0 = 没有输入。 */
int apu_testlink_poll(void);

/* 控制台打印帮助（开机自检和 `@help` 都调它） */
void apu_testlink_print_help(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_TESTLINK_H */
