/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#ifndef AXI_HDMI_H_
#define AXI_HDMI_H_

#define AXI_HDMI_REG_RESET 0x040
#define AXI_HDMI_REG_CTRL 0x044
#define AXI_HDMI_REG_SOURCE_SEL 0x048
#define AXI_HDMI_REG_COLORPATTERN 0x04c
#define AXI_HDMI_REG_STATUS 0x05c
#define AXI_HDMI_REG_VDMA_STATUS 0x060
#define AXI_HDMI_REG_TPM_STATUS 0x064
#define AXI_HDMI_REG_HTIMING1 0x400
#define AXI_HDMI_REG_HTIMING2 0x404
#define AXI_HDMI_REG_HTIMING3 0x408
#define AXI_HDMI_REG_VTIMING1 0x440
#define AXI_HDMI_REG_VTIMING2 0x444
#define AXI_HDMI_REG_VTIMING3 0x448
#define HDMI_BASE_ADDR 0x800

/* FMDA config , include camera and hdmi fdma config */

#define CAMERA_BASE_ADDR 0xC00
#define CAMERA_START 0xC04
#define CAMERA_CLEAR_INTR 0xC08

#define AXI_VDMA_REG_DMA_CTRL 0x00
#define AXI_VDMA_REG_V_SIZE 0x50
#define AXI_VDMA_REG_H_SIZE 0x54
#define AXI_VDMA_REG_FRMDLY_STRIDE 0x58
#define AXI_VDMA_REG_START_1 0x5C
#define AXI_VDMA_REG_START_2 0x60
#define AXI_VDMA_REG_START_3 0x64

#define AXI_CLKGEN_V2_REG_RESET 0x40
#define AXI_CLKGEN_V2_REG_DRP_CNTRL 0x70
#define AXI_CLKGEN_V2_REG_DRP_STATUS 0x74

#define AXI_CLKGEN_V2_RESET_MMCM_ENABLE (1 << 1)
#define AXI_CLKGEN_V2_RESET_ENABLE (1 << 0)

#define AXI_CLKGEN_V2_DRP_CNTRL_SEL (1 << 29)
#define AXI_CLKGEN_V2_DRP_CNTRL_READ (1 << 28)
#define AXI_CLKGEN_V2_DRP_STATUS_BUSY (1 << 16)

#endif
