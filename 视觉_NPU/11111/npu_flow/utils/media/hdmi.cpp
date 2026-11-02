/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#include "hdmi.hpp"
#include "axi_hdmi.h"
#include <chrono>
#include <cstdint>
#include <iostream>
#include "logger_forward.hpp"
#include "fast_io.h"
#include "armnnALUtils/cma_mem_init.hpp"
#ifndef CFV_BASEADDR
#define CFV_BASEADDR (0x80100000) // Hdmi Config
#endif
#define try_throw(expr, str)           \
    if (!(expr)) {                     \
        throw std::runtime_error(str); \
    }

hdmi_t::~hdmi_t() {
    if (is_open())
        DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_RESET), 0x0); // 关闭hdmi
    m_is_open = false;
}
void hdmi_t::selfTest() {
    try_throw(iic.IsOpen(), "i2c not open");
    DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_RESET), 0x0);

    // #define DUMP(reg) LOG_TRACE("0x{:x}=0x{:x}({})", reg, *fio(reg), #reg)
    //     DUMP(CFV_BASEADDR + AXI_HDMI_REG_RESET);
    //     DUMP(CFV_BASEADDR + AXI_HDMI_REG_CTRL);
    //     DUMP(CFV_BASEADDR + AXI_HDMI_REG_SOURCE_SEL);
    //     DUMP(CFV_BASEADDR + AXI_HDMI_REG_COLORPATTERN);
    //     DUMP(CFV_BASEADDR + AXI_HDMI_REG_STATUS);
    //     DUMP(CFV_BASEADDR + AXI_HDMI_REG_VDMA_STATUS);
    //     DUMP(CFV_BASEADDR + AXI_HDMI_REG_TPM_STATUS);
    // #undef DUMP

    const unsigned char golden[16] = {0x25, 0x37, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x62, 0x04, 0xa8, 0x00, 0x00, 0x1c, 0x84};
    (void)(golden);
    unsigned char buff[16];
    memset(buff, 0, sizeof(buff));
    auto err_code = iic.Read(dev_addr, (uint8_t)0x10, buff);
    try_throw(err_code == 0, "hdmi.iic.Read return error code = " + std::to_string(err_code));
    // for (int i = 0; i < 4; i++) {
    //     try_throw(buff[i] == golden[i], "hdmi check failed");
    // }

    // for (int i = 0; i < 16; i++) {
    //     if (buff[i] != golden[i]) {
    //         has_been_inited = true;
    //         LOG_WARN("init hdmi twice!");
    //         return;
    //     }
    // }
}

void hdmi_t::init() {
    configReg(0x41, 0x10);
    configReg(0x98, 0x03);
    configReg(0x9a, 0xe0);
    configReg(0x9c, 0x30);
    configReg(0x9d, 0x01);
    configReg(0xa2, 0xa4);
    configReg(0xa3, 0xa4);
    configReg(0xe0, 0xD0);
    configReg(0xF9, 0x00);
    configReg(0x15, 0x01);
    configReg(0x16, 0x3c);
    configReg(0x17, 0x00);
    configReg(0x48, 0x08);
    configReg(0xd0, 0x3c);
    configReg(0xaf, 0x82);
    configReg(0x4c, 0x4);
    configReg(0x40, 0x0);
    configReg(0x18, 0xe7);
    lastInit = std::chrono::system_clock::now();
}

