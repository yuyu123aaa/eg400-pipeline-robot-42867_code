#pragma once

#include "armnnALUtils/DataProcess.hpp"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "jpeg_frame.hpp"

// 视觉流水线和 HTTP 线程之间唯一共享的数据区。
// 图像来自 USB 摄像头原生 MJPEG，避免在 CPU 上重复 JPEG 编码。
class web_state {
public:
    void publish(const jpeg_frame &jpeg,
                 int camera_width, int camera_height,
                 int model_width, int model_height,
                 const std::vector<DetectObject> &objects);

    bool latest_frame(jpeg_frame &jpeg, uint64_t &sequence) const;
    std::string status_json() const;
    std::string detections_json() const;

private:
    static std::string make_detections_json(
        uint64_t sequence, int camera_width, int camera_height,
        int model_width, int model_height, double pipeline_fps,
        const std::vector<DetectObject> &objects);

    mutable std::mutex mutex_;
    jpeg_frame jpeg_;
    std::string detections_json_{"{\"ready\":false,\"objects\":[]}"};
    uint64_t sequence_ = 0;
    int camera_width_ = 0;
    int camera_height_ = 0;
    int model_width_ = 0;
    int model_height_ = 0;
    double pipeline_fps_ = 0.0;
    std::chrono::steady_clock::time_point last_publish_{};
};
