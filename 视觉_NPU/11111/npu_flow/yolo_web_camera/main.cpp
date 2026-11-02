/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#include <memory>
#include <cstdio>
#include <string>
#include "flow_common_types.hpp"
#include "icamera.hpp"
#include "udp_send.hpp"
#include "ihdmi.hpp"
#include "solution.hpp"
#include "logger_forward.hpp"
#include "cmdline.h"
#include "web_server.hpp"
#include "web_state.hpp"
using namespace YDHC;

int main(int argn, char **argv) {
    std::printf("[CAMERA-CANDIDATE] profiled USB MJPEG/JPU pipeline\n");
    cmdline::parser parser;
    parser.add<std::string>("model", 'm', "the onnx model of yolo for object detection", true);
    parser.add<std::string>("backends", 'b', "preference backends", false, "Alnpu,CpuAcc,CpuRef");
    parser.add<std::string>("input", 'i', "camera device, '/dev/video0' or others", false, "/dev/video0");
    parser.add<std::string>("server", 's', "服务器ip和udp端口号，<IP>:<PORT>", true);
    parser.add<std::string>("output", 'o', "display device, '/dev/dri/card0' or '/dev/fb0'", false, "");
    parser.add<std::string>("usercfg", 'u', "a json file contain input normalization and yolo-post-process config", true);
    parser.add<std::string>("geometry", 0, "camera&display geometry, <WIDTH>x<HEIGHT>", false, "1920x1080");
    parser.add<unsigned int>("logLevel", 'l', "log level, bigger for less", false, get_log_level());
    parser.add<bool>("yolov8", 0, "marked yolov8 for special post process", false, false);
    parser.add<unsigned int>("web-port", 0, "HTTP listen port", false, 8080);
    parser.add<std::string>("web-root", 0, "directory containing index.html/app.js/style.css", false, "/opt/vision/web");

    parser.parse_check(argn, argv);
    app_config cfg;
    cfg.model = parser.get<std::string>("model");
    cfg.backends = splitstr(parser.get<std::string>("backends"), ',');
    cfg.input = parser.get<std::string>("input");
    cfg.output = parser.get<std::string>("output");
    cfg.usercfg = parser.get<std::string>("usercfg");
    cfg.camera = true;
    cfg.hdmi = true;
    set_log_level(parser.get<unsigned int>("logLevel"));
    cfg.useYolov8 = parser.get<bool>("yolov8");
    if (!cfg.useYolov8) {
        auto low = cfg.model;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        cfg.useYolov8 = low.find("yolov8") != std::string::npos;
    }
    user_config config = paser_user_cfg(cfg.usercfg);

    CmaResourceZone zone; // use RAII to manage Cma

    // enum camera and hdmi devices
    std::string camera_geometry = parser.get<std::string>("geometry");
#ifdef FORCE_CAMERA_GEOMETRY
    camera_geometry = FORCE_CAMERA_GEOMETRY;
    std::printf("[CAMERA-CANDIDATE] forcing capture geometry=%s (model input remains unchanged)\n",
                camera_geometry.c_str());
#endif
#ifdef WEB_SHARED_JPEG
    std::printf("[CAMERA-CANDIDATE] shared JPEG enabled\n");
#else
    std::printf("[CAMERA-CANDIDATE] compatibility JPEG copy mode\n");
#endif
    std::shared_ptr<icamera> m_camera = icamera::enumerate_device(cfg.input.c_str(), camera_geometry.c_str());
    std::shared_ptr<ihdmi> m_hdmi;
    if (cfg.output.size() != 0) {
        // 表示不启用hdmi
        m_hdmi = ihdmi::enumerate_device(cfg.output.c_str(), camera_geometry.c_str());
    }
    std::shared_ptr<udp_send> udp_sender = udp_send::create_udp_sender(parser.get<std::string>("server"));
    auto web_data = std::make_shared<web_state>();
    web_server http(web_data,
                    static_cast<unsigned short>(parser.get<unsigned int>("web-port")),
                    parser.get<std::string>("web-root"));

    if (!m_camera || !udp_sender) {
        LOG_ERROR("enumerate device failed camera:{},or udp_sender:{}", (void *)m_camera.get(), (void *)udp_sender.get());
        return -1;
    }

    if (!http.start()) {
        LOG_ERROR("cannot start HTTP server at port {}", parser.get<unsigned int>("web-port"));
        return -1;
    }

    solution solution_ins(cfg, config, m_camera, m_hdmi, udp_sender, web_data);

    // init AI network
    solution_ins.loadModel(
        cfg.model.c_str(),
        "");

    solution_ins.main_loop();
    return 0;
}
