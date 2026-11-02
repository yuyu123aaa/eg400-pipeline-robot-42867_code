#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <string.h>
#include <poll.h>
#include <sys/types.h>
#include <linux/fb.h>
#include "hdmi_framebuffer.hpp"
#include <opencv2/opencv.hpp>

struct hdmi_framebuffer_t::impl_t {
    unsigned char *fb_mem = NULL;
    int fd_fb = 0;
    struct fb_var_screeninfo var_info;
    struct fb_fix_screeninfo fix_info;
    int preferred_width;
    int preferred_height;
    impl_t() {
    };
    ~impl_t() {
        if (fb_mem) {
            munmap(fb_mem, fix_info.smem_len);
            fb_mem = NULL;
        }
        if (fd_fb > 0) {
            close(fd_fb);
            fd_fb = 0;
        }
    }
    bool is_open() {
        return fd_fb > 0;
    }
    int open(const char *dev) {
        fd_fb = ::open(dev, O_RDWR);
        if (fd_fb < 0) {
            fprintf(stderr, "cannot open framebuffer device: %s\n", dev);
            return -1;
        }
        ioctl(fd_fb, FBIOGET_VSCREENINFO, &var_info);
        ioctl(fd_fb, FBIOGET_FSCREENINFO, &fix_info);
        printf("x=%d, y=%d\n", var_info.xres, var_info.yres);

        printf("line_length=%d, smem_len=%d\n", fix_info.line_length, fix_info.smem_len);
        printf("xpanstep=%d, ypanstep=%d\n", fix_info.xpanstep, fix_info.ypanstep);

        if (var_info.xres != preferred_width || var_info.yres != preferred_height) {
            fprintf(stderr, "only supported resolution %dx%d, need %dx%d\n",
                    var_info.xres, var_info.yres,
                    preferred_width, preferred_height);
            close(fd_fb);
            fd_fb = 0;
            return -1;
        }

        fb_mem = (unsigned char *)mmap(NULL, fix_info.smem_len,
                                       PROT_READ | PROT_WRITE, MAP_SHARED, fd_fb, 0);
        if (fb_mem == MAP_FAILED) {
            perror("Mmap LCD");
            close(fd_fb);
            fd_fb = 0;
            return -1;
        }
        return 0;
    }
};

hdmi_framebuffer_t::hdmi_framebuffer_t()
    : impl(new impl_t) {}

hdmi_framebuffer_t::hdmi_framebuffer_t(int w, int h)
    : impl(new impl_t) {
    impl->preferred_width = w;
    impl->preferred_height = h;
}

void hdmi_framebuffer_t::open(const char *dev) {
    impl->open(dev);
}

bool hdmi_framebuffer_t::is_open() {
    return impl->fd_fb > 0;
}
void *hdmi_framebuffer_t::DeQueueBuff() {
    return (void *)impl->fb_mem;
    // return impl->user_buf[impl->working_buf];
}
void hdmi_framebuffer_t::QueueBuff(void *buff) {
    // impl->switch_fd(impl->working_buf);
    // impl->working_buf = impl->working_buf ^ 0x1;

    // memcpy((void *)impl->buf[impl->working_buf].vaddr, impl->user_buf[impl->working_buf], impl->buf[0].width * impl->buf[0].height * 4);
    // impl->working_buf = impl->working_buf ^ 0x1;
}
int hdmi_framebuffer_t::get_width() const {
    return impl->var_info.xres;
}
int hdmi_framebuffer_t::get_height() const {
    return impl->var_info.yres;
}
int hdmi_framebuffer_t::get_pixel_size() const {
    return 3; // rgb
}

void hdmi_framebuffer_t::show(uint64_t base_addr, void *virt_addr, int pixelformat) {
    // find virt_addr
    if (virt_addr == nullptr)
        throw std::runtime_error("for hdmi_dim the virt_addr is nullptr");
    cv::Mat B(width(), height(), CV_8UC3, DeQueueBuff());
    if (pixelformat == 2) {
        cv::Mat A(width(), height(), CV_8UC2, virt_addr);
        // TODO 如果要支持其他颜色格式，可以在这里添加\修改
        cv::cvtColor(A, B, cv::COLOR_YUV2BGR_YUYV);
    } else if (pixelformat == 3) {
        memcpy(B.data, virt_addr, width() * height() * 3);
    } else if (pixelformat == 4) {
        cv::Mat A(width(), height(), CV_8UC4, virt_addr);
        cv::cvtColor(A, B, cv::COLOR_BGRA2BGR);
    } else {
        throw std::runtime_error("unsupported channel");
    }
    QueueBuff(B.data);
}
