/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_link.h —— APU1 裸机侧"核间链路"层（**只有 RPMsg 一条出口**）
 * ===========================================================================
 *  【为什么还留这一层】
 *
 *  飞腾传承工程里，"往 Linux 发一条消息"就是直接调 rpmsg_send()。本工程多包一层，
 *  唯一目的是把 rpmsg_send 收敛到**一个文件里的一个函数**，好处有三个：
 *      · 业务层（apu_app.c）永远只调 apu_send_ack()/apu_send_sonar()，
 *        不需要认识 openamp 的头文件；
 *      · "端点就绪了没有"这个判断只写一遍（dest_addr == RPMSG_ADDR_ANY 就发不出去）；
 *      · 发送统计（ok/fail）集中在一处，排障时一个数字就能看出是"没发出去"还是
 *        "发出去了但对面没收"。
 *
 *  【★ 2026-09-25：串口替代出口已删除】
 *  原先这里有个 APU_LINK_MODE_TEST 分支，把消息打到串口而不发 RPMsg，
 *  还能用 `@link:test` 在运行期切走。那是独立测试期的脚手架，**已整段删除**：
 *      · 同一份固件有两个互斥出口 = 变量控不住（"上电到底在跑哪条路"要靠猜）；
 *      · 飞腾工程只有 send_str_to_linux() 一条路，为了对齐也必须只留一条。
 *  ⇒ 现在 apu_link_send_v() **无条件**调 rpmsg_send()。
 *
 *  【串口控制台还在，但它不是第二条出口】
 *  apu_testlink.c 的 `@` 命令仍然保留。命令有：
 *      @help  @mode  @stat  @hb  @nav  @ctrl  @wdt  @wdt:regs  @i2c  @i2cprobe
 *      @sonar:read  @sonar:stat  @sonar:reset  @imu:stat  @imu:raw  @imu:loop
 *      @esc:stop  @esc:regs  @rep  @shutdown
 *  它们只是"从串口把命令喂进业务层"，走的是和 rpmsg 收到 DEVICE_CORE_CHECK 时
 *  **完全相同的函数**（apu_app_handle_command），本身不会往 Linux 发任何东西。
 *  用途：Linux 还没起来 / 起不来时，唯一能看外设真实状态的路。
 *
 *  ⚠️ 写注释时注意：本文件里**不要**出现"以星号结尾的宏名紧跟一个斜杠"这种写法，
 *     那把"星号+斜杠"会提前结束块注释（本次就因此报过 stray '@' in program）。
 * ===========================================================================
 */

#ifndef APU_LINK_H
#define APU_LINK_H

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化链路层（清统计 + 打一行日志）。返回 0 成功。
 * 位置：main.c 的 openamp_sample() 里，print_config() 之前。 */
int apu_link_init(void);

/* ===========================================================================
 * 发送（全工程唯一的出口，内部就是 rpmsg_send）
 * ===========================================================================
 *  按"6 + length"语义发 RPMsg；端点没就绪（Linux 不在 / dest_addr 无效）则返回 -1。
 * =========================================================================== */
int apu_link_send(uint32_t cmd, const char *fmt, ...);

/* va_list 版本（给已经持有 va_list 的中间层用，例如 apu_msg_impl.c 的 apu_send） */
int apu_link_send_v(uint32_t cmd, const char *fmt, va_list args);

/* 链路是否真的可以发送（端点已绑定 & dest_addr 有效）。
 * 业务层目前不用，留给将来做"链路健康度"判断。 */
int apu_link_tx_ready(void);

/* ===========================================================================
 * 内部接口（只有 apu_msg_impl.c 该用）
 * =========================================================================== */

/* openamp 端点结构前置声明 */
struct rpmsg_endpoint;

/* 端点创建后由 apu_msg_impl 登记；本文件是全工程唯一调 rpmsg_send 的地方 */
void apu_link_attach_endpoint(struct rpmsg_endpoint *ept);

/* 发送统计（调试用，可传 NULL） */
void apu_link_get_stats(uint32_t *ok, uint32_t *fail);

#ifdef __cplusplus
}
#endif

#endif /* APU_LINK_H */
