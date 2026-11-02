/************************************************************\
**	Copyright (c) 2012-2026 Anlogic Inc.
**	All Right Reserved.
\************************************************************/
/************************************************************\
**	Gen time	:	Oct 30 2026 00:43:10
**	IpTool version	:	2026.03(193873 2026/03/31)
**	App version	:	Release_2026.1_SP2
**	Ip Version	:	1.2
\************************************************************/
module design_0_AXI_UARTLite_2
(
  input                         aclk,
  input                         aresetn,
  output                        interrupt,
  input   [3:0]                 us_axi_awaddr,
  input                         us_axi_awvalid,
  output                        us_axi_awready,
  input   [31:0]                us_axi_wdata,
  input   [3:0]                 us_axi_wstrb,
  input                         us_axi_wvalid,
  output                        us_axi_wready,
  output  [1:0]                 us_axi_bresp,
  output                        us_axi_bvalid,
  input                         us_axi_bready,
  input   [3:0]                 us_axi_araddr,
  input                         us_axi_arvalid,
  output                        us_axi_arready,
  output  [31:0]                us_axi_rdata,
  output  [1:0]                 us_axi_rresp,
  output                        us_axi_rvalid,
  input                         us_axi_rready,
  input                         rx,
  output                        tx
);

  axi_uart_simple_design_0_AXI_UARTLite_2
  #(
      .AL_AXI_ACLK_FREQ_HZ(75000000),
      .AL_BAUDRATE(115200),
      .AL_DATA_BITS(8),
      .AL_USE_PARITY(0),
      .AL_ODD_PARITY(1),
      .AL_FIFO_DEPTH(16),
      .AL_USE_DYN_BAUDRATE(0)
  )axi_uart_simple_Inst
  (
      .aclk(aclk),
      .aresetn(aresetn),
      .interrupt(interrupt),
      .us_axi_awaddr(us_axi_awaddr),
      .us_axi_awvalid(us_axi_awvalid),
      .us_axi_awready(us_axi_awready),
      .us_axi_wdata(us_axi_wdata),
      .us_axi_wstrb(us_axi_wstrb),
      .us_axi_wvalid(us_axi_wvalid),
      .us_axi_wready(us_axi_wready),
      .us_axi_bresp(us_axi_bresp),
      .us_axi_bvalid(us_axi_bvalid),
      .us_axi_bready(us_axi_bready),
      .us_axi_araddr(us_axi_araddr),
      .us_axi_arvalid(us_axi_arvalid),
      .us_axi_arready(us_axi_arready),
      .us_axi_rdata(us_axi_rdata),
      .us_axi_rresp(us_axi_rresp),
      .us_axi_rvalid(us_axi_rvalid),
      .us_axi_rready(us_axi_rready),
      .rx(rx),
      .tx(tx)
  );
endmodule
