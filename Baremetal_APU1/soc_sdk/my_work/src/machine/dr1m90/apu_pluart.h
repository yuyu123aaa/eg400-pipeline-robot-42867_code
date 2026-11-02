/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_pluart.h —— 四路 PL 串口（AXI UART Lite）底层原语
 * ===========================================================================
 *  【本文件现在提供什么】
 *
 *  只有"怎么跟 AXI UART Lite 讲话"这一层原语，供**真传感器驱动**调用：
 *
 *      · 基地址查询          apu_pluart_base(dev_id)
 *      · 非阻塞收一个字节    apu_pluart_getc(base, &c)
 *      · 发一串并等排空      apu_pluart_write(base, buf, n, &stuck)
 *      · 收干净 RX FIFO      apu_pluart_rx_flush(base)
 *      · RS485 方向脚 DE     apu_pluart_rs485_de() / _de_readback()
 *
 *  【本文件不再提供什么】（2026-10-13 删除，真传感器到位后不再需要）
 *
 *      · 回环自检（TX 短接 RX 再逐字节比对）
 *      · 控制台的 @pluart 系列命令
 *      · RS485「只看输出」的示波器测试帧与连续发送模式
 *      · 上电自动发 0x55 的启动期测试流
 *
 *  这些是"手上没有真外设、只想验 FPGA 通路"时用的台架手段。三路串口
 *  已经在实测中确认为好，外部传感器也已到位，留着只会：
 *      ① 上电白等（回环自检约 0.4s；485 启动流**整整 60 秒**，且这 60 秒里
 *         固件停在 platform_init() 之前，完全不做核间通信，还会把 RS485 的
 *         DE 一直拉高、独占总线）
 *      ② 在日志里刷屏，把真正的启动信息淹掉
 *  需要那段测试代码时，从工程外的存档里取回即可。
 *
 *  【三路串口的物理位置】（板卡：米联客 MLK-F3P-CZ02 / DR1M90GEG400）
 *
 *      UART-A  0x80400000  气体传感器    CEP pin15(V11)=TX  pin16(V10)=RX
 *      UART-B  0x80500000  浑浊度传感器  板载 RS485：L17=TX  N16=RX  N15=DE
 *      UART-C  0x80600000  超声波        CEP pin13(U9)=TX   pin14(U8)=RX
 *
 *      ⚠️ UART-B 的 N16 是 SP3485 的 RO **输出脚**（RE 与 DE 短接在 N15）。
 *         所以**不要**把 B 的 TX/RX 短接做回环 —— 那是两个推挽输出对打，
 *         可能损坏 IO。A / C 短接是安全的（TX 出、RX 入，只有一侧在驱动）。
 *
 *  【三条设计硬约束】（都是踩过坑才留下来的）
 *
 *  ① **纯轮询，不开中断**
 *     本模块一个中断都不用，全靠轮询 SR 寄存器 + 超时退出。
 *     原因：JTAG 独立调试时 GIC 状态不可靠（本核通路本来就绕开了
 *     Distributor），而"串口通不通"必须比中断通路更容易判断 ——
 *     否则出问题时你分不清是串口坏了还是中断没来。
 *     真传感器驱动若要改成中断方式，那是驱动自己的事，本层不掺和。
 *
 *  ② **每一处等待都有上界**
 *     没有一处 `while (标志位)` 是无限等的。TX 等 FIFO 不满有超时，
 *     等排空也有超时。**绝不允许串口把主循环卡死。**
 *
 *  ③ **绕开 SDK HAL 层的数组越界隐患**（★ 这条最重要）
 *     `driver/pl_driver/axi_uart_lite/src/al_axi_uart_lite_hal.c:18`：
 *          static Al_AxiUartLite_HalStruct AlAxiUartLiteHandle[AL_UART_NUM_INSTANCE];
 *     而 `chip/dr1x90/common/inc/al_hwcfg.h:23`：`#define AL_UART_NUM_INSTANCE (2)`
 *     —— 这个宏本来是给 **PS UART**（只有 UART0/UART1 两个）用的，
 *     却被 PL 的 axi_uart_lite HAL 复用了。于是以 DevId=2 调
 *          AlAxiUartLite_Hal_Init(&h, 2, ...)
 *     时，它会写 `&AlAxiUartLiteHandle[2]` —— **数组只有 2 个元素，越界**。
 *     现象是"静默踩坏 BSS 里紧邻的静态变量"，不报错、不崩，最难查。
 *     ⇒ 本模块只用 **DEV 层 + LL 内联函数**（按 BaseAddress 直接访问寄存器），
 *       完全不碰 HAL，因此三个 DevId（0/1/2）都能安全使用。
 *
 *  【与其它模块的关系】
 *      · 只读/写 AXI_UARTLite 的寄存器，不动 I2C / PCA9685 / IMU / WDT
 *      · 不打印任何东西（打印是调用方的事），不占 rpmsg
 *      · 不注册任何回调、不改任何全局开关 ⇒ 对现有功能零影响
 * ===========================================================================
 */

#ifndef APU_PLUART_H
#define APU_PLUART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 *  逻辑编号 —— 与 soc_plat.h 里的 AXI_UARTLite_<id> 一一对应
 * --------------------------------------------------------------------------- */
#define APU_PLUART_GAS      0   /* UART-A  0x80400000  气体传感器 */
#define APU_PLUART_TURB     1   /* UART-B  0x80500000  浑浊度传感器（RS485） */
#define APU_PLUART_ULTRA    2   /* UART-C  0x80600000  超声波 */
#define APU_PLUART_FRONT    3   /* UART-D  0x80700000  前向超声波（直连） */
#define APU_PLUART_NUM      4

