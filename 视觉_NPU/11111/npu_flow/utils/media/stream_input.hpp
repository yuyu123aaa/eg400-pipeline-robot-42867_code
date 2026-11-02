#pragma once

// 抽象usb摄像头和mjpeg流的输入
// 输出内容为未解码的jpeg图像
#include "jputypes.h"
#include <armnnALUtils/cma_mem.hpp>
#include <armnnALUtils/cma_mem_init.hpp>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
class stream_input {
public:
    stream_input(stream_input &&) = delete;
    stream_input &operator=(stream_input &&) = delete;
    stream_input(const stream_input &) = delete;
    stream_input &operator=(const stream_input &) = delete;
    stream_input() = default;
    // stream_input(const std::string &media) { open(media); }
    virtual ~stream_input() { close(); };

    void close();
    void open(const std::string &media, int w = 1920, int h = 1080);
    bool is_open() const { return m_is_open; }
    bool is_eof() const { return m_eof; }
    operator bool() const { return is_eof(); }

    std::pair<std::shared_ptr<uint8_t>, size_t> read_frame(const std::vector<unsigned char> &wrap_data = {});
    bool is_dev_video() const { return m_is_dev_video; }

private:
    bool m_is_open = false;
    bool m_eof = false;
    // TODO: 如果需要支持的设备更多，则需要一个抽象的设备类
    bool m_is_dev_video = false; // 是否是usb摄像头 true:usb摄像头 false:mjpeg流
    // usb摄像头的资源
    int m_fd = 0;
#ifndef CAMERA_BUFFER_COUNT
#define CAMERA_BUFFER_COUNT 1
#endif
    static constexpr int MAX_BUF_NUMS = CAMERA_BUFFER_COUNT;
    static_assert(MAX_BUF_NUMS >= 1 && MAX_BUF_NUMS <= 8, "invalid camera buffer count");
    std::atomic_int m_qbuff_count = MAX_BUF_NUMS; // 在usb摄像头中，缓冲区的数量
    cma_buffer_t m_cma_buffer[MAX_BUF_NUMS];
    float m_buffer_size_filer_for_bad_frame = 0;
    int image_height;
    int image_width;
    unsigned char *image_buffer[MAX_BUF_NUMS];
    static constexpr bool user_def_buffer = false; // 不支持使用cma作为自定义的缓冲区
    int VideoDeviceInit(const char *DEVICE_NAME, int pixel_format);

    // mjpeg流的资源
    static constexpr bool user_def_buffer_mjpeg = true;
    std::ifstream m_url;
};

/// 引入jpu解码器
#include "JW_Wrapper.h"

struct jpu_cma_buffer {
    static struct JW_BuffHandleVtable vtable;

    JW_BuffHandleBase_t base = {&vtable};
    cma_buffer_t cma;
    int64_t rdptr = 0;
    std::function<void(jpu_cma_buffer *, Uint32)> feedCallback;
    std::function<void(jpu_cma_buffer *, Uint32)> writeCallback;

    jpu_cma_buffer(jpu_cma_buffer &&) = delete;
    jpu_cma_buffer &operator=(jpu_cma_buffer &&) = delete;
    jpu_cma_buffer(const jpu_cma_buffer &) = delete;
    jpu_cma_buffer &operator=(const jpu_cma_buffer &) = delete;

    void alloc(uint32_t size) {
        assert(cma.phys_addr == 0);
        cma.size = size;
        cma_mem_alloc(&cma);
    }
    jpu_cma_buffer() = default;
    jpu_cma_buffer(uint32_t size)
        : cma(size) {
        cma_mem_alloc(&cma);
    }
    ~jpu_cma_buffer() {
        if (cma.phys_addr != 0 && cma.flags != 0xf) {
            cma_mem_free(&cma);
            cma.phys_addr = 0;
            rdptr = 0;
        }
    }

    void import(const cma_buffer_t &bf) {
        cma = bf;
        cma.flags = 0xf; // 代表是外面传入的内存
    }

