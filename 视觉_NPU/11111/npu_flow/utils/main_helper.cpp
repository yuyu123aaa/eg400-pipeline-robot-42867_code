/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/
#include <fstream>
#include <unistd.h>
#include "main_helper.hpp"
#include "JW_Encoder.h"
#include "json.hpp"
#include "logger_forward.hpp"

static nlohmann::json find_json_value(nlohmann::json data, std::string key) {
    if (data.contains(key)) {
        return data[key];
    } else {
        LOG_ERROR("fail to find key(\"{}\") in user config json file", key);
        throw std::runtime_error("fail to find key in user config json file");
    }
}

user_config paser_user_cfg(std::string file) {
    user_config ret;
    using json = nlohmann::json;
    std::ifstream f(file);
    json data = json::parse(f);
    json input_params = find_json_value(data, "input_params");
    ret.mean[0] = find_json_value(input_params, "r_mean");
    ret.mean[1] = find_json_value(input_params, "g_mean");
    ret.mean[2] = find_json_value(input_params, "b_mean");
    ret.scale[0] = find_json_value(input_params, "r_scale");
    ret.scale[1] = find_json_value(input_params, "g_scale");
    ret.scale[2] = find_json_value(input_params, "b_scale");

    if (data.contains("yolo_params")) {
        json yolo_params = find_json_value(data, "yolo_params");
        ret.nmsThreshold = find_json_value(yolo_params, "nmsThreshold");
        LOG_DEBUG("nmsThreshold={}", ret.nmsThreshold);
        ret.probThreshold = find_json_value(yolo_params, "probThreshold");
        LOG_DEBUG("probThreshold={}", ret.probThreshold);
        json className = find_json_value(yolo_params, "className");
        for (auto i : className.items()) {
            LOG_TRACE("{},{}", i.key(), i.value());
            ret.className.push_back(i.value());
        }
        ret.classNum = ret.className.size();
        if (ret.classNum < 1) {
            LOG_ERROR("class number for yolo is less than 1");
            throw std::runtime_error("class number for yolo is less than 1");
        }
        LOG_DEBUG("classNum={}", ret.classNum);

        for (auto i : yolo_params["strideAnchorMap"].items()) {
            int key = i.value()["key"];
            LOG_TRACE("key = {}", key);
            for (auto j : i.value()["val"].items()) {
                ret.strideAnchorMap[key].push_back(j.value());
                LOG_TRACE("val={}", int(j.value()));
            }
        }

        if (ret.strideAnchorMap.size() < 1) {
            LOG_WARN("you haven't specify strideAnchorMap in user config json file. if you are using anchor free network, ignore this warning");
        }
    }

    return ret;
}

camera_config paser_camera_cfg(std::string file) {
    camera_config camera_obj;
    using json = nlohmann::json;
    std::ifstream f(file);
    json config = json::parse(f);
    json size_info = config["size_params"];
    camera_obj.W = size_info["W"];
    camera_obj.H = size_info["H"];
    cv::Mat intrinsic = cv::Mat::zeros(3, 3, CV_64F);
    cv::Mat extrinsic = cv::Mat::zeros(4, 4, CV_64F);
    cv::Mat distortion = cv::Mat::zeros(5, 1, CV_64F);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            intrinsic.at<double>(i, j) = config["Intrinsic Matrix"][i][j];
        }
    }
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            extrinsic.at<double>(i, j) = config["Extrinsic Matrix"][i][j];
        }
    }
    if (!config["Distortion coefficients"].is_null()) {
        for (int i = 0; i < 5; ++i) {
            distortion.at<double>(i, 0) = config["Distortion coefficients"][i][0];
        }
    }

    camera_obj.intrinsic = intrinsic;
    camera_obj.extrinsic = extrinsic;
    camera_obj.distortion = distortion;
    camera_obj.height = config["height"];
    camera_obj.angle = config["angle"];
    return camera_obj;
}

std::vector<std::string> splitstr(const std::string &str, char tag) {
    std::vector<std::string> li;
    std::string subStr;

    // 遍历字符串，同时将i位置的字符放入到子串中，当遇到tag（需要切割的字符时）完成一次切割
    // 遍历结束之后即可得到切割后的字符串数组
    for (size_t i = 0; i < str.length(); i++) {
        if (tag == str[i]) // 完成一次切割
        {
            if (!subStr.empty()) {
                li.push_back(subStr);
                subStr.clear();
            }
        } else // 将i位置的字符放入子串
        {
            subStr.push_back(str[i]);
        }
    }

    if (!subStr.empty()) // 剩余的子串作为最后的子字符串
    {
        li.push_back(subStr);
    }

    return li;
}

