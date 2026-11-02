/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#include "mipi_camera.hpp"
#include "logger_forward.hpp"
#include "mipicam_module.h"
#include <cmath>
#include <cstring>
#include <thread>
#include <stdexcept>
#define CAMERA_BASE_ADDR 0x000
#define CAMERA_START 0x004
#define CAMERA_CLEAR_INTR 0x008

#define try_throw(expr, str)           \
    if (!(expr)) {                     \
        throw std::runtime_error(str); \
    }

void camera_t::selfTest() {
    try_throw(iic.IsOpen(), "i2c not open");

    unsigned char buff[16];
    memset(buff, 0, sizeof(buff));
    auto err_code = iic.Read(dev_addr, (uint16_t)0x30, buff);

    try_throw(err_code == 0, "camera.iic.Read return error code = " + std::to_string(err_code));

    // bool has_value = false;
    // for (int i = 0; i < 16; i++) {
    //     if (buff[i] != 0) {
    //         has_value = true;
    //     }
    // }
    // try_throw(has_value, "camera check failed");
}

int camera_t::init(video_fps fps, Operation sensor_opt, Inverted_x sensor_inv) {
    // 记录初始化开始时间
    	init_start_time = std::chrono::high_resolution_clock::now();
    /* Common Config */
        printf("Mipi Camera Config Start\r\n");
        configReg(0x3000, 0x01);  // STANDBY
        configReg(0x3001, 0x00);  // REGHOLD
        configReg(0x3002, 0x01);  // XMSTA
        configReg(0x3003, 0x00);  // XMASTER
        configReg(0x3008, 0x5D);  // BCWAIT_TIME[9:0]
        configReg(0x3009, 0x00);  // BCWAIT_TIME[9:0]
        configReg(0x300A, 0x42);  // CPWAIT_TIME[9:0]
        configReg(0x300B, 0xA0);  // CPWAIT_TIME[9:0]
        configReg(0x301C, 0x00);  // WINMODE[3:0]
        configReg(0x301D, 0x08);  // -
        configReg(0x3020, 0x01);  // HADD
        configReg(0x3021, 0x01);  // VADD
        configReg(0x3022, 0x01);  // ADDMODE[1:0]
        configReg(0x3023, 0x01);  // VCMODE
        configReg(0x3024, 0xEB);  // VMAX[19:0]
        configReg(0x3025, 0x08);  // VMAX[19:0]
        configReg(0x3026, 0x00);  // VMAX[19:0]
        configReg(0x3028, 0x1E);  // HMAX[15:0]
        configReg(0x3029, 0x02);  // HMAX[15:0]
        configReg(0x302C, 0x00);  // WDMODE[1:0]
        configReg(0x302D, 0x00);  // WDSEL[1:0]
        configReg(0x3030, 0x00);  // HREVERSE
        configReg(0x3031, 0x00);  // ADBIT[1:0]
        configReg(0x3032, 0x00);  // MDBIT
        configReg(0x3033, 0x05);  // SYS_MODE[3:0]
        configReg(0x3040, 0x00);  // PIX_HST[12:0]
        configReg(0x3041, 0x00);  // PIX_HST[12:0]
        configReg(0x3042, 0x18);  // PIX_HWIDTH[12:0]
        configReg(0x3043, 0x0F);  // PIX_HWIDTH[12:0]
        configReg(0x3044, 0x00);  // PIX_VST[12:0]
        configReg(0x3045, 0x00);  // PIX_VST[12:0]
        configReg(0x3046, 0x20);  // PIX_VWIDTH[12:0]
        configReg(0x3047, 0x11);  // PIX_VWIDTH[12:0]
        configReg(0x3050, 0x08);  // SHR0[19:0]
        configReg(0x3051, 0x00);  // SHR0[19:0]
        configReg(0x3052, 0x00);  // SHR0[19:0]
        configReg(0x3054, 0x19);  // SHR1[19:0]
        configReg(0x3055, 0x00);  // SHR1[19:0]
        configReg(0x3056, 0x00);  // SHR1[19:0]
        configReg(0x3058, 0x3E);  // SHR2[19:0]
        configReg(0x3059, 0x00);  // SHR2[19:0]
        configReg(0x305A, 0x00);  // SHR2[19:0]
        configReg(0x305C, 0x66);  // SHR3[19:0]
        configReg(0x305D, 0x00);  // SHR3[19:0]
        configReg(0x305E, 0x00);  // SHR3[19:0]
        configReg(0x3060, 0x25);  // RHS1[19:0]
        configReg(0x3061, 0x00);  // RHS1[19:0]
        configReg(0x3062, 0x00);  // RHS1[19:0]
        configReg(0x3064, 0x4A);  // RHS2[19:0]
        configReg(0x3065, 0x00);  // RHS2[19:0]
        configReg(0x3066, 0x00);  // RHS2[19:0]
        configReg(0x3090, 0x32);  // GAIN_PGC_0[8:0]
        configReg(0x3091, 0x00);  // GAIN_PGC_0[8:0]
        configReg(0x3092, 0x32);  // GAIN_PGC_1[8:0]
        configReg(0x3093, 0x00);  // GAIN_PGC_1[8:0]
        configReg(0x3094, 0x32);  // GAIN_PGC_2[8:0]
        configReg(0x3095, 0x00);  // GAIN_PGC_2[8:0]
        configReg(0x3096, 0x32);  // GAIN_PGC_3[8:0]
        configReg(0x3097, 0x00);  // GAIN_PGC_3[8:0]
        configReg(0x30C0, 0x2A);  // XVSOUTSEL[1:0]
        configReg(0x30C1, 0x00);  // XVS_DRV[1:0]
        configReg(0x30CC, 0x00);  // -
        configReg(0x30CD, 0x00);  // -
        configReg(0x30CF, 0x00);  // XVSMSKCNT_INT[1:0]
        configReg(0x30D9, 0x02);  // DIG_CLP_VSTART[4:0]
        configReg(0x30DA, 0x01);  // DIG_CLP_VNUM[1:0]
        configReg(0x30E2, 0x32);  // BLKELVEL[9:0]
        configReg(0x30E3, 0x00);  // BLKELVEL[9:0]
        configReg(0x3115, 0x00);  // INCKSEL1[7:0]
        configReg(0x3116, 0x23);  // INCKSEL2[7:0]
        configReg(0x3118, 0xC6);  // INCKSEL3[10:0]
        configReg(0x3119, 0x00);  // INCKSEL3[10:0]
        configReg(0x311A, 0xE7);  // INCKSEL4[10:0]
        configReg(0x311B, 0x00);  // INCKSEL4[10:0]
        configReg(0x311E, 0x23);  // INCKSEL5[7:0]
        configReg(0x3260, 0x01);  // GAIN_PGC_FIDMD
        configReg(0x32C8, 0x01);  // -
        configReg(0x32D4, 0x21);  // -
        configReg(0x32EC, 0xA1);  // -
        configReg(0x344C, 0x2B);  // -
        configReg(0x344D, 0x01);  // -
        configReg(0x344E, 0xED);  // -
        configReg(0x344F, 0x01);  // -
        configReg(0x3450, 0xF6);  // -
        configReg(0x3451, 0x02);  // -
        configReg(0x3452, 0x7F);  // -
        configReg(0x3453, 0x03);  // -
        configReg(0x358A, 0x04);  // -
        configReg(0x35A1, 0x02);  // -
        configReg(0x35EC, 0x27);  // -
        configReg(0x35EE, 0x8D);  // -
        configReg(0x35F0, 0x8D);  // -
        configReg(0x35F2, 0x29);  // -
        configReg(0x36BC, 0x0C);  // -
        configReg(0x36CC, 0x53);  // -
        configReg(0x36CD, 0x00);  // -
        configReg(0x36CE, 0x3C);  // -
        configReg(0x36D0, 0x8C);  // -
        configReg(0x36D1, 0x00);  // -
        configReg(0x36D2, 0x71);  // -
        configReg(0x36D4, 0x3C);  // -
        configReg(0x36D6, 0x53);  // -
        configReg(0x36D7, 0x00);  // -
        configReg(0x36D8, 0x71);  // -
        configReg(0x36DA, 0x8C);  // -
        configReg(0x36DB, 0x00);  // -
        configReg(0x3701, 0x00);  // ADBIT1[7:0]
        configReg(0x3720, 0x00);  // -
        configReg(0x3724, 0x02);  // -
        configReg(0x3726, 0x02);  // -
        configReg(0x3732, 0x02);  // -
        configReg(0x3734, 0x03);  // -
        configReg(0x3736, 0x03);  // -
        configReg(0x3742, 0x03);  // -
        configReg(0x3862, 0xE0);  // -
        configReg(0x38CC, 0x30);  // -
        configReg(0x38CD, 0x2F);  // -
        configReg(0x395C, 0x0C);  // -
        configReg(0x39A4, 0x07);  // -
        configReg(0x39A8, 0x32);  // -
        configReg(0x39AA, 0x32);  // -
        configReg(0x39AC, 0x32);  // -
        configReg(0x39AE, 0x32);  // -
        configReg(0x39B0, 0x32);  // -
        configReg(0x39B2, 0x2F);  // -
        configReg(0x39B4, 0x2D);  // -
        configReg(0x39B6, 0x28);  // -
        configReg(0x39B8, 0x30);  // -
        configReg(0x39BA, 0x30);  // -
        configReg(0x39BC, 0x30);  // -
        configReg(0x39BE, 0x30);  // -
        configReg(0x39C0, 0x30);  // -
        configReg(0x39C2, 0x2E);  // -
        configReg(0x39C4, 0x2B);  // -
        configReg(0x39C6, 0x25);  // -
        configReg(0x3A42, 0xD1);  // -
        configReg(0x3A4C, 0x77);  // -
        configReg(0x3AE0, 0x02);  // -
        configReg(0x3AEC, 0x0C);  // -
        configReg(0x3B00, 0x2E);  // -
        configReg(0x3B06, 0x29);  // -
        configReg(0x3B98, 0x25);  // -
        configReg(0x3B99, 0x21);  // -
        configReg(0x3B9B, 0x13);  // -
        configReg(0x3B9C, 0x13);  // -
        configReg(0x3B9D, 0x13);  // -
        configReg(0x3B9E, 0x13);  // -
        configReg(0x3BA1, 0x00);  // -
        configReg(0x3BA2, 0x06);  // -
        configReg(0x3BA3, 0x0B);  // -
        configReg(0x3BA4, 0x10);  // -
        configReg(0x3BA5, 0x14);  // -
        configReg(0x3BA6, 0x18);  // -
        configReg(0x3BA7, 0x1A);  // -
        configReg(0x3BA8, 0x1A);  // -
        configReg(0x3BA9, 0x1A);  // -
        configReg(0x3BAC, 0xED);  // -
        configReg(0x3BAD, 0x01);  // -
        configReg(0x3BAE, 0xF6);  // -
        configReg(0x3BAF, 0x02);  // -
        configReg(0x3BB0, 0xA2);  // -
        configReg(0x3BB1, 0x03);  // -
        configReg(0x3BB2, 0xE0);  // -
        configReg(0x3BB3, 0x03);  // -
        configReg(0x3BB4, 0xE0);  // -
        configReg(0x3BB5, 0x03);  // -
        configReg(0x3BB6, 0xE0);  // -
        configReg(0x3BB7, 0x03);  // -
        configReg(0x3BB8, 0xE0);  // -
        configReg(0x3BBA, 0xE0);  // -
        configReg(0x3BBC, 0xDA);  // -
        configReg(0x3BBE, 0x88);  // -
        configReg(0x3BC0, 0x44);  // -
        configReg(0x3BC2, 0x7B);  // -
        configReg(0x3BC4, 0xA2);  // -
        configReg(0x3BC8, 0xBD);  // -
        configReg(0x3BCA, 0xBD);  // -
        configReg(0x4000, 0x10);  // -
        configReg(0x4001, 0x03);  // LANEMODE[2:0]
        configReg(0x4004, 0xC0);  // TXCLKESC_FREQ[15:0]
        configReg(0x4005, 0x06);  // TXCLKESC_FREQ[15:0]
        configReg(0x400C, 0x00);  // INCKSEL6
        configReg(0x4018, 0x7F);  // TCLKPOST[15:0]
        configReg(0x4019, 0x00);  // TCLKPOST[15:0]
        configReg(0x401A, 0x37);  // TCLKPREPARE[15:0]
        configReg(0x401B, 0x00);  // TCLKPREPARE[15:0]
        configReg(0x401C, 0x37);  // TCLKTRAIL[15:0]
        configReg(0x401D, 0x00);  // TCLKTRAIL[15:0]
        configReg(0x401E, 0xF7);  // TCLKZERO[15:0]
        configReg(0x401F, 0x00);  // TCLKZERO[15:0]
        configReg(0x4020, 0x3F);  // THSPREPARE[15:0]
        configReg(0x4021, 0x00);  // THSPREPARE[15:0]
        configReg(0x4022, 0x6F);  // THSZERO[15:0]
        configReg(0x4023, 0x00);  // THSZERO[15:0]
        configReg(0x4024, 0x3F);  // THSTRAIL[15:0]
        configReg(0x4025, 0x00);  // THSTRAIL[15:0]
        configReg(0x4026, 0x5F);  // THSEXIT[15:0]
        configReg(0x4027, 0x00);  // THSEXIT[15:0]
        configReg(0x4028, 0x2F);  // TLPX[15:0]
        configReg(0x4029, 0x00);  // TLPX[15:0]
        configReg(0x4074, 0x01);  // INCKSEL7[2:0]
        configReg(0x3000, 0x00); // Standby Cancel
        std::this_thread::sleep_for(std::chrono::milliseconds(18));
        configReg(0x3002, 0x00);
        std::this_thread::sleep_for(std::chrono::milliseconds(320));
        printf("Mipi Camera Config End\r\n");
        printf("-----------------------------------------------\r\n");
        printf("    ReadoutMode   :     H/V 2/2-line binning   \r\n");
    
        auto init_end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            init_end_time - init_start_time).count();
        double elapsed_s = elapsed_ms / 1000.0;
    
        printf("    Elapsed time  :     %.3lfs                 \r\n", elapsed_s);
        printf("-----------------------------------------------\r\n");

    return OpenDevice();
}

