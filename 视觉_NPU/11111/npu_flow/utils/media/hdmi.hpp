/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#pragma once

#include "i2c_helper.hpp"
#include <chrono>
#include "ihdmi.hpp"
#include <cstdint>
#ifndef DEMOV2_REG32_WRITE
#define DEMOV2_REG32_WRITE(reg, val) (*fio(reg) = val)
#endif
/**
 * @brief hdmi暂时性解决方案
 * @code {.cpp}
 *  m_hdmi.iic.Open(0);
 *  m_hdmi.selfTest();
 *  m_hdmi.init();
 * @endcode
 * @deprecated 暂时性解决方案，已废弃
 *
 */
class hdmi_t : public ihdmi {
public:
    i2c_helper iic;
    ~hdmi_t();

private:
    static constexpr uint8_t dev_addr = 0x39;
    unsigned char i2c_buf[1];
    bool has_been_inited = false;
    std::chrono::time_point<std::chrono::system_clock> lastInit;
    bool m_is_open = 0;

public:
    /**
     * @brief 打开设备
     *
     * @param dev 空字符串，自动枚举
     */
    virtual void open(const char *dev = "") override;
    virtual bool is_open() override { return m_is_open; }
    virtual int width() override {
        return 1920;
    }
    virtual int height() override {
        return 1080;
    }
    virtual int channel() override {
        return 4; // fixed bgra
    }
    virtual size_t buffer_size() override {
        return width() * height() * channel();
    }

    virtual void show(uint64_t base_addr, void *virt_addr = nullptr, int pixelformat = 4) override;

    void selfTest();

    void init();

    void HPD_Intr_Clr() {
        configReg(0x96, 1u << 7);
    }

    bool Read_HPD_Status();

    void reConfig() {
        init();
        m_cfg_done = false;
    }

private:
    void Hdmi_Cfg();
    void configReg(uint8_t reg, uint8_t val);
    bool m_cfg_done = false;
};