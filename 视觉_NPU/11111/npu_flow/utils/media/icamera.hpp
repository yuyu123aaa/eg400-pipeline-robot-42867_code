#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

class icamera {
public:
    icamera() = default;
    virtual ~icamera() = default;

    icamera(icamera &&) = delete;
    icamera &operator=(icamera &&) = delete;
    icamera(const icamera &) = delete;
    icamera &operator=(const icamera &) = delete;
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
     * @brief 获取摄像头分辨率
     *
     * @return int
     */
    virtual int width() = 0;
    /**
     * @brief 获取摄像头分辨率
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
     * @brief 提交一帧传输
     *
     * @param dma_rx_buf 缓存物理地址
     * @param rx_buf 缓存虚拟地址
     */
    virtual void SubmitAndStart(uint64_t dma_rx_buf, void *rx_buf) = 0;
    /**
     * @brief 等待一帧传输完成
     *
     * @return 0 表示成功
     * @return 非0 失败发生错误
     */
    virtual int WaitFinish() = 0;

    /**
     * @brief 自动曝光，通过直方图计算正确的曝光
     * @deprecated 不再使用
     * @param histogram 直方图
     * @return int
     */
    virtual int AutoExposure(const int (&histogram)[256]) {
        return 0;
    }

    // 当前前缓存在外部申请，之后可能由camera 或者display 申请

    /**
     * @brief 枚举设备
     *
     * @param dev 设备路径
     * @param resolution 分辨率
     * @return std::shared_ptr<icamera> nullptr表示没有可用设备
     */
    static std::shared_ptr<icamera> enumerate_device(const char *dev, const char *resolution = "1920x1080");
};