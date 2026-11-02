/*********************************************************************
**  system_top.v —— FPGA 顶层（USB 摄像头方案 + 4 路 PL 串口）
**
**  工程 : C:\qiansai_FPGA\my_cpu_npu\soc_prj
**  器件 : DR1M90GEG400      工具 : Anlogic TD
**  底稿 : 官方 NPU Demo（05-5_NPU演示\demo\soc_prj）
**
**  ── 版本记录 ────────────────────────────────────────────────
**    2026-09-26  基于官方 NPU Demo 顶层，移植 my_work 的 8 个 EMIO 外设端口
**    2026-10-10  裁剪 MIPI 采集通路（design_top_wrapper / uidbuf_write /
**                uiFDMA 及其端口），改为 USB-only
**    2026-10-10  ★ 新增 3 路 AXI UART Lite（GP0 总线，基地址
**                0x8040_0000 / 0x8050_0000 / 0x8060_0000），
**                并把 pl_gpio 扩展到 3 位（bit[2] = RS485 收发使能）
**    2026-10-15  ★ pl_gpio 再扩到 5 位，新增气泵/电磁阀控制：
**                bit[3] = gas_valve（电磁阀 V7） / bit[4] = gas_pump（气泵 W6）
**                需求见《气泵与电磁阀控制引脚IO及工作时序20261015.md》
**    2026-10-30  ★ 新增第 4 路 AXI UART Lite（UART-D，0x8070_0000）：
**                前向超声波的 32 板接口损坏，改为 CEP pin17/18 直连
**                TX=Y7  RX=Y6（BANK11 / LVCMOS33）
**
**  ── ⚠️ 前置条件：pl_gpio 必须是对外 5 位端口 ────────────────────
**    本文件里 pl_gpio_in / pl_gpio_oe / pl_gpio_out 声明为 [4:0]。
**    PS IP 内部也已经是 5 位（GPIO_PL_Dot_IO = 5），且 design_0.v
**    的端口已确认是 [4:0]（2026-10-15 导出）。
**    ⚠️ 但**块设计的对外端口**不会因为 IP 参数变化而自动加宽，
**    历史上就这么静默截断过一次：assign pl_gpio_oe = <3位net> 把
**    bit[2] 丢掉 → RS485 的 DE 永远拉不高 → 串口 B 只能收不能发。
**    所以改 GPIO 位数后，务必：
**      1. 在 TD 框图里删掉三个对外端口 pl_gpio_in / pl_gpio_oe / pl_gpio_out
**      2. 重选 PS 的这三个引脚 → 右键 Make External（重新生成端口）
**      3. Validate Design → 保存 → Generate Output Products
**      4. 打开 hdl/design_0.v 确认已变成 [4:0]
**    本文件现在按 [4:0] 写死：位宽不匹配时会**直接报错**而不是静默失效
**    （报错总比查半天"为什么阀不动作"好）。
**
**  ── 四路串口分工 ────────────────────────────────────────────
**    UART-A  rx_a/tx_a  0x8040_0000  115200  → 气体传感器   （CEP 排针）
**    UART-B  rx_b/tx_b  0x8050_0000  115200  → 浑浊度传感器 （板载 RS485 U21）
**    UART-C  rx_c/tx_c  0x8060_0000  115200  → 超声波       （CEP 排针）
**    UART-D  rx_front/tx_front 0x8070_0000 115200 → 前向超声波（CEP pin17/18）
**    中断：UART-A/B/C/D 的 interrupt 分别接 Concat_0 的 In3/In4/In5/In6，
**          Concat_0 的 Dout[15:0] 进 PS 的 f2p_irq[15:0]。
**          GIC_SPI = Concat 输入索引 + 82  →  85 / 86 / 87 / 88
**          Linux INTR_ID = GIC_SPI + 32    →  117 / 118 / 119 / 120
*********************************************************************/

