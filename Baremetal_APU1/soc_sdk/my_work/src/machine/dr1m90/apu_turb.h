/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_turb.h —— 浑浊度传感器 XM3318B（板载 RS485，走 UART-B）
 * ===========================================================================
 *  【硬件与供电】（规格书《XM3318B 产品使用手册 V25.2.5》第 2 页）
 *
 *      品牌/型号 : XUNCHIP 讯芯 XM3318B（RS485 浊度模块）
 *      通讯      : RS485，**Modbus-RTU**
 *      默认波特率: **9600 8-n-1**
 *      默认地址  : **1**
 *      供电      : **DC 6~24V 1A**   ← ⚠️ 5V 低于下限，不能用！
 *      测量范围  : 0~1000 UNT
 *      响应时间  : <500ms
 *
 *  【接线】模块 RS485 直接挂到板载 SP3485（U21）的对外端子 J11：
 *      模块 485-A -> J11 第5脚 (485_A0 / 网络 RS485_A)     A 接 A
 *      模块 485-B -> J11 第6脚 (485_B0 / 网络 RS485_B)     B 接 B
 *      模块 GND   -> J11 第1或4脚 (GND)                    ★ 必须共地
 *      模块 电源  -> DC 9~12V（不要用 5V）
 *
 *      FPGA 侧三根线（板载 SP3485，U21）：
 *          L17 = RS485_TX0 -> U21 DI      （发送）
 *          N15 = RS485_DE0 -> U21 DE+RE   （方向：高=发，低=收）
 *          N16 = RS485_RX0 <- U21 RO      （接收）
 *
 *  【协议】（规格书第 8~9 页）
 *
 *      读浊度（功能码 0x03，读 1 个寄存器，起始地址 0x0000）：
 *          问询: 01 03 00 00 00 01 84 0A
 *          应答: 01 03 02 02 18 B9 2E
 *                |  |  |  |     +--- CRC16，低字节在前
 *                |  |  |  +--------- 数据 = 0x0218 = 536
 *                |  |  +------------ 字节数 = 2
 *                |  +--------------- 功能码 03
 *                +------------------ 设备地址（出厂 1）
 *
 *  【数值换算 —— 手册唯一的含糊点，务必实测标定】
 *      手册原文："02 18 转为十进制数值为 536，**假设**数据倍率为 100，
 *                则真实值为 536/100 = 5.36"
 *      ⚠️ 它写的是"假设"。而且量程是 0~1000 UNT，若倍率真是 100，
 *         原始值满量程 65535 对应 655.35，与 1000 UNT 对不上。
 *      所以本模块的做法是：
 *          · 原始值 raw 一定原样打印出来（这是硬事实）
 *          · 换算倍率由 APU_TURB_SCALE 决定，默认 1.0（即 raw 就是 UNT）
 *          · 上板后拿清水/浊水对照手册（清水应接近 0），再决定倍率
 *      改倍率只需改 apu_turb_set_scale()，不用重编。
 *
 *  【RS485 半双工 —— 本模块最容易出错的地方】
 *      收发顺序必须是：
 *          ① 拉高 DE（切发送态）
 *          ② 发问询帧（apu_pluart_write 内部已等排空）
 *          ③ **立刻拉低 DE（切回接收态）** —— 晚了就把传感器的回复压住
 *          ④ 收应答
 *      ⚠️ 第③步不能靠"发完就算"，必须显式做，而且必须在 write 返回后立刻做。
 *
 *  【与另外两路的分工】
 *      UART-A : 气体传感器   <-- apu_gas.c   (Modbus-RTU 9600, 地址1)
 *      UART-B : 浑浊度传感器 <-- 本模块       (Modbus-RTU 9600, 地址1)
 *      UART-C : 下潜深度超声 <-- apu_depth.c (一问一答 115200)
 *      三者物理独立；地址虽都是 1，但各走各的总线，不冲突。
 * ===========================================================================
 */

#ifndef APU_TURB_H
#define APU_TURB_H

#include <stdint.h>

/* 需要 apu_pluart_base() / APU_PLUART_TURB / RS485 DE 原语 */
#include "apu_pluart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 *  协议常量（照抄规格书，改这些等于改协议）
 * --------------------------------------------------------------------------- */
