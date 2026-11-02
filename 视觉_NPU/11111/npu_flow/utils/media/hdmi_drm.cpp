#ifdef ENABLE_LIBDRM_HDMI
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "hdmi_drm.hpp"
#include <opencv2/opencv.hpp>
struct buffer_object {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t handle;
    uint32_t size;
    uint32_t *vaddr;
    uint32_t fb_id;
};
struct hdmi_drm_t::impl_t *hdmi_drm_t::hdmi_drm_impl_ins;
struct hdmi_drm_t::impl_t {
    struct buffer_object buf[2];
    void *user_buf[2];
    int fd = 0;
    int working_buf = 0;
    drmModeConnector *conn;
    drmModeRes *res;
    uint32_t conn_id;
    uint32_t crtc_id;
    int preferred_width = 1920;  // 用户传入的分辨率
    int preferred_height = 1080; // 用户传入的分辨率
    drmEventContext ev = {};
    impl_t() {
        hdmi_drm_impl_ins = this;
    };
    ~impl_t() {
        deinit();
    }
    int modeset_create_fb(int fd, struct buffer_object *bo, uint32_t color) {
        struct drm_mode_create_dumb create = {};
        struct drm_mode_map_dumb map = {};
        uint32_t i;

        create.width = bo->width;
        create.height = bo->height;
        create.bpp = 32;
        drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);

        bo->pitch = create.pitch;
        bo->size = create.size;
        bo->handle = create.handle;
        drmModeAddFB(fd, bo->width, bo->height, 24, 32, bo->pitch,
                     bo->handle, &bo->fb_id);

        map.handle = create.handle;
        drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);

        bo->vaddr = (uint32_t *)mmap(0, create.size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd, map.offset);

        for (i = 0; i < (bo->size / 4); i++)
            bo->vaddr[i] = color;

        return 0;
    }

    void modeset_destroy_fb(int fd, struct buffer_object *bo) {
        struct drm_mode_destroy_dumb destroy = {};

        drmModeRmFB(fd, bo->fb_id);

        munmap(bo->vaddr, bo->size);

        destroy.handle = bo->handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }

    void switch_fd(int idx) {
        if (fd > 0) {
            // drmModeSetCrtc(fd, crtc_id, buf[idx].fb_id,
            //    0, 0, &conn_id, 1, &conn->modes[0]);
            drmHandleEvent(fd, &ev); // 阻塞等待垂直同步
        }
    }

    static void modeset_page_flip_handler(int fd, uint32_t frame,
                                          uint32_t sec, uint32_t usec,
                                          void *data) {
        uint32_t crtc_id = *(uint32_t *)data;
        drmModePageFlip(fd, crtc_id, hdmi_drm_impl_ins->buf[hdmi_drm_impl_ins->working_buf].fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, data);
    }
    int open(const char *dev) {
        fd = ::open(dev, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "cannot open drm device device: %s\n", dev);
            return -1;
        }
        res = drmModeGetResources(fd);
        if (!res) {
            fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n", errno);
            close(fd);
            fd = 0;
            return -1;
        }
        crtc_id = res->crtcs[0];
        conn_id = res->connectors[0];

        conn = drmModeGetConnector(fd, conn_id);
        if (!conn) {
            fprintf(stderr, "cannot retrieve DRM connector (%d): %m\n", errno);
            close(fd);
            fd = 0;
            return -1;
        }
        // 打印所有支持的模式
        for (int i = 0; i < conn->count_modes; i++) {
            fprintf(stdout, "mode %d: %dx%d (%d)\n", i, conn->modes[i].hdisplay, conn->modes[i].vdisplay, conn->modes[i].vrefresh);
        }
        // 搜索指定宽高的模式
        int preferred_mode = -1;
        for (int i = 0; i < conn->count_modes; i++) {
            if (conn->modes[i].hdisplay == preferred_width && conn->modes[i].vdisplay == preferred_height) {
                preferred_mode = i;
                break;
            }
        }
        if (preferred_mode == -1) {
            fprintf(stderr, "cannot find %dx%d mode\n", preferred_width, preferred_height);
            close(fd);
            fd = 0;
            return -1;
        }

        buf[0].width = conn->modes[preferred_mode].hdisplay;
        buf[0].height = conn->modes[preferred_mode].vdisplay;
        buf[1].width = conn->modes[preferred_mode].hdisplay;
        buf[1].height = conn->modes[preferred_mode].vdisplay;

        modeset_create_fb(fd, &buf[0], 0xff0000);
        modeset_create_fb(fd, &buf[1], 0x0000ff);

        ev.version = DRM_EVENT_CONTEXT_VERSION;
        ev.page_flip_handler = modeset_page_flip_handler;

        drmModeSetCrtc(fd, crtc_id, buf[0].fb_id,
                       0, 0, &conn_id, 1, &conn->modes[preferred_mode]);
        drmModeDirtyFB(fd, buf[0].fb_id, NULL, 0);
        // drmModePageFlip(fd, crtc_id, buf[0].fb_id,
        // DRM_MODE_PAGE_FLIP_EVENT, &crtc_id);

        user_buf[0] = malloc(sizeof(uint32_t) * buf[0].width * buf[0].height);
        user_buf[1] = malloc(sizeof(uint32_t) * buf[0].width * buf[0].height);
        return 0;
        // modeset_destroy_fb(fd, &buf[1]);
        // modeset_destroy_fb(fd, &buf[0]);

        // drmModeFreeConnector(conn);
        // drmModeFreeResources(res);

        // close(fd);
        // fd = 0;
    }

    void deinit() {
        if (fd > 0) {
            modeset_destroy_fb(fd, &buf[1]);
            modeset_destroy_fb(fd, &buf[0]);

            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            free(user_buf[0]);
            free(user_buf[1]);
            close(fd);
            fd = 0;
        }
    }
};

