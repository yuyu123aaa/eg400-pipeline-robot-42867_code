#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "icamera.hpp"
#include "jpeg_frame.hpp"

/**
 * @brief 对usb相机、mjpeg视频的包装，接口类似于mipi_camera，输出结果经过了JPU解码
 * 所以格式是YUYV
 *
 */
class usb_camera_t : public icamera {
public:
    usb_camera_t(usb_camera_t &&) = delete;
    usb_camera_t &operator=(usb_camera_t &&) = delete;
    usb_camera_t(const usb_camera_t &) = delete;
    usb_camera_t &operator=(const usb_camera_t &) = delete;

    usb_camera_t();
    usb_camera_t(int w, int h);
    ~usb_camera_t();

public:
    void selfTest() { return; }

    /**
     * @brief 初始化相机或者是mjpeg视频源，只支持1080p哦，只输出yuv422
     *
     * @param dev_path 相机路径（例如/dev/videoX）或者是mjpeg视频路径
     * @return int 错误代码，0表示成功
     */
    int init(const char *dev_path);

    /**
     * @brief 只输出yuv422
     *  错误代码请调用WaitFinish()获取
     *
     * @param dma_rx_buf frame 物理地址
     * @param rx_buf 缓存虚拟地址
     */
    void SubmitAndStart(uint64_t dma_rx_buf, void *rx_buf = nullptr) override;

    // 返回最近一次送入板载 JPU 的原始 MJPEG 帧；供网页直接转发。
    jpeg_frame GetJpegStream();
    void SaveJpegStream(std::ostream &fs);
    /**
     * @brief 等待一帧传输完成
     *
     * @return 0 表示成功
     * @return 非0 失败发生错误
     */
    int WaitFinish() override { return error_code; }

    int SetExposure(float exp_in_ms) { return 0; }

    // 判断输入流是否是usb相机
    bool is_dev_video() const;

    /**
     * @brief 打开设备
     *
     * @param dev 传入usb 相机设备号字符串，例如"/dev/video0" 或者媒体文件 jpg mjpeg
     */
    virtual void open(const char *dev) override {
        open_err_code = init(dev);
    }
    /**
     * @brief 设备是否打开
     *
     * @return true 已打开
     * @return false 未打开
     */
    virtual bool is_open() override { return open_err_code == 0; }
    /**
     * @brief 获取摄像头分辨率
     *
     * @return int
     */
    virtual int width() override { return m_width; }
    /**
     * @brief 获取摄像头分辨率
     *
     * @return int
     */
    virtual int height() override { return m_height; }
    /**
     * @brief 每像素多少bytes
     *
     * @return int
     */
    virtual int channel() override { return 2; }
    /**
     * @brief 一帧缓存大小，单位bytes
     *
     * @return size_t
     */
    virtual size_t buffer_size() override { return m_width * m_heightPad16 * 2; }

private:
    struct usb_camera_impl;
    friend struct usb_camera_impl;
    std::shared_ptr<usb_camera_impl> impl;
    int error_code = 0;
    int open_err_code = -1;
    int m_width = 1920;
    int m_height = 1080;

    int m_widthPad16 = 1920;
    int m_heightPad16 = 1088;
    mutable std::mutex m_jpeg_mutex;
    jpeg_frame m_latest_jpeg;
};