std::vector<std::string> loadNames(const std::string &path) {
    // load class names
    std::vector<std::string> classNames;
    std::ifstream infile(path);
    if (infile.good()) {
        std::string line;
        while (getline(infile, line)) {
            if (line.back() == '\r')
                line.pop_back();
            classNames.emplace_back(line);
        }
        infile.close();
    } else {
        std::cerr << "ERROR: Failed to access class name path: " << path << std::endl;
    }

    return classNames;
}
MediaLoader::MediaWriter::~MediaWriter() {
    if (isMjpeg) {
        if (isOpened) {
            JW_EncoderDeInit(&context);
            JW_BuffHandleMemNoCopy_DeInit(&yuvfeed);
            JW_BuffHandleMemNoCopy_DeInit(&bswrite);
            mjpegFile.close();
        }
    } else if (isVideo) {
#if defined(__linux__)
        if (isOpened) {
            videoWriter.release();
        }
#endif
    }
}
void MediaLoader::MediaWriter::operator<<(const cv::Mat &pic) {
    if (fileName.empty()) {
        return;
    }
    if (isMjpeg) {
        if (pic.empty()) {
            return;
        }
        if (!isOpened) {
            mjpegFile.open(fileName);
            if (!mjpegFile.is_open())
                throw std::runtime_error("Error: Cannot open mjpeg file");
            // 初始化编码器
            JW_EncoderContextSetDefault(&context);
            context.encOP.picWidth = pic.cols;
            context.encOP.picHeight = pic.rows;
            context.encOP.sourceFormat = FORMAT_422;
            context.encOP.chromaInterleave = CBCR_SEPARATED;
            context.encOP.jpg12bit = FALSE;
            context.encOP.packedFormat = PACKED_FORMAT_422_YUYV;
            context.encOP.tiledModeEnable = FALSE;
            JW_EncoderContextSetHuffTable(&context);

            // 注册feed
            context.yuvFeed = &yuvfeed.hm.base;
            context.bsWrite = &bswrite.hm.base;

#define CHECK(x)                                             \
    if (!(x)) {                                              \
        std::cerr << "error at " << __LINE__ << std::endl;   \
        throw std::runtime_error("JPU encoder init failed"); \
    }
            CHECK(0 == JW_BuffHandleMemNoCopy_Init(&yuvfeed, pic.rows * pic.cols * pic.channels()));
            // 给jpeg编码预留超足够的缓存，仅限JW_BuffHandleMemNoCopy_t
            CHECK(0 == JW_BuffHandleMemNoCopy_Init(&bswrite, pic.rows * pic.cols > 400 * 400 ? pic.rows * pic.cols : 400 * 400 * 3));
            CHECK(0 == JW_EncoderInit(&context));
            isOpened = true;
        }
        if (pic.channels() != 2) {
            throw std::runtime_error("Error: Not Implemented for Image");
        }
        // 开始压缩的流程
        yuvfeed.hm.base.vtable->ResetWrRdPtr(&yuvfeed.hm.base, TRUE);
        bswrite.hm.base.vtable->ResetWrRdPtr(&bswrite.hm.base, TRUE);
        memcpy(yuvfeed.hm.base.vtable->GetVirtAddr(&yuvfeed.hm.base), pic.data, pic.rows * pic.cols * pic.channels());
        if (TRUE != JW_EncoderAct(&context)) {
            throw std::runtime_error("Error: Cannot encode jpeg");
        }
        auto bitlen = bswrite.hm.base.vtable->GetWriteSize(&bswrite.hm.base);
        mjpegFile.write((const char *)bswrite.jpg_header, bswrite.jpg_header_size); // JW_BuffHandleMemNoCopy_t的jpg_header是头部信息和bitstream是分开的
        mjpegFile.write((const char *)bswrite.hm.base.vtable->GetVirtAddr(&bswrite.hm.base), bitlen);
    } else if (isVideo) {
#if defined(__linux__)
        if (!isOpened) {
            videoWriter.open(fileName, fourcc, fps, pic.size());
            if (!videoWriter.isOpened()) {
                std::cerr << "Error: Cannot open video file " << fileName << std::endl;
                isOpened = false;
            } else {
                isOpened = true;
            }
        }
        if (isOpened)
            videoWriter << pic;
#endif
    } else {
        cv::imwrite(fileName, pic);
    }
}
void MediaLoader::open(const std::string &media) {
    // 判断后缀是不是mjepeg
    if (media.find(".mjpeg") != std::string::npos || media.find(".mjpg") != std::string::npos
        || (jpegASmjpeg && (media.find(".jpg") != std::string::npos || media.find(".jpeg") != std::string::npos))) {
        isMjpeg = true;
        stream_input_ins.open(media, 0, 0);
        if (stream_input_ins.is_open()) {
            isOpened = true;
        } else {
            isOpened = false;
        }
        return; // 旁路到stream_input
    }
    raw_pic = cv::imread(media, cv::IMREAD_COLOR);
    if (raw_pic.empty()) {
#if defined(__linux__)
        cap.open(media);
        if (!cap.isOpened()) {
            std::cerr << "Error: Cannot open video file " << media << std::endl;
            isOpened = false;
        } else {
            isVideo = true;
            isOpened = true;
        }
#endif
    } else {
        isVideo = false;
        isOpened = true;
    }
}
void MediaLoader::operator>>(cv::Mat &pic) {
    if (isMjpeg) {
        if (gift.get_bitfeed().cma.size == 0) {
            gift.emplace_jpeg_bits_from(stream_input_ins); // 导入bit
        }
        while (!(gift.decode_frame_to(buf))) {
            if (stream_input_ins.is_eof()) {
                std::cerr << "Error: Cannot read stream" << std::endl;
                eofbit = true;
                throw std::runtime_error("Cannot read stream");
            }
            gift.emplace_jpeg_bits_from(stream_input_ins);
        }
        cma_mem_sync_for_cpu(buf.cma.phys_addr, buf.cma.size);
        pic = cv::Mat(gift.get_height(), gift.get_width(), CV_8UC2, buf.cma.virt_addr);
        if (stream_input_ins.is_eof()) {
            eofbit = true;
        }
    } else if (isVideo) {
#if defined(__linux__)
        cap >> pic;
        if (pic.empty()) {
            eofbit = true;
        }
#endif
    } else {
        pic = std::move(raw_pic);
        eofbit = true;
    }
}
MediaLoader::MediaWriter MediaLoader::get_writer(const std::string &fileName) {
    if (!isOpened) {
        std::cerr << "Error: Cannot open media file" << std::endl;
        return MediaWriter();
    }
    if (isVideo) {
#if defined(__linux__)
        return MediaWriter(isMjpeg, isVideo, fileName, cap.get(cv::CAP_PROP_FPS));
#endif
    } else {
        return MediaWriter(isMjpeg, isVideo, fileName);
    }
}
