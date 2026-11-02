/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#pragma once
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <memory>
#include <type_traits>
#include <unistd.h>

#include <linux/i2c-dev.h>
#include <linux/i2c.h>

class i2c_helper {
public:
    bool Open(int dev) {
        *fd = open((std::string("/dev/i2c-") + std::to_string(dev)).c_str(), O_RDWR);
        return IsOpen();
    }

    template <typename T, size_t N>
    int Write(unsigned char dev_addr, const T (&buf)[N]) {
        struct i2c_rdwr_ioctl_data data;
        struct i2c_msg messages;
        messages.addr = dev_addr;
        messages.flags = 0;
        messages.len = N;
        messages.buf = (unsigned char *)buf;
        data.msgs = &messages;
        data.nmsgs = 1;
        int rtv = ioctl(*fd, I2C_RDWR, &data) == 0;
        (void)rtv;
        return rtv;
    }

    // template <typename T_REG, typename T, size_t N>
    // int Write(unsigned char dev_addr, T_REG reg, const T (&buf)[N]) {
    //     static_assert(
    //         std::is_same_v<unsigned char, T_REG>
    //             || std::is_same_v<uint16_t, T_REG>,
    //         "no support");
    //     struct i2c_rdwr_ioctl_data data;
    //     struct i2c_msg messages[2];
    //     messages[0].addr = dev_addr;
    //     messages[0].flags = 0;
    //     messages[0].len = sizeof(reg);
    //     messages[0].buf = reinterpret_cast<uint8_t *>(&reg);
    //     messages[1].addr = dev_addr;
    //     messages[1].flags = 0;
    //     messages[1].len = N;
    //     messages[1].buf = (unsigned char *)buf;
    //     data.msgs = messages;
    //     data.nmsgs = 2;
    //     int rtv = ioctl(*fd, I2C_RDWR, &data) == 0;
    //     (void)rtv;
    //     return rtv;
    // }

    template <typename T_REG, typename T, size_t N>
    int Read(unsigned char dev_addr, T_REG reg, T (&buf)[N]) {
        static_assert(
            std::is_same_v<unsigned char, T_REG>
                || std::is_same_v<uint16_t, T_REG>,
            "no support");
        struct i2c_rdwr_ioctl_data data;
        struct i2c_msg messages[2];
        messages[0].addr = dev_addr;
        messages[0].flags = 0;
        messages[0].len = sizeof(reg);
        messages[0].buf = reinterpret_cast<uint8_t *>(&reg);
        messages[1].addr = dev_addr;
        messages[1].flags = I2C_M_RD;
        messages[1].len = N;
        messages[1].buf = (unsigned char *)buf;
        data.msgs = messages;
        data.nmsgs = 2;
        int rtv = ioctl(*fd, I2C_RDWR, &data) == 0;
        (void)rtv;
        return rtv;
    }

    bool IsOpen() { return *fd > 0; }

private:
    std::shared_ptr<int> fd{
        new int(-1), [this](int *p) {
            (void)this;
            if (*p > 0)
                close(*p);
            delete p;
        }};

    // i2c操作的一些公共函数
public:
    static int GetBusNums() {
        int i = 0;
        // 判断/dev/i2c-是否存在
        while (access((std::string("/dev/i2c-") + std::to_string(i)).c_str(), F_OK) == 0) {
            i++;
        }
        return i;
    }
};