void hdmi_t::Hdmi_Cfg() {
    if (m_cfg_done) return;
    m_cfg_done = true;
    //
    // static const unsigned long detailedTiming[9] = {74250000, 1280, 370, 110, 40, 720, 30, 5, 5};
    static const unsigned long detailedTiming[9] = {150000000, 1920, 280, 88, 44, 1080, 45, 4, 5};
    unsigned short horizontalCount = 0;
    unsigned short verticalCount = 0;
    unsigned short horizontalBackPorch = 0;
    unsigned short verticalBackPorch = 0;
    unsigned short horizontalDeMin = 0;
    unsigned short horizontalDeMax = 0;
    unsigned short verticalDeMin = 0;
    unsigned short verticalDeMax = 0;
    unsigned short horizontalActiveTime = detailedTiming[1];
    unsigned short horizontalBlankingTime = detailedTiming[2];
    unsigned short horizontalSyncOffset = detailedTiming[3];
    unsigned short horizontalSyncPulseWIDTH_720P = detailedTiming[4];
    unsigned short verticalActiveTime = detailedTiming[5];
    unsigned short verticalBlankingTime = detailedTiming[6];
    unsigned short verticalSyncOffset = detailedTiming[7];
    unsigned short verticalSyncPulseWIDTH_720P = detailedTiming[8];

    horizontalCount = horizontalActiveTime + horizontalBlankingTime;
    verticalCount = verticalActiveTime + verticalBlankingTime;
    horizontalBackPorch = horizontalBlankingTime - horizontalSyncOffset - horizontalSyncPulseWIDTH_720P;
    verticalBackPorch = verticalBlankingTime - verticalSyncOffset - verticalSyncPulseWIDTH_720P;
    horizontalDeMin = horizontalSyncPulseWIDTH_720P + horizontalBackPorch;
    horizontalDeMax = horizontalDeMin + horizontalActiveTime;
    verticalDeMin = verticalSyncPulseWIDTH_720P + verticalBackPorch;
    verticalDeMax = verticalDeMin + verticalActiveTime;

    DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_RESET), 0x0);

    DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_HTIMING1), ((horizontalActiveTime << 16) | horizontalCount));
    DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_HTIMING2), horizontalSyncPulseWIDTH_720P);
    DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_HTIMING3), ((horizontalDeMax << 16) | horizontalDeMin));
    DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_VTIMING1), ((verticalActiveTime << 16) | verticalCount));
    DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_VTIMING2), verticalSyncPulseWIDTH_720P);
    DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_VTIMING3), ((verticalDeMax << 16) | verticalDeMin));
    DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_RESET), 0x1);
    DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_SOURCE_SEL), 0x0);
    DEMOV2_REG32_WRITE((CFV_BASEADDR + AXI_HDMI_REG_SOURCE_SEL), 0x1);
    static bool first = false;
    if (!first) {
        first = true;
        LOG_WARN("HDMI TX Core Config Finished ! \n");
    }
}

void hdmi_t::configReg(uint8_t reg, uint8_t val) {
    uint8_t val_[2];
    val_[0] = reg;
    val_[1] = val;
    try_throw(0 == iic.Write(dev_addr, val_), "i2c write failed");
}

bool hdmi_t::Read_HPD_Status() {
    iic.Read(dev_addr, (unsigned char)0x42, i2c_buf);
    if ((i2c_buf[0] & (1U << 6)) == 0) {
        return false;
    }
    iic.Read(dev_addr, (unsigned char)0x96, i2c_buf);
    if ((i2c_buf[0] & (1U << 7)) == 0) {
        return false;
    }
    return true;
}
void hdmi_t::show(uint64_t base_addr, void *virt_addr, int pixelformat) {
    cma_mem_sync_for_device(base_addr, buffer_size());
    (*fio((CFV_BASEADDR) + 0x800) = base_addr);
    auto during = std::chrono::system_clock::now() - lastInit;
    if (during.count() > 5e8) {
        // 每隔一段时间去检查hdmi是否要热插拔
        lastInit = std::chrono::system_clock::now();
        if (Read_HPD_Status()) {
            reConfig();
            HPD_Intr_Clr();
        }
    }
    Hdmi_Cfg();
}

void hdmi_t::open(const char *dev) {
    int nums = i2c_helper::GetBusNums();
    int dev_num = -1;
    for (int i = 0; i < nums; i++) {
        i2c_helper iicx;
        iicx.Open(i);
        if (iicx.IsOpen()) {
            uint8_t val_[2];
            val_[0] = 0x41;
            val_[1] = 0x10;
            if (0 == iicx.Write(dev_addr, val_)) {
                unsigned char buff[1];
                buff[0] = 0;
                iicx.Read(dev_addr, *val_, buff);
                if (buff[0] != val_[1] && buff[0] != 0) {
                    fprintf(stderr, "not sure dev of i2c%d res-0x%x(not 0x10)\n", i, buff[0]);
                }
                if (buff[0] == val_[1]) {
                    dev_num = i;
                    break;
                }
            }
        }
    }
    if (dev_num < 0) {
        std::cerr << "No HDMI device found" << std::endl;
    }
    iic.Open(dev_num);
    if (iic.IsOpen()) {
        try {
            selfTest();
            init();
            m_is_open = true;
        } catch (const std::runtime_error &e) {
            std::cerr << e.what() << std::endl;
            return;
        }
    }
}
