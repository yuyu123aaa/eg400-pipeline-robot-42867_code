/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_depth.h —— 下潜深度超声波（直接挂 UART-C 的 PL 串口）
 * ===========================================================================
 *  【与现有三路超声的区别】
 *
 *      前/左/右三路：传感器 -> STM32 中转 -> I2C1 -> 本板  （apu_sonar.c）
 *      本模块      ：传感器 -------- 直连 ------> UART-C    （不问 I2C）
 *
 *  两者物理上完全独立，本模块**不碰** apu_sonar.c，也不进业务层的
 *  安全门/居中控制 —— 它现在只负责"读出来 + 打到串口1"。
 *
 *  【通信协议】（来源：参考资料 L08 驱动 ultrasonic.c，三路串口配置逐字一致）
 *
 *      串口参数 : 115200 8N1
 *      交互方式 : **一问一答** —— 必须先发触发字节，传感器才回一帧
 *      触发字节 : 0x01
 *      应答帧   : 固定 4 字节
 *                 [0] = 0xFF          帧头
 *                 [1] = 距离 高字节
 *                 [2] = 距离 低字节
 *                 [3] = 校验和
 *      距离     : (buf[1] << 8) | buf[2]        单位 mm
 *      校验     : sum = (buf[0]+buf[1]+buf[2]) & 0xFF，必须 == buf[3]
 *
 *  【读数语义】（踩过的坑，务必记住）
 *
 *      · 65535 = 回波超时/丢波 = **传感器失效**。传承项目的安全规则是
 *        "任一路出现 65535 就立刻停车"（见其设计文档 sonar-65535）。
 *      · **0 不算失效**，它可能真的贴着目标。
 *      · ⚠️ 参考代码的 L08_ParseData() 在"校验失败/帧头不对"时**返回 0**，
 *        于是"解析失败"和"距离真的是 0"混在一起，调用方分不清。
 *        本模块把这两种情况分开：res 给出明确的失败原因（见 APU_DEPTH_E_*）。
 *
 *  【怎么让路】
 *      本模块只为"打通链路"而加。验证通过、要接进业务层时：
 *        · 要么把它改成正式的深度驱动（进 apu_app_periodic + 上报）；
 *        · 要么整个删掉，只留这里的协议说明与 APU_DEPTH_* 常量。
 *      因此它**刻意不依赖任何业务模块**，删起来是干净的。
 *
 *  【接线】UART-C = CEP 排针 pin13/pin14
 *      FPGA: uart_ultra_tx = U9  ->  接传感器 RX
 *      FPGA: uart_ultra_rx = U8  <-  接传感器 TX
 *      ⚠️ TX/RX 要交叉；且两者都是 3.3V LVCMOS33（PL 引脚约束已定）
 * ===========================================================================
 */

#ifndef APU_DEPTH_H
#define APU_DEPTH_H

#include <stdint.h>

/* 本模块建立在三路 PL 串口原语之上：需要 apu_pluart_base() 与 APU_PLUART_ULTRA。
 * 在这里 include 是为了让调用方（main.c 等）只 #include "apu_depth.h" 就够，
 * 不必自己再记得包含 apu_pluart.h —— 少一个漏包含的机会。 */
#include "apu_pluart.h"

/* ---------------------------------------------------------------------------
 *  ★ 调试打印开关（2026-10-15）
 * ---------------------------------------------------------------------------
 *  数据通路已走 rpmsg（apu_env.c 的 "ENV:..." 上报），驱动层不再刷串口1。
 *  默认 0 ⇒ 连一帧都不会打。需要单独排查这一路时改成 1 重编。
 * --------------------------------------------------------------------------- */
#ifndef APU_DEPTH_DEBUG_PRINT
#define APU_DEPTH_DEBUG_PRINT   0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 *  协议常量（照抄自 L08 驱动，改这些等于改协议，别乱动）
 * --------------------------------------------------------------------------- */
#define APU_DEPTH_TRIGGER_BYTE      0x01U   /* 主机发出的触发字节 */
#define APU_DEPTH_FRAME_LEN         4U      /* 响应帧固定 4 字节 */
#define APU_DEPTH_FRAME_HEADER      0xFFU   /* 帧头 */
#define APU_DEPTH_INVALID_MM        65535U  /* 失效值：回波超时/丢波 */

/* 默认读取周期（ms）。参考实现是"触发后等应答，再 Delay_ms(30)"，
 * 这里留足余量取 200ms（5Hz），足够看清波形又不刷屏。 */
#define APU_DEPTH_PERIOD_MS         200U

/* 触发后等整帧的超时（ms）。参考实现给的是 ms 级余量，这里给 60ms。 */
#define APU_DEPTH_FRAME_TIMEOUT_MS  60U

/* ---------------------------------------------------------------------------
 *  单次读取结果
 *
 *  ⚠️ distance_mm 只有在 res == APU_DEPTH_E_OK 时才可信。
 *     特别地：res != OK 时 distance_mm 一定是 0，**不要**把它当"距离 0"用。
 * --------------------------------------------------------------------------- */
typedef enum {
	APU_DEPTH_E_OK = 0,        /* 收到完整合法帧 */
	APU_DEPTH_E_NO_DEVICE,     /* UART-C 实例查不到 —— bit 流不对 */
	APU_DEPTH_E_TX_STUCK,      /* 发触发字节时 TX FIFO 一直满 —— IP 没在跑 */
	APU_DEPTH_E_TIMEOUT_HEAD,  /* 等帧头 0xFF 超时 —— 传感器没回 */
	APU_DEPTH_E_TIMEOUT_REST,  /* 收到帧头但后 3 字节没来齐 */
	APU_DEPTH_E_BAD_SUM,       /* 校验和不匹配 */
	APU_DEPTH_E_INVALID        /* 帧合法但值是 65535（丢波/失效） */
} apu_depth_err_t;

typedef struct {
	int      res;              /* apu_depth_err_t */
	uint16_t distance_mm;      /* 仅在 res == APU_DEPTH_E_OK 时可信 */
	uint8_t  raw[4];           /* 原始 4 字节，便于对照示波器/逻辑分析仪 */
	uint8_t  raw_len;          /* 实际收到的字节数（0~4） */
	uint32_t elapsed_ms;       /* 从发触发到收齐（或超时）用了多少 ms */
} apu_depth_result_t;

/* ---------------------------------------------------------------------------
 *  接口
 * --------------------------------------------------------------------------- */

/* 初始化。返回 0 = 成功；-1 = UART-C 实例不存在（bit 流不是含三路 UART 的那版） */
int apu_depth_init(void);

/* 读一次（**阻塞式，最长约 APU_DEPTH_FRAME_TIMEOUT_MS**）。
 * 返回 0 = 本次拿到合法读数（res 里再细分 OK / INVALID）；
 * 返回 -1 = 参数无效。详细原因看 out->res。 */
int apu_depth_read(apu_depth_result_t *out);

/* 周期任务：内部按 APU_DEPTH_PERIOD_MS 限速，到点就读一次并打印到串口1。
 * 挂进主循环调用即可（非阻塞返回，没到点直接 return）。 */
void apu_depth_poll(void);

/* 把结果按人类可读格式打到串口1（PS UART1 日志口 -> PC） */
void apu_depth_print(const apu_depth_result_t *r);

/* 纠错字符串（打印用） */
const char *apu_depth_err_str(int res);

#ifdef __cplusplus
}
#endif

#endif /* APU_DEPTH_H */