hdmi_drm_t::hdmi_drm_t()
    : impl(new impl_t) {}

hdmi_drm_t::hdmi_drm_t(int w, int h)
    : impl(new impl_t) {
    impl->preferred_width = w;
    impl->preferred_height = h;
}

void hdmi_drm_t::open(const char *dev) {
    impl->open(dev);
}

bool hdmi_drm_t::is_open() {
    return impl->fd > 0;
}
void *hdmi_drm_t::DeQueueBuff() {
    return (void *)impl->buf[impl->working_buf].vaddr;
    // return impl->user_buf[impl->working_buf];
}
void hdmi_drm_t::QueueBuff(void *buff) {
    // impl->switch_fd(impl->working_buf);
    // impl->working_buf = impl->working_buf ^ 0x1;

    // memcpy((void *)impl->buf[impl->working_buf].vaddr, impl->user_buf[impl->working_buf], impl->buf[0].width * impl->buf[0].height * 4);
    // impl->working_buf = impl->working_buf ^ 0x1;
}
int hdmi_drm_t::get_width() const {
    return impl->buf[0].width;
}
int hdmi_drm_t::get_height() const {
    return impl->buf[0].height;
}
int hdmi_drm_t::get_pixel_size() const {
    return 4;
}

void hdmi_drm_t::show(uint64_t base_addr, void *virt_addr, int pixelformat) {
    // find virt_addr
    if (virt_addr == nullptr)
        throw std::runtime_error("for hdmi_dim the virt_addr is nullptr");
    cv::Mat B(width(), height(), CV_8UC4, DeQueueBuff());
    if (pixelformat == 2) {
        cv::Mat A(width(), height(), CV_8UC2, virt_addr);
        cv::cvtColor(A, B, cv::COLOR_YUV2BGRA_YUYV);
    } else if (pixelformat == 4) {
        memcpy(B.data, virt_addr, width() * height() * 4);
    } else {
        throw std::runtime_error("unsupported channel");
    }
    QueueBuff(B.data);
}

#endif // ENABLE_LIBDRM_HDMI