module system_top(
    // ================= HDMI 输出（块设计内，必需）=================
    output [2:0] O_hdmi_tx_p,
    output       O_hdmi_clk_p,

    // ================= LED 状态指示 =============================
    //   led[0] = pwm_out, led[1] = vtg_lock, led[2] = axis_to_video_lock
    output [2:0] led,

    // ================= PS EMIO 外设（接 CEP 排针）=================
    inout  wire i2c0_scl,
    inout  wire i2c0_sda,
    inout  wire i2c1_scl,
    inout  wire i2c1_sda,
    input  wire uart0_rx,
    output wire uart0_tx,
    inout  wire led1,
    inout  wire led2,

    // ================= ★ 新增：4 路 AXI UART Lite =================
    //  UART-A：气体传感器（RS485/RS232 模块，走 CEP 排针）
    output wire uart_gas_tx,        // → CEP 排针 pin15 (V11)
    input  wire uart_gas_rx,        // ← CEP 排针 pin16 (V10)

    //  UART-B：浑浊度传感器（走板载 RS485 收发器 U21 = SP3485）
    output wire uart_turb_tx,       // → U21 第4脚 DI     (L17)
    input  wire uart_turb_rx,       // ← U21 第1脚 RO     (N16)

    //  UART-C：超声波（走 CEP 排针）
    output wire uart_ultra_tx,      // → CEP 排针 pin13 (U9)
    input  wire uart_ultra_rx,      // ← CEP 排针 pin14 (U8)

    //  UART-D：前向超声波（32 板接口损坏后改直连 CEP 排针）
    //  ⚠️ 这是 3.3V TTL；若探头是 5V TX，必须加电平转换/分压，不能直连
    output wire uart_front_tx,      // → CEP 排针 pin17 (Y7)
    input  wire uart_front_rx,      // ← CEP 排针 pin18 (Y6)

    //  RS485 收发使能（U21 的 DE 与 /RE 短接）：1 = 发送，0 = 接收
    output wire rs485_de,           // → U21 第2/3脚 DE+RE (N15)

    // ================= ★ 新增：气泵 / 电磁阀 继电器控制 ==========
    //  两块继电器均为**高电平有效**：1 = 吸合（阀开 / 泵转），0 = 断开。
    //  驱动电路：GPIO → 限流电阻 → N-MOS 栅极；继电器线圈反并联续流二极管。
    //  栅极 10k 下拉到 GND，保证复位瞬间为低（阀关、泵关）。
    //  见《气泵与电磁阀控制引脚IO及工作时序20261015.md》第 1.2 节。
    output wire gas_valve,          // 电磁阀（进气阀）→ CEP pin12 (V7)
    output wire gas_pump            // 微型隔膜泵     → CEP pin10 (W6)
);

    /**********************************************************
                    块设计 design_0 的连线
    **********************************************************/
    wire        i2c0_scl_in;
    wire        i2c0_sda_in;
    wire        i2c0_scl_oe;
    wire        i2c0_sda_oe;

    wire        i2c1_scl_in;
    wire        i2c1_sda_in;
    wire        i2c1_scl_oe;
    wire        i2c1_sda_oe;

    wire        uart0_rx_in;
    wire        uart0_tx_out;

    /*  ★ 5 位：与块设计 design_0 的 pl_gpio 端口位宽一致
     *     bit[0] = led1 照明灯1
     *     bit[1] = led2 照明灯2
     *     bit[2] = rs485_de（UART-B 收发使能）
     *     bit[3] = gas_valve 电磁阀
     *     bit[4] = gas_pump  气泵
     *  ⚠️ 位宽必须与 design_0.v 的 [4:0] 完全相同 —— 少了会报位宽错，
     *     多了会静默截断（历史上 RS485 DE 就这么失效过）。 */
    wire  [4:0] pl_gpio_in;
    wire  [4:0] pl_gpio_oe;
    wire  [4:0] pl_gpio_out;

    /* 采集通路已删除 → 这个中断不再产生，显式接 0
       （design_0 边界上 f2p_irq 仍是 1 位；16 位汇聚在 BD 内部，
         外部这一位原来由 uidbuf_write.camera_done 驱动） */
    wire        f2p_irq = 1'b0;

    /**********************************************************
                    PS（块设计 design_0）
    **********************************************************/
    design_0 design_0_inst (
        // ---- ★ EMIO：I2C / UART / PL_GPIO ----
        .i2c0_scl_in  (i2c0_scl_in ),
        .i2c0_scl_oe  (i2c0_scl_oe ),
        .i2c0_sda_in  (i2c0_sda_in ),
        .i2c0_sda_oe  (i2c0_sda_oe ),
        .i2c1_scl_in  (i2c1_scl_in ),
        .i2c1_scl_oe  (i2c1_scl_oe ),
        .i2c1_sda_in  (i2c1_sda_in ),
        .i2c1_sda_oe  (i2c1_sda_oe ),
        .uart0_rx_in  (uart0_rx_in ),
        .uart0_tx_out (uart0_tx_out),
        .pl_gpio_in   (pl_gpio_in  ),
        .pl_gpio_oe   (pl_gpio_oe  ),
        .pl_gpio_out  (pl_gpio_out ),

        // ---- ★ 新增 4 路 AXI UART Lite ----
        .rx_a (uart_gas_rx   ),   // UART-A 气体传感器
        .tx_a (uart_gas_tx   ),
        .rx_b (uart_turb_rx  ),   // UART-B 浑浊度传感器（RS485）
        .tx_b (uart_turb_tx  ),
        .rx_c (uart_ultra_rx ),   // UART-C 超声波
        .tx_c (uart_ultra_tx ),
        .rx_front (uart_front_rx), // UART-D 前向超声波
        .tx_front (uart_front_tx),

        // ---- HDMI ----
        .O_hdmi_tx_p (O_hdmi_tx_p),
        .O_hdmi_clk_p(O_hdmi_clk_p),

        // ---- LED 状态指示 ----
        .pwm_out            (led[0]),
        .vtg_lock           (led[1]),
        .axis_to_video_lock (led[2]),

        // ---- 采集通路已删除：中断输入显式接 0 ----
        .f2p_irq (f2p_irq)
    );

    /*====================================================================
     *  以下 design_0 端口本版**刻意不连接**，不是漏了：
     *
     *   hp1_w_us1_*        PS 的 HP1 写从口。原来是 uiFDMA 往 DDR 写
     *                      摄像头数据用的，现在没有主设备。
     *                      输入悬空 → 工具按 0 处理 → 总线静默，无害。
     *
     *   clk_150m/rstn_150m 原是 uiFDMA / uidbuf_write 的时钟和复位
     *   p2f_clk0_50m       原是 design_top_wrapper 里 MIPI PLL 的参考时钟
     *   camera_base_addr / camera_start / camera_clear
     *                      原是 uidbuf_write 的地址与启动/清中断握手
     *
     *   以上都是 design_0 的**输出**，悬空无害。
     *==================================================================*/

    /*====================================================================
     *  I2C0 —— 开漏双向（接 PCA9685 舵机板，地址 0x40）
     *==================================================================*/
    assign i2c0_scl_in = i2c0_scl;                   /* 回读 SCL 线电平 */
    assign i2c0_scl    = i2c0_scl_oe ? 1'b0 : 1'bz;  /* oe=1 拉低，否则释放 */
    assign i2c0_sda_in = i2c0_sda;
    assign i2c0_sda    = i2c0_sda_oe ? 1'b0 : 1'bz;

    /*====================================================================
     *  I2C1 —— 开漏双向（CEP pin 5/6）
     *==================================================================*/
    assign i2c1_scl_in = i2c1_scl;
    assign i2c1_scl    = i2c1_scl_oe ? 1'b0 : 1'bz;
    assign i2c1_sda_in = i2c1_sda;
    assign i2c1_sda    = i2c1_sda_oe ? 1'b0 : 1'bz;

    /*====================================================================
     *  UART0 —— PS 侧 EMIO 串口（CEP pin 7/8，115200 8N1）
     *      TX/RX 要【交叉】接：本板 TX -> 模块 RX，本板 RX <- 模块 TX
     *==================================================================*/
    assign uart0_rx_in = uart0_rx;
    assign uart0_tx    = uart0_tx_out;

    /*====================================================================
     *  PL_GPIO[1:0] —— led1 / led2（两盏照明灯，CEP pin 9 / 11）
     *      ⚠️ 引脚驱动只有 8mA，只够点普通 LED 看亮灭；
     *         要点真照明灯必须经 MOSFET 驱动板 + 灯独立供电
     *==================================================================*/
    assign pl_gpio_in[0] = led1;
    assign led1          = pl_gpio_oe[0] ? pl_gpio_out[0] : 1'bz;
    assign pl_gpio_in[1] = led2;
    assign led2          = pl_gpio_oe[1] ? pl_gpio_out[1] : 1'bz;

    /*====================================================================
     *  PL_GPIO[2] —— 板载 RS485（U21 SP3485）收发使能 DE
     *
     *  ⚠️ 这里【不能】照抄 led1/led2 的三态写法！
     *     DE 是一根控制输入，悬空会让 SP3485 进入不确定状态，
     *     很可能误进发送态把整条 485 总线堵死（所有设备都收不到数据）。
     *     所以这里做成**常驱推挽输出**。
     *
     *  复位默认态：PS 的 GPIO 输出寄存器复位为 0，oe 复位为 0，
     *     于是 rs485_de = 0 → 接收态，这是最安全的默认值。
     *
     *  时序（很重要）：发送时先拉高 DE → 写 TX FIFO → 等最后一个字节
     *     完全移出移位寄存器 → 再拉低 DE。
     *     只判 SR.bit2（TX FIFO Empty）**不够**：FIFO 空了不代表字节已
     *     移出。115200 下建议额外等约 1 个字节时间（≈87 us）。
     *     少了这步，一帧最后 1~2 字节会被截断，Modbus RTU 靠帧间间隔
     *     判帧，表现为"偶发通信失败"，极难定位。
     *==================================================================*/
    assign rs485_de      = pl_gpio_out[2];

    /*  ⚠️ 这里回读的是上面那根**内部连线**（= 我们自己写出去的值），
        不是 N15 焊盘上的实际电平。
        它的用处只有一个：让软件确认"自己确实写进去了、端口位宽没被截断"
        （真被截成 2 位的话，pl_gpio_out[2] 这个索引会直接编译报错）。
        焊盘电平只能用万用表/示波器实测 —— 别把它当成引脚测量结果。 */
    assign pl_gpio_in[2] = rs485_de;

    /*====================================================================
     *  PL_GPIO[3] / PL_GPIO[4] —— 电磁阀 / 气泵 继电器控制（2026-10-15 新增）
     *
     *  CEP 排针：pin12 (V7) = 电磁阀，pin10 (W6) = 气泵
     *            （紧挨着已有的 pin9 V6 = led1、pin11 U7 = led2）
     *
     *  ⚠️ 三条硬性要求，改这里之前务必读完：
     *
     *  ① 【必须常驱推挽输出，不能照抄 led1/led2 的三态写法】
     *     继电器驱动级的 N-MOS 栅极是**高阻输入**，如果这里输出 'z'，
     *     栅极就只靠那颗 10k 下拉电阻定电平 —— 平时看似没问题，
     *     但只要有一点漏电/干扰就可能被抬到阈值以上，继电器**乱吸合**。
     *     泵和阀是执行机构，误动比不动的后果严重得多。
     *       ⇒ 所以用 assign 直接推出 pl_gpio_out 的值，**不要**写 'z'。
     *
     *  ② 【复位默认必须是低电平（阀关、泵关）】
     *     PS 的 GPIO 输出寄存器复位为 0、oe 复位为 0，
     *     所以 pl_gpio_out[4:3] 上电即为 00 → 两路都断开。
     *     ⚠️ 不要在 PS 侧给这两位设初始为 1，否则上电瞬间泵就会转。
     *     另外硬件上 N-MOS 栅极还应加 10k 下拉（见需求文档 1.2 节），
     *     软件默认态 + 硬件下拉 = 双保险。
     *
     *  ③ 【上电后软件必须主动写一次 0】
     *     虽然复位值是 0，但为了防御"复位不彻底/热复位残留"，
     *     APU1 固件启动时应显式把这两位清 0（见需求文档 6.2 节）。
     *
     *  回读（pl_gpio_in[4:3]）接的是上面这两根内部连线，
     *  用途和 bit[2] 一样：让软件确认自己确实写进去了、位宽没被截断。
     *  焊盘上的实际电平仍只能实测。
     *==================================================================*/
    assign gas_valve = pl_gpio_out[3];
    assign gas_pump  = pl_gpio_out[4];

    assign pl_gpio_in[3] = gas_valve;
    assign pl_gpio_in[4] = gas_pump;

endmodule
