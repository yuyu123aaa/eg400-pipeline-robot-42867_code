// #include "hdmi.hpp"
#include "hdmi_drm.hpp"
#include "hdmi_framebuffer.hpp"
#include "ihdmi.hpp"
#include <cassert>
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
                             std::function<std::shared_ptr<ihdmi>(int w, int h)>>>
    ihdmi_creators = {
// {nullptr, []() { return std::make_shared<mipi_camera_t>(); }},
#ifdef ENABLE_LIBDRM_HDMI
        {"/dev/dri/card", [](int w, int h) { return std::make_shared<hdmi_drm_t>(w, h); }},
#endif
        {"/dev/fb", [](int w, int h) { return std::make_shared<hdmi_framebuffer_t>(w, h); }},
};

std::shared_ptr<ihdmi> ihdmi::enumerate_device(const char *dev, const char *resolution) {
    int w = -1, h = -1;
    sscanf(resolution, "%dx%d", &w, &h);
    if (w == -1 or h == -1) {
        fprintf(stderr, "invalid resolution:'%s', for example '1920x1080'\n", resolution);
        return nullptr;
    }
    std::shared_ptr<ihdmi> ret = nullptr;
    std::size_t mask = 0;
    assert(sizeof(size_t) * 8 >= ihdmi_creators.size());
    // 先按照pattern去匹配
    for (size_t i = 0; i < ihdmi_creators.size(); i++) {
        if (ihdmi_creators[i].first != nullptr && prefix_cmp(ihdmi_creators[i].first, dev)) {
            mask |= (1 << i);
            ret = ihdmi_creators[i].second(w, h);
            ret->open(dev);
            if (ret->is_open()) return ret;
        }
    }
    // 还是不行的话，就全部试一遍
    for (size_t i = 0; i < ihdmi_creators.size(); i++) {
        if (mask & (1 << i)) continue;
        ret = ihdmi_creators[i].second(w, h);
        ret->open(dev);
        if (ret->is_open()) return ret;
    }
    // 都没有成功
    return nullptr;
}