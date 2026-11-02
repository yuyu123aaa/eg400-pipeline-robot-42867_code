/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#pragma once
#ifdef ENABLE_LIBDRM_HDMI
#include <cstdint>
#include <memory>
#include "ihdmi.hpp"

/**
 * @brief 使用libdrm控制的hdmi输出，支持设置指定分辨率
 */
class hdmi_drm_t : public ihdmi {
public:
    /**
     * @brief Construct a new hdmi drm t object
     *
     */
    hdmi_drm_t();
    /**
     * @brief Construct a new hdmi drm t object
     * @param w 屏幕宽
     * @param h 屏幕高
     */
    hdmi_drm_t(int w, int h);
    ~hdmi_drm_t() = default;
    hdmi_drm_t(hdmi_drm_t &&) = delete;
    hdmi_drm_t &operator=(hdmi_drm_t &&) = delete;
    hdmi_drm_t(const hdmi_drm_t &) = delete;
    hdmi_drm_t &operator=(const hdmi_drm_t &) = delete;

    /**
     * @brief 打开设备
     *
     * @param dev 简单缓存drm的设备，例如"/dev/dri/card0"
     */
    virtual void open(const char *dev = "/dev/dri/card0") override;
    /**
     * @brief 设备是否打开
     *
     * @return true 已打开
     * @return false 未打开
     */
    virtual bool is_open() override;
    /**
     * @brief 获取屏幕宽度
     *
     * @return int
     */
    virtual int width() override { return get_width(); }
    /**
     * @brief 获取屏幕高度
     *
     * @return int
     */
    virtual int height() override { return get_height(); }

    /**
     * @brief 每像素多少bytes
     *
     * @return int
     */
    virtual int channel() override {
        return 2; // yuyv, will convert to bgra(4) after show() by cvtColor
    }
    /**
     * @brief 一帧缓存大小，单位bytes
     *
     * @return size_t
     */
    virtual size_t buffer_size() override {
        return width() * height() * channel();
    }
    /**
     * @brief 输出接口，传入缓存虚拟地址或者物理地址
     *
     * @param base_addr 传入缓存的物理地址
     * @param virt_addr 传入缓存的虚拟地址
     * @param pixelformat 像素格式，低位代表每像素多少bytes
     */
    virtual void show(uint64_t base_addr, void *virt_addr, int pixelformat) override;

    /**
     * @brief 获取一帧缓存，缓存被本类管理，不需要外部释放
     * 
     * @return void* 
     */
    void *DeQueueBuff();
    /**
     * @brief 提交一帧被本类管理的缓存，默认双缓冲
     * 
     * @param buff 
     */
    void QueueBuff(void *buff);

    int get_width() const;
    int get_height() const;
    int get_pixel_size() const;
    void init() { open(); }

private:
    struct impl_t;
    static struct impl_t *hdmi_drm_impl_ins;
    std::shared_ptr<impl_t> impl;
};
#endif // ENABLE_LIBDRM_HDMI