/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/
#pragma once

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>

#include <string>
#include <unordered_map>
#include <vector>
#include <opencv2/opencv.hpp>
#include "stream_input.hpp"

struct app_config {
    std::string model;
    std::string input;
    std::string output;
    std::string usercfg;
    std::string cameracfg;
    std::string class_names; // An Text file contains class names
    bool useResize;
    bool useYolov8 = false; // if model name contain str("yolov8"),it`s auto turn ON
    std::string useGraphviz;
    std::vector<std::string> backends;
    unsigned int logLevel;

    bool camera = true; // true if powered by PL, else using <std::string>input
    bool hdmi = true;   // true if powered by PL, else using <std::string>output
    bool resize = true; //  true if powered by PL, else by CPU
};

struct app_config_c {
    std::string input;
    std::string output;
    std::string cameracfg;
    unsigned int logLevel;

    bool camera = true; // true if powered by PL, else using <std::string>input
    bool hdmi = true;   // true if powered by PL, else using <std::string>output
    bool resize = true; //  true if powered by PL, else by CPU
};

struct user_config {
	// 训练时输入数据的mean, scale配置
	float mean[3];
	float scale[3];	

	// yolo 处理相关配置
    std::unordered_map<int, std::vector<int>> strideAnchorMap;
    size_t classNum;
    std::vector<std::string> className;
    float probThreshold;
    float nmsThreshold;
};

struct camera_config{
    int W;
    int H;
    float height;
    float angle;
    cv::Mat intrinsic;
    cv::Mat extrinsic;
    cv::Mat distortion;
};

user_config paser_user_cfg(std::string file);

camera_config paser_camera_cfg(std::string file);

void run_object_detection(const app_config &cfg, const user_config &config);
void run_classification(const app_config &cfg, const user_config &config);

std::vector<std::string> splitstr(const std::string &str, char tag);

std::vector<std::string> loadNames(const std::string &path);

using namespace std::chrono;
class Profiling {
private:
    bool mProfilingEnabled{};
    steady_clock::time_point mStart{};
    steady_clock::time_point mStop{};

public:
    Profiling() : mProfilingEnabled(false) {};
    explicit Profiling(bool isEnabled) : mProfilingEnabled(isEnabled) {};

    void ProfilingStart()
    {
        if (mProfilingEnabled)
        {
            mStart = steady_clock::now();
        }
    }

    auto ProfilingStop()
    {
        if (mProfilingEnabled)
        {
            mStop = steady_clock::now();
        }
    }

    auto ProfilingGetMs()
    {
        // return mProfilingEnabled ? duration_cast<microseconds>(mStop - mStart).count() : 0;
        return mProfilingEnabled ? duration_cast<milliseconds>(mStop - mStart).count() : 0;
    }

    void ProfilingStopAndPrintMs(const std::string &title)
    {
        ProfilingStop();
        if (mProfilingEnabled) {
            std::cout << "Profiling: " << title << ": " << ProfilingGetMs() << " mSeconds" << std::endl;
        }
    }
};

// 统一图像和视频的加载接口
class MediaLoader {
private:
    bool isOpened = false;
    bool eofbit = false;
    bool isVideo = false;
    bool isMjpeg = false;
    bool jpegASmjpeg = false;
    cv::Mat raw_pic;
    stream_input stream_input_ins;
    jpu_decoder_gift gift;
    jpu_cma_buffer buf;
#if defined(__linux__)
    cv::VideoCapture cap;
#endif

public:
    class MediaWriter {
    private:
        bool isOpened = false;
        bool isMjpeg = false;
        bool isVideo = false;
        std::string fileName;
        double fps = 30;
        std::ofstream mjpegFile;
        JW_EncoderContext_t context;
        JW_BuffHandleMemNoCopy_t yuvfeed, bswrite;
#if defined(__linux__)
        cv::VideoWriter videoWriter;
        int fourcc = cv::VideoWriter::fourcc('X', '2', '6', '4');
#endif

    private:
        friend class MediaLoader;
        MediaWriter() = default;
        MediaWriter(bool isMjpeg, bool isVideo, const std::string &fileName, double fps = 30)
            : isMjpeg(isMjpeg)
            , isVideo(isVideo)
            , fileName(fileName)
            , fps(fps) {
        }

    public:
        void operator<<(const cv::Mat &pic);
        ~MediaWriter();
    };

public:
    MediaLoader(MediaLoader &&) = delete;
    MediaLoader &operator=(MediaLoader &&) = delete;
    MediaLoader(const MediaLoader &) = delete;
    MediaLoader &operator=(const MediaLoader &) = delete;

    MediaLoader() = default;
    MediaLoader(const std::string &media, bool jpegASmjpeg = false)
        : jpegASmjpeg(jpegASmjpeg) {
        open(media);
    }

    void open(const std::string &media);

    bool is_open() const {
        return isOpened;
    }

    bool is_eof() const {
        return eofbit;
    }

    operator bool() const {
        return is_eof();
    }

    void operator>>(cv::Mat &pic);

    MediaWriter get_writer(const std::string &fileName);
};

#if defined(__linux__)
#define DEMO_ENTRY(NAME, ARGN, ARGV) \
    int main(int ARGN, char *ARGV[])
#else
#define DEMO_ENTRY(NAME, ARGN, ARGV)                                                                   \
    int NAME(int ARGN, char *ARGV[]);                                                                  \
    SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN), NAME, NAME, NAME); \
    int NAME(int ARGN, char *ARGV[])
#endif
