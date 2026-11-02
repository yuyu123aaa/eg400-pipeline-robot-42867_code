/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#pragma once
#include <cstddef>
#include <memory>

/**
 * @brief HDMI接口抽象类
 */
class ihdmi {
public:
    ihdmi() = default;
    virtual ~ihdmi() = default;
    ihdmi(ihdmi &&) = delete;
    ihdmi &operator=(ihdmi &&) = delete;
    ihdmi(const ihdmi &) = delete;
    ihdmi &operator=(const ihdmi &) = delete;
    /**
     * @brief 打开设备
     *
     * @param dev 设备路径
     */
    virtual void open(const char *dev) = 0;
    /**
     * @brief 设备是否打开
     *
     * @return true 已打开
     * @return false 未打开
     */
    virtual bool is_open() = 0;
    /**
     * @brief 获取屏幕宽度
     *
     * @return int
     */
    virtual int width() = 0;
    /**
     * @brief 获取屏幕高度
     *
     * @return int
     */
    virtual int height() = 0;
    /**
     * @brief 每像素多少bytes
     *
     * @return int
     */
    virtual int channel() = 0;
    /**
     * @brief 一帧缓存大小，单位bytes
     *
     * @return size_t
     */
    virtual size_t buffer_size() = 0;
    /**
     * @brief 输出接口，传入缓存虚拟地址或者物理地址
     *
     * @param base_addr 传入缓存的物理地址， 0 则表示不使用
     * @param virt_addr 传入缓存的虚拟地址，如果为nullptr则表示不使用
     * @param pixelformat 像素格式，低位代表每像素多少bytes
     */
    virtual void show(uint64_t base_addr, void *virt_addr, int pixelformat) = 0;

    /**
     * @brief 枚举设备
     *
     * @param dev 设备路径
     * @param resolution 分辨率，例如"1920x1080"
     * @return std::shared_ptr<ihdmi> nullptr表示没有可用设备
     */
    static std::shared_ptr<ihdmi> enumerate_device(const char *dev, const char *resolution = "1920x1080");
};