int camera_t::OpenDevice() {
    fd = ::open("/dev/mipicam", O_RDWR);
    if (fd <= 0) {
        LOG_ERROR("Open /dev/mipicam Failed");
        return -1;
    }

    mipicam_reg_info_t reg_info;
    int err = ioctl(fd, MIPICAM_IOCTL_GET_REGISTER_INFO, &reg_info);
    if (err != 0) {
        close(fd);
        fd = 0;
        LOG_ERROR("MIPICAM_IOCTL_GET_REGISTER_INFO failed");
        return -1;
    }

    m_map_addr = mmap(NULL, reg_info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, __off_t(reg_info.addr));
    m_map_size = reg_info.size;
    if (m_map_addr == MAP_FAILED) {
        close(fd);
        fd = 0;
        LOG_ERROR("mmap failed");
        return -1;
    }

    // 处理偏移
    // 这里要hardcode一下支持老设备
    uint32_t offset = reg_info.addr & ((1 << 12) - 1);
    if (reg_info.addr == 0x80100000 && reg_info.size == 0x00010000) {
        offset = 0xC00;
    }
    m_map_data_start = (char *)m_map_addr;
    m_map_data_start += offset;

    return 0;
}
void camera_t::configReg(uint16_t CfgAddr, uint8_t CfgValue) {
    uint8_t val_[3];
    val_[0] = (CfgAddr >> 8) & 0xFF;
    val_[1] = CfgAddr & 0xFF;
    val_[2] = CfgValue;
    try_throw(0 == iic.Write(dev_addr, val_), "i2c write failed");

    unsigned char buff[1];
    buff[0] = 0;
    iic.Read(dev_addr, *(uint16_t *)val_, buff);
    try_throw(buff[0] == val_[2], "i2c read-back failed");
}