#ifndef APU_TURB_DEBUG_PRINT
#define APU_TURB_DEBUG_PRINT    0       /* ★ 默认关闭：数据已走 rpmsg，不再刷串口1 */
#endif
#define APU_TURB_SLAVE_ADDR     0x01U   /* 出厂默认地址；可用 0xFA(250) 通用地址查询 */
#define APU_TURB_ADDR_BROADCAST 0xFAU   /* 250：不知道地址时的通用地址 */
#define APU_TURB_FUNC_READ      0x03U   /* 功能码：读保持寄存器 */
#define APU_TURB_REG_TURBIDITY  0x0000U /* 浊度寄存器 */
#define APU_TURB_REG_TYPE_CODE  0x0064U /* 型号编码 */
#define APU_TURB_REG_ADDR       0x0066U /* 设备地址（读/写） */
#define APU_TURB_REG_BAUD       0x0067U /* 波特率编码（读/写） */
#define APU_TURB_REG_MODE       0x0068U /* 通讯模式：1=查询 2=主动上传 */
#define APU_TURB_REG_PROTOCOL   0x0069U /* 协议类型：1=MODBUS-RTU */

#define APU_TURB_REQ_LEN        8U      /* 问询帧固定 8 字节 */
#define APU_TURB_RESP_LEN       7U      /* 应答帧固定 7 字节（1+1+1+2+2） */

/* 读取周期（ms）。9600 下问询约 8ms、应答约 7ms，模块响应 <500ms，取 1000ms */
#define APU_TURB_PERIOD_MS      1000U

/* 整帧超时（ms）。规格书说响应时间 <500ms，这里给 600ms。 */
#define APU_TURB_FRAME_TIMEOUT_MS 600U

/* 浊度换算倍率：真实值 = raw / APU_TURB_SCALE_DEFAULT
 * 默认 1.0f（即 raw 直接就是 UNT）。手册写"假设倍率100"，但那是假设，
 * 且与 0~1000UNT 量程对不上 —— 实测标定后再定。 */
#define APU_TURB_SCALE_DEFAULT  1.0f

/* ---------------------------------------------------------------------------
 *  读取结果
 * --------------------------------------------------------------------------- */
typedef enum {
	APU_TURB_E_OK = 0,       /* 收到合法应答，CRC 通过 */
	APU_TURB_E_NO_DEVICE,    /* UART-B 实例查不到 —— bit 流不对 */
	APU_TURB_E_TX_STUCK,     /* 发问询时 TX FIFO 一直满 —— IP 没在跑 */
	APU_TURB_E_TIMEOUT,      /* 没收到应答（DE 切回太晚 / 模块没上电 / 波特率） */
	APU_TURB_E_BAD_ADDR,     /* 应答里的地址/功能码对不上 */
	APU_TURB_E_BAD_BYTECOUNT,/* 字节数域不是 0x02 */
	APU_TURB_E_BAD_CRC,      /* CRC 不过 —— 波特率/接线/干扰 */
	APU_TURB_E_DE_FAIL       /* RS485 方向脚（PL GPIO）没起来 */
} apu_turb_err_t;

typedef struct {
	int      res;                  /* apu_turb_err_t */
	uint8_t  raw[APU_TURB_RESP_LEN]; /* 原始应答，便于对照示波器 */
	uint8_t  raw_len;              /* 实际收到多少字节 */

	uint16_t raw_value;            /* 从应答里取出的 16 位原始值 */
	float    value;                /* 换算后的浊度值（= raw_value / scale） */
	float    scale;                /* 本次用的倍率 */
	const char *unit;              /* "UNT" */

	uint32_t elapsed_ms;           /* 从发问询到收齐用了多少 ms */
} apu_turb_result_t;

/* ---------------------------------------------------------------------------
 *  接口
 * --------------------------------------------------------------------------- */
int  apu_turb_init(void);
int  apu_turb_read(apu_turb_result_t *out);
void apu_turb_poll(void);       /* 周期任务：按 APU_TURB_PERIOD_MS 限速，打串口1 */
void apu_turb_print(const apu_turb_result_t *r);
const char *apu_turb_err_str(int res);

/* 现场调试用 --------------------------------------------------------------- */

/* 改从机地址（仅影响后续问询；不写模块内部寄存器）。返回 0 成功。 */
int  apu_turb_set_addr(uint8_t addr);
uint8_t apu_turb_get_addr(void);

/* 改换算倍率（真实值 = raw / scale）。scale <= 0 会被拒绝。 */
int  apu_turb_set_scale(float scale);
float apu_turb_get_scale(void);

/* 读模块的「设备地址」寄存器（0x0066）。
 * 用途：拿到一个只有地址未知的模块时，先用 0xFA 通用地址问出来。
 * 返回 0 = 成功，out_addr 有效；-1 = 失败（详细原因看 out->res）。 */
int  apu_turb_probe_addr(uint8_t *out_addr);

/* 读模块内部寄存器（通用）。reg = APU_TURB_REG_xxx。
 * 成功返回 0 并把值写进 *out_val。 */
int  apu_turb_read_reg(uint16_t reg, uint16_t *out_val);

/* 上板自检：主动查一次模块地址并打印（用 0xFA 通用地址）。
 * 用途：接线/供电/波特率都对不对，一条命令看清楚。 */
void apu_turb_diag(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_TURB_H */
