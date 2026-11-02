
# =====================================================================
#  ★ 2026-10-10 USB-only 裁剪：删除所有 MIPI 相关时钟约束
# =====================================================================
#  删掉的两处：
#
#   1) create_clock -name clk_mipi -period 6.4 [...]
#      指向 design_top_wrapper_u/u_mipi_dphy_rx_dr1_mipiio_wrapper/...
#      —— 注意：这个层次路径本来就和实际例化的
#         u_mipi_dphy_rx_over_lvds_wrapper 对不上，也就是说这条约束
#         一直是**空的**（这正是之前报 9 处时钟引脚无约束的原因之一）。
#      MIPI 整条通路删掉后，clk_mipi 彻底没有意义，必须删，
#      否则 set_clock_groups 会报 "clock not found"。
#
#   2) design_top_wrapper_u/U_PLL/dr1_phy_pll_59fc11d80b0d_Inst/...
#      这是 MIPI 通路里那个参考时钟 PLL 的输出，随通路一起删。
#
#  ★ 保留原则：原来 design_0 内部那几个 PLL 时钟之间的关系
#    （PLL_npu × pll_75m 互斥）**一个字都没改**，
#    只把 clk_mipi 这一组拿掉、把 pll_150m 独立成组。
#    不要把三组合并成一个，那会改变时序分析结果。
# =====================================================================

derive_clocks

# 从 ps p2f_rst0_n 开始的路径都设为 false path（原样保留）
set_false_path -from [get_nets {design_0_inst/design_0_ARM_Processor_System_0_Inst/p2f_rst0_n}]

# 块设计内部：NPU 时钟 × 75M 时钟 互斥（原样保留，去掉 clk_mipi 与 U_PLL 两组）
set_clock_groups -exclusive \
    -group [get_clocks {design_0_inst/design_0_PLL_npu_Inst/dr1_phy_pll_dcc52a480158_Inst/u_DR1_PHY_PLL.clkc[0]}] \
    -group [get_clocks {design_0_inst/design_0_pll_75m_Inst/dr1_phy_pll_1f386b1db2c3_Inst/u_DR1_PHY_PLL.clkc[0]}]

# ---------------------------------------------------------------------
#  ⚠️ 综合/布局后请重点看这一条：
#     clk_150m 原来是给 uiFDMA / uidbuf_write 用的，本版顶层已不再连接，
#     所以 design_0_pll_150m_Inst 有可能被工具当成"没人用"而优化掉。
#     如果 TD 报 "clock ... not found"，把这个 PLL 也加进上面的互斥组
#     （或单独给它 set_clock_groups -asynchronous）即可，不影响功能。
# ---------------------------------------------------------------------