int camera_t::WaitFinish() {
    mipicam_intr_info_t intr;
    intr.intr_flag = 0; // For Worst
    intr.timeout = 1000;
    int err = ioctl(fd, MIPICAM_IOCTL_WAIT_INTERRUPT, &intr);
    if (err) {
        printf("MIPICAM_IOCTL_WAIT_INTERRUPT failed\r\n");
        return -1;
    }
    return 0;
}

int camera_t::SetExposure(float exp_in_ms, uint16_t gain) {
    auto expf = max_line - (std::abs(exp_in_ms) * 1000.f / line_1H_us);
    if (expf < 5) expf = 5;
    uint16_t exp = expf;
    configReg(0x3050, exp);
    configReg(0x3051, exp >> 8);

    configReg(0x3090, gain); // gain
    configReg(0x3091, gain >> 8);
    return 0;
}

int camera_t::AutoExposure(const int (&histogram)[256]) {
    if (0) {
        int m_histogramIntegral[256];

        m_histogramIntegral[0] = histogram[0];
        for (int i = 1; i < 256; i++) {
            m_histogramIntegral[i] = m_histogramIntegral[i - 1] + histogram[i];
        }
        int total = m_histogramIntegral[255];
        int mid = total / 2;
        int mid_pos = 0;

        for (int i = 0; i < 256; i++) {
            if (m_histogramIntegral[i] >= mid) {
                mid_pos = i;
                break;
            }
        }

        if (m_exp_status == 0) {
            if (mid_pos < 72)
                m_exp_status = 1;
            else if (mid_pos > 256 - 72)
                m_exp_status = -1;
        } else if (m_exp_status == 1) {
            if (mid_pos > 90)
                m_exp_status = 0;
        } else {
            if (mid_pos < 256 - 90)
                m_exp_status = 0;
        }
    } else {
        /*
        Skewness Calculation in reference to:
        Zwillinger, D. and Kokoska, S. (2000). CRC Standard Probability and Statistics
        Tables and Formulae. Chapman & Hall: New York. 2000. Section 2.2.24.1
        */
        double m_2 = 0, m_3 = 0;
        int nums = 0;
        double v = -120.; // 90 是调整的常量
        for (int i = 0; i < 256; i++, v += 1.) {
            auto val = v;
            auto val2 = val * val;
            auto val3 = val2 * val;
            nums += histogram[i];
            m_2 += val2 * histogram[i];
            m_3 += val3 * histogram[i];
        }
        double dnums = nums;

        m_2 /= dnums;
        m_3 /= dnums;
        double g_1 = std::sqrt(dnums * (dnums - 1)) / (dnums - 2);
        double skewness = g_1 * m_3 / std::pow(m_2, 1.5);
        constexpr double threshold = 0.45;
        if (skewness > threshold) {
            m_exp_status = -1;
        } else if (skewness < -threshold) {
            m_exp_status = 1;
        } else {
            m_exp_status = 0;
        }
    }
    // 0.035 = 10^(0.3/20) - 1 ,相当于每次调整0.3db
    float delta = 0.035f * m_exp_ms;

    m_exp_ms += delta * m_exp_status;
    if (m_exp_ms <= 0) m_exp_ms = 0.001;
    if (m_exp_ms > 66.0) {
        m_exp_ms = 66.0;
        if (m_gain < 0xF0) m_gain++;
    } else if (m_exp_ms < 66.0) {
        if (m_gain > 0) {
            m_gain--;
            m_exp_ms = 66.0;
        }
    }
    SetExposure(m_exp_ms, m_gain);
    return 0;
}
camera_t::~camera_t() {
    if (fd > 0) {
        munmap(m_map_addr, m_map_size);
        m_map_addr = nullptr;
        m_map_data_start = nullptr;
        close(fd);
    }
    fd = 0;
}
void camera_t::SubmitAndStart(uint64_t dma_rx_buf, void *rx_buf) {
    *(uint32_t *)(m_map_data_start + CAMERA_BASE_ADDR) = dma_rx_buf;
    *(uint32_t *)(m_map_data_start + CAMERA_START) = 0x1;
}
