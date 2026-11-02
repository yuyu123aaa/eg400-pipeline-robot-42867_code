#include "icamera.hpp"
#include "usb_camera.hpp"
#include "mipi_camera.hpp"
#include <cassert>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>
static bool prefix_cmp(const char *pattern, const char *dev) {
    while (*pattern != 0 and *dev != 0) {
        if (*pattern != *dev) return false;
        pattern++;
        dev++;
    }
    return *pattern == 0;
}
static std::vector<std::pair<const char *,
                             std::function<std::shared_ptr<icamera>(int w, int h)>>>
    icam_creators = {
        {"/dev/mipicam", [](int w, int h) { return std::make_shared<camera_t>(); }},
        {"/dev/video", [](int w, int h) { return std::make_shared<usb_camera_t>(w, h); }},
};

std::shared_ptr<icamera> icamera::enumerate_device(const char *dev, const char *resolution) {
    int w = -1, h = -1;
    sscanf(resolution, "%dx%d", &w, &h);
    if (w == -1 or h == -1) {
        fprintf(stderr, "invalid resolution:'%s', for example '1920x1080'\n", resolution);
        return nullptr;
    }
    std::shared_ptr<icamera> ret = nullptr;
    std::size_t mask = 0;
    assert(sizeof(size_t) * 8 >= icam_creators.size());
    // 先按照pattern去匹配
    for (size_t i = 0; i < icam_creators.size(); i++) {
        if (icam_creators[i].first != nullptr && prefix_cmp(icam_creators[i].first, dev)) {
            mask |= (1 << i);
            ret = icam_creators[i].second(w, h);
            ret->open(dev);
            if (ret->is_open()) return ret;
        }
    }
    // 还是不行的话，就全部试一遍
    for (size_t i = 0; i < icam_creators.size(); i++) {
        if (mask & (1 << i)) continue;
        ret = icam_creators[i].second(w, h);
        ret->open(dev);
        if (ret->is_open()) return ret;
    }
    // 都没有成功
    return nullptr;
}