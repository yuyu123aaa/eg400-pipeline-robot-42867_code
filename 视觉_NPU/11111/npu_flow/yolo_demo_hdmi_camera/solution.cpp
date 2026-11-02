/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/
#include "solution.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <opencv2/opencv.hpp>
#include <ratio>
#include <stdexcept>
#include <vector>
#include "buffered_executor.hpp"
#include "postprocess.hpp"
#include "preprocess.hpp"
#include "logger_forward.hpp"
#include "helper.hpp"
#include "udp_send.hpp"
namespace YDHC {
static auto format_during(const std::chrono::duration<double, std::micro> &dr) {
    if (dr.count() > 1e6) {
        return fmt::format("{:.1f}s", dr.count() * 1e-6);
    } else if (dr.count() > 1e3) {
        return fmt::format("{:.1f}ms", dr.count() * 1e-3);
    } else {
        return fmt::format("{:.1f}us", dr.count());
    }
};

static void ARotatedRect(cv::Mat &pic) {
    static float angle = 30;
    angle += 5;
    cv::RotatedRect box(cv::Point(pic.cols - 30, 30), cv::Size(20, 20), angle);
    cv::Point2f vertex[4];
    box.points(vertex);
    for (int i = 0; i < 4; i++) {
        cv::line(pic, vertex[i], vertex[(i + 1) % 4], cv::Scalar(0x01, 0xbc, 0xe9, 0), 2, cv::LINE_AA);
    }
}

void solution::main_loop() {
    printf("\r\n****Press 'enter' to Exit****\r\n\r\n");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cin.sync_with_stdio(false);
    while (m_stop_flag == false && std::cin.rdbuf()->in_avail() == 0) {
        ++work_buffer_idx;
        work_buffer_idx %= m_buf_nums;
        if (m_last_work_buffer_idx == work_buffer_idx) throw std::runtime_error("buff nums is not enough");
        LOG_TRACE("idx{}: run_once()", work_buffer_idx);
        std::shared_ptr<task_context> p = std::make_shared<task_context>();
        p->work_idx = work_buffer_idx;
        p->time_infos.reserve(10);
        p->time_infos.push_back({"frame", std::chrono::system_clock::now()});
        auto now = std::chrono::system_clock::now();
        ppl_task_camera.mt_emplace(p);
        std::this_thread::sleep_until(now + std::chrono::milliseconds(13)); // 60Hz max
    }
    m_stop_flag = true;
}

void solution::task_camera(std::shared_ptr<task_context> p) {
    p->time_infos.push_back({"camera", std::chrono::system_clock::now()});
    m_camera->SubmitAndStart(m_cmabuf[p->work_idx]->phys_addr, m_cmabuf[p->work_idx]->virt_addr);
    if (m_camera->WaitFinish()) {
        LOG_ERROR("m_camera->WaitFinish timeout");
        m_stop_flag = true;
    }
    p->time_infos.back().end = std::chrono::system_clock::now();
}

void solution::task_resize(std::shared_ptr<task_context> p) {
    LOG_TRACE("idx{}: task_resize()", p->work_idx);
    p->time_infos.push_back({"resize", std::chrono::system_clock::now()});
    cv::Mat pic(m_camera->height(), m_camera->width(), CV_8UC(m_camera->channel()), m_cmabuf[p->work_idx]->virt_addr);

    switch (m_camera->channel()) {
    case 4: {
        cv::Mat tmp;
        cv::resize(pic, tmp, {(int)get_input_width(), (int)get_input_height()});
        cv::cvtColor(tmp, tmp, cv::COLOR_BGRA2BGR);
        prepare_input_data(tmp.data, tmp.rows, tmp.cols, m_npu_quant_param, (uint8_t *)m_executor->GetInputBuffer(0, p->work_idx)->virt_addr);
    } break;
    case 2:
        soft_resize_yuv422_to_rgb_plane(pic.data, (uint8_t *)m_executor->GetInputBuffer(0, p->work_idx)->virt_addr,
                                        pic.rows, pic.cols, get_input_height(), get_input_width());
        break;
    default:
        throw std::runtime_error("not support channel");
    }

    p->time_infos.back().end = std::chrono::system_clock::now();
}

void solution::task_npu(std::shared_ptr<task_context> p) {
    LOG_TRACE("idx{}: task_npu()", p->work_idx);
    p->time_infos.push_back({"npu", std::chrono::system_clock::now()});
    if (true != m_executor->Run(p->work_idx))
        throw std::runtime_error(" buffed-executor run inference failed");
    p->time_infos.back().end = std::chrono::system_clock::now();
}

void solution::task_yolo(std::shared_ptr<task_context> p) {
    LOG_TRACE("idx{}: task_yolo()", p->work_idx);
    p->time_infos.push_back({"yolo", std::chrono::system_clock::now()});
    if (!m_yolo_workload) {
        if (m_cfg.useYolov8) {
            LOG_INFO("get workload fot yolov8 post process");
            m_yolo_workload =
                getFastDecodeOutputYolov8WorkLoad(m_executor->GetOutputTensors(0),
                                                  m_input_height, m_input_width, m_config.classNum,
                                                  m_config.probThreshold, m_config.nmsThreshold);
        } else {
            LOG_INFO("get workload fot yolo post process");
            m_yolo_workload =
                getFastDecodeOutputYoloWorkLoad(m_executor->GetOutputTensors(0),
                                                m_input_height, m_input_width, m_config.classNum,
                                                m_config.probThreshold, m_config.nmsThreshold, m_config.strideAnchorMap);
        }
    }
    m_objects[p->work_idx] = m_yolo_workload(const_cast<armnn::OutputTensors &>(m_executor->GetOutputTensors(p->work_idx)));
    p->time_infos.back().end = std::chrono::system_clock::now();
}

void solution::task_hdmi(std::shared_ptr<task_context> p) {
    LOG_TRACE("idx{}: task_hdmi()", p->work_idx);
    p->time_infos.push_back({"hdmi", std::chrono::system_clock::now()});

    auto &objects = m_objects[p->work_idx];

    m_udp_send->send_objects(m_input_width, m_input_height, objects);

    if (m_hdmi) {
        cv::Mat pic(m_camera->height(), m_camera->width(), CV_8UC(m_camera->channel()), m_cmabuf[p->work_idx]->virt_addr);
        // calculate coordinates
        for (auto &i : objects) {
            i.w = i.w * pic.cols / get_input_width();
            i.x = i.x * pic.cols / get_input_width();
            i.h = i.h * pic.rows / get_input_height();
            i.y = i.y * pic.rows / get_input_height();
            // 转换到左上角坐标表示
            i.x = i.x - i.w / 2;
            i.y = i.y - i.h / 2;
        }

        for (auto iter = objects.begin(); iter != objects.end(); iter++) {
            cv::rectangle(
                pic, cv::Point(iter->x, iter->y),
                cv::Point(iter->x + iter->w, iter->y + iter->h),
                cv::Scalar(0xF7, 0x85, 0x3C), pic.cols > 1000 ? 4 : 2);
            cv::putText(pic,
                        fmt::format("{:2.0f}%{}", 100. * iter->prob, m_config.className[iter->label]),
                        cv::Point(iter->x, std::max(int(iter->y) - 6, 30)),
                        cv::FONT_HERSHEY_SIMPLEX, pic.cols > 1000 ? 1. : 0.7, cv::Scalar(100, 50, 250), pic.cols > 1000 ? 2 : 1);
        }

        ARotatedRect(pic);
        m_hdmi->show(m_cmabuf[p->work_idx]->phys_addr, m_cmabuf[p->work_idx]->virt_addr, m_camera->channel());
    }
    p->time_infos.back().end = std::chrono::system_clock::now();
}

void solution::task_report(std::shared_ptr<task_context> p) {
    ++m_last_work_buffer_idx;
    m_last_work_buffer_idx %= m_buf_nums;

    p->time_infos[0].end = std::chrono::system_clock::now();
    static auto for_fps_calcu = std::chrono::system_clock::now();
    p->time_infos.push_back({"inv_fps", for_fps_calcu});
    p->time_infos.back().end = std::chrono::system_clock::now();
    for_fps_calcu = std::chrono::system_clock::now();
    LOG_TRACE("idx{}: task_report()", p->work_idx);

    static auto last = std::chrono::system_clock::now();
    static size_t count = 0;
    static std::vector<std::chrono::duration<double, std::micro>> sum_time;
    static std::vector<solution::task_context::time_info_t> time_infos[m_buf_nums];
    time_infos[p->work_idx] = p->time_infos;

    sum_time.resize(p->time_infos.size());
    for (size_t i = 0; i < sum_time.size(); i++) {
        sum_time[i] += p->time_infos[i].end - p->time_infos[i].start;
    }
    count++;

    std::chrono::duration<double, std::micro> during = std::chrono::system_clock::now() - last;
    if (during.count() > 10e6) {
        LOG_INFO("***** Frame Count:{}, During:{} *****", count, format_during(during));
        for (size_t i = 0; i < sum_time.size(); i++) {
            LOG_INFO("\t{:10}:{:10}({:6.1f}fps)",
                     p->time_infos[i].id, format_during(sum_time[i] / count), 1e6 * count / sum_time[i].count());
        }
        count = 0;
        sum_time.clear();
        last = std::chrono::system_clock::now();
    }

    static bool first_to_cancel = false;
    if (!first_to_cancel) {
        first_to_cancel = true;
        armnn::ConfigureLogging(true,
                                false, armnn::LogSeverity::Error);
    }
}

int solution::loadModel(const char *model, const char *addtions) {
    LOG_INFO("solution::loadModel {} {}", model, (addtions));

    armnn::ConfigureLogging(true,
                            false, armnn::LogSeverity::Info);

    std::vector<armnn::BackendId> backendPreferences(m_cfg.backends.begin(), m_cfg.backends.end());

    m_executor = std::make_unique<BufferedNPUExecutor>(m_cfg.model, backendPreferences);

    if (!m_executor) throw std::runtime_error("executor is null");

    if (m_executor->GetInputTensorNum() != 1) throw std::runtime_error("Not Support InputTensorNum() != 1");

    Size input_size = m_executor->GetInputAspectRatio(0);

    m_input_height = input_size.m_height;
    m_input_width = input_size.m_width;

    float input_scale = m_executor->GetInputQuantizationScale(0);
    int input_zero = m_executor->GetInputQuantizationOffset(0);
    armnn::DataType network_data_type = m_executor->GetInputDataType();

    npu_input_quant_param_init(m_npu_quant_param, input_scale, input_zero, network_data_type, m_config.scale, m_config.mean);

    return 0;
}

solution::solution(app_config cfg, user_config config, std::shared_ptr<icamera> _cam, std::shared_ptr<ihdmi> _hdmi, std::shared_ptr<udp_send> _udp_send)
    : m_cfg(cfg)
    , m_config(config)
    , m_camera(_cam)
    , m_hdmi(_hdmi)
    , m_udp_send(_udp_send) {
    m_mean[0] = m_config.mean[0];
    m_mean[1] = m_config.mean[1];
    m_mean[2] = m_config.mean[2];
    m_scale[0] = m_config.scale[0];
    m_scale[1] = m_config.scale[1];
    m_scale[2] = m_config.scale[2];

    size_t buffsize = m_camera->buffer_size();

    for (size_t i = 0; i < m_buf_nums; i++) {
        m_cmabuf[i] = CmaBuf{buffsize};
    }
}
solution::~solution() {
    ppl_task_camera.close();
    ppl_task_resize.close();
    ppl_task_npu.close();
    ppl_task_yolo.close();
    ppl_task_hdmi.close();
    ppl_task_report.close();
}

} // namespace YDHC