/* ---------------------------------------------------------------------------
 *  超时（ms）
 *
 *  ⚠️ 这些值是按 115200bps 定的：1 个字节 = 87us。
 *     `apu_pluart_write()` 的一次调用里，**每个字节**等 TX FIFO 不满、
 *     以及最后等排空，都用 APU_PLUART_TX_TIMEOUT_MS 作为上界。
 *     若把波特率降到 9600，单字节变成 1.04ms，仍然远小于 50ms，够用。
 * --------------------------------------------------------------------------- */
#define APU_PLUART_TX_TIMEOUT_MS    50U

/* 发完再等这么久才允许拉低 RS485 的 DE（见 apu_pluart_write 的说明） */
#define APU_PLUART_485_DRAIN_US     120U    /* 87us（1 字节）+ 裕量 */

/* RS485 方向脚：PL GPIO bank2 bit2 = 全局引脚号 56（54=led1，55=led2） */
#define APU_PLUART_DE_PIN           56U

/* ---------------------------------------------------------------------------
 *  RS485 方向控制（UART-B 专用）
 *
 *  SP3485 的 DE 与 /RE 短接在 N15：
 *      DE = 1 -> 发送态；DE = 0 -> 接收态
 *  上电复位默认是"接收"，这也是最安全的状态。
 * --------------------------------------------------------------------------- */

/* 手动控制 DE：1 = 发送态，0 = 接收态。
 * ⚠️ 内部会做一次幂等的 GPIO 初始化；初始化失败时**静默返回**，
 *    要判断是否可用请配合 apu_pluart_rs485_de_readback()。 */
void apu_pluart_rs485_de(int on);

/* 回读 DE。*out = GPIO 输出寄存器值（我们写进去的）；*pin = 内部回环读回值。
 *
 * ⚠️ **这两个都不是物理焊盘上的电平**（别说成"引脚实测"，会误导）：
 *      system_top.v 里是  assign rs485_de = pl_gpio_out[2];
 *                         assign pl_gpio_in[2] = rs485_de;
 *    所以 pin 读回来的就是"我们自己写出去的值"，是 **FPGA 内部连线回环**。
 *    它的价值有两个：
 *      ① 证明 PS 侧确实把 1 写进了 GPIO 输出寄存器
 *      ② 证明 pl_gpio 的对外端口是 3 位 —— 真被截成 2 位的话，
 *         pl_gpio_out[2] 这个索引会直接编译报错，根本跑不起来
 *    N15 焊盘上到底是不是 3.3V ⇒ **只能实测**（示波器/万用表量 N15 或 U21 第 3 脚）。
 *
 * 返回 0 成功，-1 = GPIO 没起来。任一指针可为 NULL。 */
int  apu_pluart_rs485_de_readback(int *out, int *pin);

/* ---------------------------------------------------------------------------
 *  串口原语
 * --------------------------------------------------------------------------- */

/* 基地址（设备实例不存在时返回 0） */
uint32_t apu_pluart_base(int dev_id);

/* 收一个字节。
 * 返回：1 = 收到（*c 有效）；0 = 当前没有数据（**非阻塞**，立刻返回）。
 * ⚠️ 返回值语义容易记反：**1 才是收到**。典型用法：
 *        uint8_t c;
 *        while (apu_pluart_getc(base, &c)) { 处理(c); } */
int apu_pluart_getc(uint32_t base, uint8_t *c);

/* 清空 RX FIFO：写 CR 的 RX-RESET 位，然后把残留字节读干净。
 * 有上界（最多 256 字节）——即使线上一直有数据在来也不会死循环。
 * 用途：一帧开始前丢弃上一帧的残渣，避免把旧数据当新帧解析。 */
void apu_pluart_rx_flush(uint32_t base);

/* 把 n 个字节写进 TX FIFO，然后**等排空**。
 *
 *   · 写每个字节前等 TX FIFO 不满，超时上界 APU_PLUART_TX_TIMEOUT_MS；
 *   · 发完等 TX FIFO 空，**再多等 APU_PLUART_485_DRAIN_US（120us）**。
 *
 *  ⚠️ 最后那 120us 不能省：**FIFO 空 ≠ 最后一个字节已经移出移位寄存器**。
 *     115200 下还要约 87us 才真正发完。少了这一步，一串的最后 1~2 字节
 *     会被截断 —— Modbus RTU 靠帧间间隔判帧，症状是"偶发通信失败"，极难查。
 *
 *  ⚠️ RS485（UART-B）的调用顺序必须是这样，否则一帧会被自己截断：
 *        apu_pluart_rs485_de(1);        // 先切发送态
 *        apu_pluart_write(base, buf, n, &stuck);   // 本函数内部已等排空
 *        apu_pluart_rs485_de(0);        // 再切回接收态
 *
 *  ⚠️ **本函数不碰 DE**，只负责把字节交给 IP 并等它发完。方向控制是调用方的事，
 *     因为 A / C 两路根本没有 DE 这根线。
 *
 *  返回：实际写进 FIFO 的字节数（可能 < n）。
 *        *stuck != 0 表示 TX FIFO 一直满、超时退出（该 IP 没在跑，
 *        通常是 bit 流不对或 PL 时钟没起来）。 */
uint32_t apu_pluart_write(uint32_t base, const uint8_t *buf, uint32_t n,
			  int *stuck);

#ifdef __cplusplus
}
#endif

#endif /* APU_PLUART_H */
