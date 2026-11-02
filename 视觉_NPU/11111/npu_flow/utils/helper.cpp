/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#include "helper.hpp"
#include <memory>

#pragma GCC optimize("O3")

const char *the_yolo_res_class_names[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
    "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"};

AL_S8 draw_yolo_res_hd(cv::Mat &canvas, uint32_t input_height, uint32_t input_width, bool with_letterbox, std::vector<predictions_t> pred_res) {
    int img_h = static_cast<int> (input_height);
    int img_w = static_cast<int> (input_width);

    static const uint8_t palette[8][4] = {
        {0xF7, 0x85, 0x3C, 4},
        {0xF7, 0xBD, 0x3C, 4},
        {0xDB, 0x35, 0x6A, 4},
        {0xA0, 0x4B, 0x13, 4},
        {0xBF, 0xEB, 0x39, 4},
        {0x2E, 0xBF, 0x40, 4},
        {0xF7, 0xE3, 0x3C, 4},
        {0x78, 0x99, 0x12, 4}};
    if (!with_letterbox) {
        for (AL_S16 i = 0; i < pred_res.size(); i++) {
            // pred_res[i].prob = prob;
            // pred_res[i].label = label;
            pred_res[i].label_name =
                the_yolo_res_class_names[pred_res[i].label >= (sizeof(the_yolo_res_class_names) / sizeof(const char *)) ? 0 : pred_res[i].label];

            pred_res[i].x_l = pred_res[i].x_l * canvas.cols / img_w;
            pred_res[i].y_l = pred_res[i].y_l * canvas.rows / img_h;
            pred_res[i].x_h = pred_res[i].x_h * canvas.cols / img_w;
            pred_res[i].y_h = pred_res[i].y_h * canvas.rows / img_h;
            auto &use_palette = palette[pred_res[i].label % (sizeof(palette) / 4)];
            cv::rectangle(
                canvas, cv::Point(pred_res[i].x_l, pred_res[i].y_l),
                cv::Point(pred_res[i].x_h, pred_res[i].y_h),
                cv::Scalar(use_palette[0], use_palette[1], use_palette[2]), use_palette[3]);
            cv::putText(canvas,
                        std::string(pred_res[i].label_name) + ":" + std::to_string((int)(pred_res[i].prob * 100.)) + "%",
                        cv::Point(pred_res[i].x_l, pred_res[i].y_l),
                        cv::FONT_HERSHEY_SIMPLEX, 1., cv::Scalar(100, 50, 250), 2);
        }
    } else {
        int bl_h;
        int bl_w;
        int fill_h;
        if (canvas.rows * img_w < img_h * canvas.cols) // (canvas.rows / canvas.cols < img_h / img_w)
        {
            fill_h = 1;
            bl_w = canvas.cols;
            bl_h = img_h * canvas.cols / img_w;
        } else {
            fill_h = 0;
            bl_w = img_w * canvas.rows / img_h;
            bl_h = canvas.rows;
        }
        int ydif = (bl_h - canvas.rows) / 2;
        int xdif = (bl_w - canvas.cols) / 2;
        for (AL_S16 i = 0; i < pred_res.size(); i++) {
            // recalculate site of bigger letter_box
            pred_res[i].x_l = pred_res[i].x_l * bl_w / img_w;
            pred_res[i].y_l = pred_res[i].y_l * bl_h / img_h;
            pred_res[i].x_h = pred_res[i].x_h * bl_w / img_w;
            pred_res[i].y_h = pred_res[i].y_h * bl_h / img_h;
            // recalculate site of bigger letter
            if (fill_h) {
                pred_res[i].y_l -= ydif;
                pred_res[i].y_h -= ydif;
                pred_res[i].y_l = MAX(pred_res[i].y_l, 0);
            } else {
                pred_res[i].x_l -= xdif;
                pred_res[i].x_h -= xdif;
                pred_res[i].x_l = MAX(pred_res[i].x_l, 0);
            }
            auto &use_palette = palette[pred_res[i].label % (sizeof(palette) / 4)];
            cv::rectangle(
                canvas, cv::Point(pred_res[i].x_l, pred_res[i].y_l),
                cv::Point(pred_res[i].x_h, pred_res[i].y_h),
                cv::Scalar(use_palette[0], use_palette[1], use_palette[2]), use_palette[3]);
            cv::putText(canvas,
                        std::string(pred_res[i].label_name) + ":" + std::to_string((int)(pred_res[i].prob * 100.)) + "%",
                        cv::Point(pred_res[i].x_l, pred_res[i].y_l),
                        cv::FONT_HERSHEY_SIMPLEX, 1., cv::Scalar(100, 50, 250), 2);
        }
    }

    return 0;
}


void un_plane(cv::Mat &src) {
    assert(src.isContinuous());
    auto dst = cv::Mat(src.rows, src.cols, CV_8UC3);
    uint8_t *B = src.data;
    uint8_t *G = src.data + src.rows * src.cols;
    uint8_t *R = src.data + src.rows * src.cols * 2;
    for (size_t i = 0; i < src.rows; i++) {
        for (size_t j = 0; j < src.cols; j++) {
            dst.at<cv::Vec3b>(i, j)[0] = B[src.cols * i + j];
            dst.at<cv::Vec3b>(i, j)[1] = G[src.cols * i + j];
            dst.at<cv::Vec3b>(i, j)[2] = R[src.cols * i + j];
        }
    }
    std::swap(src, dst);
}