    // 和import对应
    void reserve(Uint32 new_cap_size) {
        if (cma.size >= new_cap_size)
            return;
        this->~jpu_cma_buffer();
        cma = cma_buffer_t(new_cap_size);
        cma_mem_alloc(&cma);
    }
    void ResetPtr() {
        rdptr = 0;
    }

    // 下面是接口区域
    static void DeInit(struct JW_BuffHandleBase *p);
    static PhysicalAddress GetPhysAddr(struct JW_BuffHandleBase *p);
    static void *GetVirtAddr(struct JW_BuffHandleBase *p);
    static Uint32 GetSize(struct JW_BuffHandleBase *p);
    static Uint32 Reserve(struct JW_BuffHandleBase *p, Uint32 new_cap_size);
    static Uint32 FeedAct(struct JW_BuffHandleBase *p, Uint32 feed_size);
    static Uint32 WriteAct(struct JW_BuffHandleBase *p, Uint32 write_size);
    static Uint32 GetWriteSize(struct JW_BuffHandleBase *p);
    static void ResetWrRdPtr(struct JW_BuffHandleBase *p, BOOL update_mem_offset);
    static void *HeaderWrite(struct JW_BuffHandleBase *p, Uint32 reserve_size);
    static void HeaderWriteFinish(struct JW_BuffHandleBase *p, Uint32 write_size);
};

/// 包装解码器，jpu解码恒定输出yuv422 yuyv 的图像
/// bitfeed和yuvwrite都是上面的jpu_cma_buffer
class jpu_decoder_gift {
public:
    jpu_decoder_gift(jpu_decoder_gift &&) = delete;
    jpu_decoder_gift &operator=(jpu_decoder_gift &&) = delete;
    jpu_decoder_gift(const jpu_decoder_gift &) = delete;
    jpu_decoder_gift &operator=(const jpu_decoder_gift &) = delete;
    jpu_decoder_gift() = default;
    /*
    下面是接口区域的伪代码

    jpu_decoder_gift gift;
    stream_input input;

    input.open("xxx");
    assert(input.is_open());
    // 判断input是否为空
    while (input) {
        gift << input;
        jpu_cma_buffer& yuv = some_yuv_buffer_manager.get(xxx);
        // 解码一帧
        while (gift >> yuv) {
            // do something to yuv frame in gift.yuvwrite
        }
    }


    */

    // 导入jpeg bit，可以包含0个或多个帧
    void emplace_jpeg_bits_from(stream_input &r);
    // 仅解码一帧,返回true代表解码一帧，否则还得继续填充bit
    bool decode_frame_to(jpu_cma_buffer &bf) {
        yuvwrite = &bf;
        if (!m_is_open) {
            m_is_open = true;
            return InitAndDecode();
        } else {
            return Decode();
        }
    }

    // 返回错误代码
    int get_error_code() const;

    int get_width() const {
        return context.initialInfo.picWidth;
    }
    int get_height() const {
        return context.initialInfo.picHeight;
    }

    const jpu_cma_buffer &get_bitfeed() const {
        return bitfeed;
    }

    // 相对于JW_DecStartOneFrame，修改了部分逻辑
    static JpgRet JDEC_DecStartOneFrame(JpgDecHandle handle, JpgDecParam *param);

private:
    // 初始化的饿时候，bitfeed是有东西的
    // 要返回解码是否成功
    bool InitAndDecode();
    bool Decode();
    bool m_is_open = false; // 是否已经初始化
    bool m_eof = false;     // 代表bitfeed是否已经读完
    jpu_cma_buffer bitfeed;
    jpu_cma_buffer *yuvwrite;
    int int_reason = 0; // 中断代码，也是解码的状态机
    JW_DecoderContext_t context;
    // debug变量区域
    PhysicalAddress rdPtr;
    PhysicalAddress wrPtr;

    // 控制是否是usb相机
    bool m_is_dev_video = false;

    // 在mjpeg流投喂过程中，jpu解码到缓存边界时候不能处理warp的数据，这里手动处理一下
    std::vector<unsigned char> m_jpu_wrap_data;
    int m_jpu_wrap_mode_status = 0; // 解码的时候的处理状态机
};
