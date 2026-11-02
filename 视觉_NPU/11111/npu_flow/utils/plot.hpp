/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#include "opencv2/imgproc.hpp"
#include "logger_forward.hpp"
#include <cfloat>
#include <vector>
#include <map>
#include <opencv2/opencv.hpp>

class plot_t {
public:
    struct bar {
        struct box {
            cv::Rect2f rect;
            std::string id;
            cv::Scalar color;
        };
        std::vector<box> boxs;
    };

    std::map<size_t, bar> bars;

    void plot() {
        for (const auto &i : bars) {
            for (const auto &j : i.second.boxs) {
                min_x = std::min(min_x, j.rect.x);
                min_y = std::min(min_y, j.rect.y);
                max_x = std::max(max_x, j.rect.x + j.rect.width);
                max_y = std::max(max_y, j.rect.y + j.rect.height);
            }
        }
        scale_x = pic_x_range / (max_x - min_x);
        scale_y = pic_y_range / (max_y - min_y) / (bars.size() + 1);
        step_y = pic_y_range / bars.size();

        cv::Mat canvas((int)pic_y_range, (int)pic_x_range, CV_8UC3);

        int idx = 0;
        for (const auto &i : bars) {
            for (size_t j = 0; j < i.second.boxs.size(); j++) {
                auto sit = resite(i.second.boxs[j].rect, idx);
                cv::rectangle(canvas, pt1(sit), pt2(sit), i.second.boxs[j].color, 1);
                cv::line(canvas, pt1(sit), pt2(sit), i.second.boxs[j].color, 1);
                cv::putText(canvas,
                            i.second.boxs[j].id,
                            ptstr(sit),
                            cv::FONT_HERSHEY_SIMPLEX, 0.75, cv::Scalar(100, 150, 200), 2);
            }
            idx++;
        }
        cv::imwrite("the_demov2_timeline.jpg", canvas);
        LOG_INFO("See \"the_demov2_timeline.jpg\"");
    }

    cv::Rect2f resite(const cv::Rect2f &r, int idx) {
        return {(r.x - min_x) * scale_x, r.y - min_y + idx * step_y, r.width * scale_x, r.height * scale_y};
    }

    cv::Point pt1(const cv::Rect2f &r) {
        return {(int)r.x, (int)r.y};
    }

    cv::Point ptstr(const cv::Rect2f &r) {
        int x = r.x;
        if (x > pic_x_range - 30)
            x -= 30;
        return {x, (int)r.y + 30};
    }
    cv::Point pt2(const cv::Rect2f &r) {
        return {(int)(r.x + r.width), (int)(r.y + r.height)};
    }

private:
    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;

    float scale_x = 0;
    float scale_y = 0;
    float step_y = 0;

    float pic_x_range = 1920;
    float pic_y_range = 720;
};