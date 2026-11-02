/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#pragma once

#include "i2c_helper.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <stdio.h>
#include "icamera.hpp"
#include <cstdint>
#include <chrono>
#include <ctime>
/**
 * @brief mipicamera暂时性解决方案
 * @code {.cpp}
 *  m_camera.iic.Open(1);
 *  m_camera.selfTest();
 *  m_camera.init();
 * @endcode
 * @deprecated 暂时性解决方案，已废弃
 *
 */
class camera_t : public icamera {
public:
    typedef enum {
        video_30fps,
        video_60fps,
        no_mipi
    } video_fps;

    typedef enum {
        Normal,
        Inverted
    } Operation;

    typedef enum {
        Inverted_h,
        Inverted_v,
        Inverted_hv
    } Inverted_x;

public:
    camera_t() = default;
    ~camera_t();

    /**
     * @brief
     *
     * @param dev /dev/mipicam?0 ?x指定第几个i2c总线设备
     * @param dev /dev/mipicam 自动扫描i2c总线设备
     */
    virtual void open(const char *dev = "/dev/mipicam") override {
        if (strncmp(dev, "/dev/mipicam", 12) != 0) {
            fprintf(stderr, "mipi camera only support /dev/mipicam device, but got %s\n", dev);
            return;
        }
        bool enum_i2c_mode = strcmp(dev, "/dev/mipicam") == 0;
        int nums = i2c_helper::GetBusNums();
        int dev_num = -1;
        auto detict_i2c_device = [&](int i2c_bus) {
            i2c_helper iicx;
            iicx.Open(i2c_bus);
            if (iicx.IsOpen()) {
                uint8_t val_[3];
                val_[0] = 0x30;
                val_[1] = 0x0;
                val_[2] = 1U;
                if (0 == iicx.Write(dev_addr, val_)) {
                    unsigned char buff[1];
                    buff[0] = 0;
                    iicx.Read(dev_addr, *(uint16_t *)val_, buff);
                    if (buff[0] == val_[2]) {
                        return true;
                    }
                }
            }
            return false;
        };
        if (enum_i2c_mode) {
            for (int i = 0; i < nums; i++) {
                if (detict_i2c_device(i)) {
                    dev_num = i;
                    break;
                }
            }
            if (dev_num < 0) {
                fprintf(stderr, "cannot find mipicamera on any i2c bus (scanned %d buses)\r\n", nums);
            }
        } else if (strlen(dev) >= strlen("/dev/mipicam?0")) {
            const char *num_str = dev + strlen("/dev/mipicam");
            if (*num_str == '?') {
                dev_num = atoi(num_str + 1);
            } else {
                fprintf(stderr, "Invalid device format: %s (expected /dev/mipicam?X)\n", dev);
                return;
            }
            if (!detict_i2c_device(dev_num)) {
                fprintf(stderr, "cannot find mipicamera at i2c bus %d\r\n", dev_num);
                dev_num = -1;
            }
        }
        if (dev_num < 0) {
            fprintf(stderr, "unrecognized mipicamera device %s\r\n", dev);
            return;
        } else {
            printf("Found mipicamera at i2c bus %d\r\n", dev_num);
        }
        iic.Open(dev_num);
        if (!iic.IsOpen()) return;

        try {
            selfTest();
            std::string x(dev);
            video_fps fps = video_30fps;
            Operation sensor_opt = Normal;
            Inverted_x sensor_inv = Inverted_h;
            if (x.find("60fps") != std::string::npos) fps = video_60fps;
            if (x.find("inverted") != std::string::npos) sensor_opt = Inverted;
            if (x.find("hv") != std::string::npos)
                sensor_inv = Inverted_hv;
            else if (x.find("v") != std::string::npos)
                sensor_inv = Inverted_v;
            else
                sensor_inv = Inverted_h;

            init(fps, sensor_opt, sensor_inv);
        } catch (const std::runtime_error &e) {
            std::cerr << e.what() << std::endl;
            fd = 0;
            return;
        }
    }
    virtual bool is_open() override { return fd > 0; }
    virtual int width() override { return 1920; }
    virtual int height() override { return 1080; }
    virtual int channel() override { return 4; }
    virtual size_t buffer_size() override { return width() * height() * channel(); }

    void selfTest();

    int init(video_fps fps = video_30fps, Operation sensor_opt = Normal, Inverted_x sensor_inv = Inverted_h);

    void SubmitAndStart(uint64_t dma_rx_buf, void *rx_buf = nullptr) override;

    int WaitFinish() override;

    

    int AutoExposure(const int (&histogram)[256]) override;

    int SetExposure(float exp_in_ms, uint16_t gain = 0x00);

public:
    i2c_helper iic;

private:
    static constexpr uint8_t dev_addr = 0x1a;
    int OpenDevice();
    void configReg(uint16_t CfgAddr, uint8_t CfgValue);
    // 记录初始化开始时间
    std::chrono::high_resolution_clock::time_point init_start_time;
    float max_line = 2250;
    float line_1H_us = 29.63;

    // 自动曝光
    float m_exp_ms = 15.0;
    int m_exp_status = 0; // 1: 需要上调, -1: 需要下调, 0: suspend
    uint16_t m_gain = 0;

    int fd = 0;
    void *m_map_addr = nullptr;
    uint32_t m_map_size = 0;
    char *m_map_data_start = nullptr; // = m_map_addr+offset
};
