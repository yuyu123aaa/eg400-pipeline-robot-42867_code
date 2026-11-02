/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#include "npu_c_api.h"
#include <opencv2/opencv.hpp>
extern const char *the_yolo_res_class_names[];
AL_S8 draw_yolo_res_hd(cv::Mat &canvas, uint32_t input_height, uint32_t input_width, bool with_letterbox, std::vector<predictions_t> pred_res);

void un_plane(cv::Mat &src);