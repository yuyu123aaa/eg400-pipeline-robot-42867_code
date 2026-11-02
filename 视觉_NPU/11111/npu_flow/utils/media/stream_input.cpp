#include "stream_input.hpp"
#include "JW_Wrapper.h"
#include "jdi.h"
#include "jpuapifunc.h"
#include "jpuapi.h"
#include "regdefine.h"
#include "logger_forward.hpp"
#include <armnnALUtils/cma_mem_init.hpp>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <fmt/core.h>
#include <stdio.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <unistd.h>

constexpr int jpeg_buff_common_size() { return 1024 * 1024; }

/*
函数功能: 摄像头设备初始化
*/
int stream_input::VideoDeviceInit(const char *DEVICE_NAME, int pixel_format) {
    static struct v4l2_capability cap;
    /*1. 打开摄像头设备*/
    m_fd = ::open(DEVICE_NAME, O_RDWR);
    if (m_fd < 0) {
        printf("%s 摄像头设备打开失败!\n", DEVICE_NAME);
        return -1;
    } else {
        printf("摄像头打开成功.\n");
    }

    if (ioctl(m_fd, VIDIOC_QUERYCAP, &cap) == -1) {
        printf("Error opening device %s: unable to query device.\n", DEVICE_NAME);
        return -1;
    } else {
        printf("driver:\t\t%s\n", cap.driver);
        printf("card:\t\t%s\n", cap.card);
        printf("bus_info:\t%s\n", cap.bus_info);
        printf("version:\t%d\n", cap.version);
        printf("capabilities:\t%x\n", cap.capabilities);

        if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == V4L2_CAP_VIDEO_CAPTURE) {
            printf("Device %s: supports capture.\n", DEVICE_NAME);
        }

        if ((cap.capabilities & V4L2_CAP_STREAMING) == V4L2_CAP_STREAMING) {
            printf("Device %s: supports streaming.\n", DEVICE_NAME);
        }
    }

    /*2 设置摄像头的属性*/
    struct v4l2_format format;
    /*2.1 查询当前摄像头支持的格式*/
    // 当前视频设备支持的视频图像格式
    struct v4l2_fmtdesc fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.index = 0;
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    printf("当前摄像头支持输出的图像格式如下:\n");
    while (ioctl(m_fd, VIDIOC_ENUM_FMT, &fmt) != -1) {
        printf("\t%d.%s\n", fmt.index + 1, fmt.description);
        fmt.index++;
    }

#if 1
    while (ioctl(m_fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
        fmt.index++;
        printf("<'%c%c%c%c'--'%s'>\n",
               fmt.pixelformat & 0xff, (fmt.pixelformat >> 8) & 0xff,
               (fmt.pixelformat >> 16) & 0xff, (fmt.pixelformat >> 24) & 0xff,
               fmt.description);
    }
#endif

    /*2.2 设置摄像头输出的宽度高度与颜色格式(V4L2_PIX_FMT_YUYV)*/
    memset(&format, 0, sizeof(struct v4l2_format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; /*表示视频捕获设备*/
    format.fmt.pix.width = image_width;        /*预设的宽度*/
    format.fmt.pix.height = image_height;      /*预设的高度*/

    if (pixel_format == 0)
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    else if (pixel_format == 1)
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    else
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

    format.fmt.pix.field = V4L2_FIELD_ANY; /*系统自动设置: 帧属性*/
    if (ioctl(m_fd, VIDIOC_S_FMT, &format)) {
        printf("摄像头格式设置失败!\n");
        return -2;
    }

    printf("摄像头实际输出的图像尺寸:x=%d,y=%d\n", format.fmt.pix.width, format.fmt.pix.height);
    if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV) {
        printf("当前摄像头支持YUYV格式图像输出!\n");
    } else if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG) {
        printf("当前摄像头支持MJPEG格式图像输出!\n");
    } else {
        printf("当前摄像头不支持YUYV/MJPEG格式图像输出!\n");
        return -3;
    }

    /*2.3 设置摄像头采集的帧率*/
    struct v4l2_streamparm streamparm{};
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; /*表示视频捕获设备*/
    streamparm.parm.capture.timeperframe.numerator = 1;
    streamparm.parm.capture.timeperframe.denominator = 30;
    printf("设置当前摄像头采集帧率: %d秒%d帧\n", streamparm.parm.capture.timeperframe.numerator, streamparm.parm.capture.timeperframe.denominator);
    if (ioctl(m_fd, VIDIOC_S_PARM, &streamparm)) { /*设置摄像头的帧率*/
        printf("设置摄像头采集的帧率失败!\n");
        return -3;
    }

    if (ioctl(m_fd, VIDIOC_G_PARM, &streamparm)) { /*读取驱动最终接受的帧率*/
        printf("获取摄像头采集的帧率失败!\n");
        return -3;
    }
    printf("当前摄像头实际采集帧率: %d秒%d帧\n", streamparm.parm.capture.timeperframe.numerator, streamparm.parm.capture.timeperframe.denominator);

    if (!user_def_buffer) {
        /*3. 请求缓冲区: 申请摄像头数据采集的缓冲区*/
        struct v4l2_requestbuffers req_buff;
        memset(&req_buff, 0, sizeof(struct v4l2_requestbuffers));
        req_buff.count = MAX_BUF_NUMS;               /*预设要申请4个缓冲区*/
        req_buff.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; /*视频捕获设备*/
        req_buff.memory = V4L2_MEMORY_MMAP;          /*支持mmap内存映射*/
        if (ioctl(m_fd, VIDIOC_REQBUFS, &req_buff)) {
            printf("申请摄像头数据采集的缓冲区失败!\n");
            return -4;
        }
        m_qbuff_count = static_cast<int>(req_buff.count);
        printf("摄像头缓冲区申请的数量: %d\n", req_buff.count);

        /*4. 获取缓冲区的详细信息: 地址,编号*/
        struct v4l2_buffer buff_info;
        memset(&buff_info, 0, sizeof(struct v4l2_buffer));
        int i;
        for (i = 0; i < req_buff.count; i++) {
            memset(&buff_info, 0, sizeof(struct v4l2_buffer));
            buff_info.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; /*视频捕获设备*/
            buff_info.memory = V4L2_MEMORY_MMAP;          /*支持mmap内存映射*/
            buff_info.index = i;
            if (ioctl(m_fd, VIDIOC_QUERYBUF, &buff_info)) {
                printf("获取缓冲区的详细信息失败!\n");
                return -5;
            }
            /*根据摄像头申请缓冲区信息: 使用mmap函数将内核的地址映射到进程空间*/
            image_buffer[i] = (uint8_t *)mmap(NULL, buff_info.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, buff_info.m.offset);
            if (image_buffer[i] == NULL) {
                printf("缓冲区映射失败!\n");
                return -6;
            }
        }

        /*5. 将缓冲区放入采集队列*/
        memset(&buff_info, 0, sizeof(struct v4l2_buffer));
        for (i = 0; i < req_buff.count; i++) {
            buff_info.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; /*视频捕获设备*/
            buff_info.index = i;                          /*缓冲区的节点编号*/
            buff_info.memory = V4L2_MEMORY_MMAP;          /*支持mmap内存映射*/
            if (ioctl(m_fd, VIDIOC_QBUF, &buff_info)) {
                printf("根据节点编号将缓冲区放入队列失败!\n");
                return -7;
            }
        }
    } else {
        /*3. 请求缓冲区: 申请摄像头数据采集的缓冲区*/
        struct v4l2_requestbuffers req_buff;
        memset(&req_buff, 0, sizeof(struct v4l2_requestbuffers));
        req_buff.count = MAX_BUF_NUMS;               /*预设要申请4个缓冲区*/
        req_buff.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; /*视频捕获设备*/
        req_buff.memory = V4L2_MEMORY_USERPTR;       /*支持mmap内存映射*/
        if (ioctl(m_fd, VIDIOC_REQBUFS, &req_buff)) {
            printf("申请摄像头数据采集的缓冲区失败!\n");
            return -4;
        }
        printf("摄像头缓冲区申请的数量: %d\n", req_buff.count);
        /*4. 自定义缓冲区*/
        struct v4l2_buffer buff_info;
        memset(&buff_info, 0, sizeof(struct v4l2_buffer));
        for (int i = 0; i < MAX_BUF_NUMS; i++) {
            m_cma_buffer[i].size = jpeg_buff_common_size(); // 这里缓存是JPEG bitstream的
            cma_mem_alloc(&m_cma_buffer[i]);
            image_buffer[i] = (uint8_t *)m_cma_buffer[i].virt_addr;
            buff_info.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; /*视频捕获设备*/
            buff_info.index = i;                          /*缓冲区的节点编号*/
            buff_info.memory = V4L2_MEMORY_USERPTR;       /*支持mmap内存映射*/
            buff_info.m.userptr = (unsigned long)image_buffer[i];
            buff_info.length = jpeg_buff_common_size();
            int err = ioctl(m_fd, VIDIOC_QBUF, &buff_info); // 这里会返回-1，应该是不支持自定义cma的缓存区，用malloc的可以
            if (err) {
                printf("根据节点编号将缓冲区放入队列失败! V4L2_MEMORY_USERPTR MODE\n");
                return -7;
            }
        }
    }

    /*6. 启动摄像头数据采集*/
    int Type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(m_fd, VIDIOC_STREAMON, &Type)) {
        printf("启动摄像头数据采集失败!\n");
        return -8;
    }
    return 0;
}

void stream_input::open(const std::string &media, int w, int h) {
    if (media.find(".mjpeg") != std::string::npos
        || media.find(".mjpg") != std::string::npos
        || media.find(".jpeg") != std::string::npos
        || media.find(".jpg") != std::string::npos) {
        m_is_dev_video = false;
    } else if (media.find("/dev/video") != std::string::npos) {
        m_is_dev_video = true;
    } else {
        LOG_ERROR("unsupported media:{}", media);
        return;
    }

    if (m_is_dev_video) {
        image_height = h;
        image_width = w;
        int err = VideoDeviceInit(media.c_str(), 1);
        if (err != 0) {
            LOG_ERROR("open video device({}) failed:{}", media, err);
            return;
        }
        m_is_open = true;
    } else {
        m_url.open(media, std::ios::binary | std::ios::in);
        if (!m_url.is_open()) {
            LOG_ERROR("open mjpeg stream({}) failed", media);
            return;
        }
        if (user_def_buffer_mjpeg) {
            m_cma_buffer[0].size = jpeg_buff_common_size();
            m_cma_buffer[0].size += 4096; // jpu bug,会多读写缓存区
            cma_mem_alloc(&m_cma_buffer[0]);
            m_cma_buffer[0].size -= 4096;
        }
        m_is_open = true;
    }
}
std::pair<std::shared_ptr<uint8_t>, size_t> stream_input::read_frame(const std::vector<unsigned char> &wrap_data) {
    if (!m_is_open) {
        return {nullptr, 0};
    }

    if (m_is_dev_video) {
        if (m_qbuff_count == 0) {
            LOG_ERROR("usb camera buffer is empty");
            return {nullptr, 0};
        }
        struct pollfd fds;
        fds.fd = m_fd;
        fds.events = POLLIN;
        auto ret = poll(&fds, 1, -1);
        if (ret <= 0) {
            LOG_ERROR("USB camera maybe closed,witch poll error:{}", ret);
            m_eof = true;
            return {nullptr, 0};
        }
        m_qbuff_count--;
        struct v4l2_buffer buff_info;
        buff_info.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; /*视频捕获设备*/
        buff_info.memory = V4L2_MEMORY_MMAP;
        if (ioctl(m_fd, VIDIOC_DQBUF, &buff_info)) {
            LOG_ERROR("从缓冲区取出数据失败!");
            m_eof = true;
            return {nullptr, 0};
        }
        LOG_VERBOSE("image_buffer[{}]={} size:{} \n", buff_info.index, image_buffer[buff_info.index], buff_info.bytesused);
        return {
            std::shared_ptr<uint8_t>(
                image_buffer[buff_info.index], [buff_info, this](uint8_t *p) {
                    ioctl(m_fd, VIDIOC_QBUF, &buff_info);
                    m_qbuff_count++;
                }),
            buff_info.bytesused};
    } else {
        std::shared_ptr<uint8_t> ptr;
        size_t n;
        if (user_def_buffer_mjpeg) {
            ptr = std::shared_ptr<uint8_t>(
                (uint8_t *)m_cma_buffer[0].virt_addr, [](uint8_t *p) {
                    // do nothing
                });
            n = m_cma_buffer[0].size;
        } else {
            ptr = std::shared_ptr<uint8_t>(
                (uint8_t *)malloc(128 * 1024), [](uint8_t *p) {
                    free(p);
                });
            n = 128 * 1024;
        }
        if (!wrap_data.empty()) {
            memcpy((char *)ptr.get(), wrap_data.data(), wrap_data.size());
            m_url.read((char *)ptr.get() + wrap_data.size(), n - wrap_data.size());
            n = wrap_data.size() + m_url.gcount();
        } else {
            m_url.read((char *)ptr.get(), n);
            n = m_url.gcount();
        }
        m_eof = m_url.eof() || m_url.fail();
        return {ptr, (size_t)n};
    }
}
void stream_input::close() {
    if (m_is_open) {
        if (m_is_dev_video) {
            // 关闭摄像头数据采集
            int Type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(m_fd, VIDIOC_STREAMOFF, &Type)) {
                LOG_ERROR("关闭摄像头数据采集失败!");
            }
            ::close(m_fd);
        } else {
            m_url.close();
        }
        m_is_open = false;
    }
}

// 下面是jpu的部分

#ifndef MIN
#define MIN(a, b) ((a) > (b) ? (b) : (a))
#endif

#ifndef MAX
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#endif

#ifndef container_of
#define container_of(ptr, type, member)                           \
    __extension__({                                               \
        const __typeof__(((type *)0)->member) *__pmember = (ptr); \
        (type *)((char *)__pmember - offsetof(type, member));     \
    })
#endif
#define _FORCE_THIS container_of(p, jpu_cma_buffer, base)

void jpu_cma_buffer::DeInit(struct JW_BuffHandleBase *p) {
    jpu_cma_buffer *P = _FORCE_THIS;
    P->~jpu_cma_buffer();
}
PhysicalAddress jpu_cma_buffer::GetPhysAddr(struct JW_BuffHandleBase *p) {
    jpu_cma_buffer *P = _FORCE_THIS;
    return P->cma.phys_addr;
}
void *jpu_cma_buffer::GetVirtAddr(struct JW_BuffHandleBase *p) {
    jpu_cma_buffer *P = _FORCE_THIS;
    return (void *)P->cma.virt_addr;
}
Uint32 jpu_cma_buffer::GetSize(struct JW_BuffHandleBase *p) {
    jpu_cma_buffer *P = _FORCE_THIS;
    return P->cma.size;
}
Uint32 jpu_cma_buffer::Reserve(struct JW_BuffHandleBase *p, Uint32 new_cap_size) {
    jpu_cma_buffer *P = _FORCE_THIS;
    // 因为有import的情况，所以不允许Reserve
    if (P->cma.flags == 0xf && new_cap_size > P->cma.size) {
        LOG_ERROR("new_cap_size:{} is larger than current size:{}", new_cap_size, P->cma.size);
        throw std::runtime_error("new_cap_size is larger than current size");
    }
    if (P->cma.size < new_cap_size) {
        P->reserve(new_cap_size);
    }
    return P->cma.size;
}
Uint32 jpu_cma_buffer::FeedAct(struct JW_BuffHandleBase *p, Uint32 feed_size) {
    jpu_cma_buffer *P = _FORCE_THIS;
    feed_size = MIN(feed_size, P->cma.size - P->rdptr);
    P->rdptr += feed_size;
    if (P->feedCallback) {
        P->feedCallback(P, feed_size);
    }
    return feed_size;
}
Uint32 jpu_cma_buffer::WriteAct(struct JW_BuffHandleBase *p, Uint32 write_size) {
    jpu_cma_buffer *P = _FORCE_THIS;
    write_size = MIN(write_size, P->cma.size - P->rdptr);
    P->rdptr += write_size;
    if (P->writeCallback) {
        P->writeCallback(P, write_size);
    }
    return write_size;
}
Uint32 jpu_cma_buffer::GetWriteSize(struct JW_BuffHandleBase *p) {
    jpu_cma_buffer *P = _FORCE_THIS;
    return P->rdptr;
}
void jpu_cma_buffer::ResetWrRdPtr(struct JW_BuffHandleBase *p, BOOL update_mem_offset) {
    jpu_cma_buffer *P = _FORCE_THIS;
    P->rdptr = 0;
}
void *jpu_cma_buffer::HeaderWrite(struct JW_BuffHandleBase *p, Uint32 reserve_size) {
    jpu_cma_buffer *P = _FORCE_THIS;
    throw std::runtime_error("not implemented");
    return nullptr;
}
void jpu_cma_buffer::HeaderWriteFinish(struct JW_BuffHandleBase *p, Uint32 write_size) {
    jpu_cma_buffer *P = _FORCE_THIS;
    throw std::runtime_error("not implemented");
}

struct JW_BuffHandleVtable jpu_cma_buffer::vtable = {
    DeInit,
    GetPhysAddr,
    GetVirtAddr,
    GetSize,
    Reserve,
    FeedAct,
    WriteAct,
    GetWriteSize,
    ResetWrRdPtr,
    HeaderWrite,
    HeaderWriteFinish,
};

void jpu_decoder_gift::emplace_jpeg_bits_from(stream_input &r) {
    auto [data, size] = r.read_frame(m_jpu_wrap_data);
    m_jpu_wrap_data.clear();
    if (data == nullptr || size == 0) {
        return;
    }
    cma_buffer_t bf;
    if (cma_mem_search_buffer(bf, (void *)data.get())) {
        bitfeed.import(bf);
        // import之后 bitfeed能获得buffer的真正大小，但考虑到jpu的bug，这里按照约定使用read_frame返回的大小
        bitfeed.cma.size = size;
        cma_mem_sync_for_device(bitfeed.cma.phys_addr, size);
        bitfeed.rdptr = size;
    } else {
        // bitfeed不允许二次reserve，因为物理地址会被锁定在JPU中
        bitfeed.reserve(jpeg_buff_common_size()); // 这里的大小是jpeg bitstream永远都不会超过的大小
        if (size > bitfeed.cma.size) {
            LOG_ERROR("bitfeed size is not enough");
            throw std::runtime_error("bitfeed size is not enough");
            return;
        }
        memcpy(bitfeed.cma.virt_addr, data.get(), size);
        cma_mem_sync_for_device(bitfeed.cma.phys_addr, size);
        bitfeed.rdptr = size;
    }
    m_is_dev_video = r.is_dev_video();
}

bool jpu_decoder_gift::InitAndDecode() {
    if (!JPU_IsInit()) JW_Init();
    JW_DecoderContextSetDefault(&context);
    context.decOP.outputFormat = FORMAT_422;
    context.decOP.chromaInterleave = CBCR_SEPARATED;
    context.decOP.packedFormat = PACKED_FORMAT_422_YUYV;
    context.bsFeed = &bitfeed.base;
    auto ret = JW_DecoderInit(&context);
    if (ret != JPG_RET_SUCCESS) {
        LOG_ERROR("JW_DecoderInit failed:{}", ret);
        return false;
    }
    JW_DecoderRegisterFrameBuffer(&context, &yuvwrite->base);
    auto success = JW_DecoderAct(&context);
    if (!success) {
        LOG_ERROR("JW_DecoderAct Failed at first time ");
        m_is_open = false;
        JW_DecoderDeInit(&context);
        return false;
        // throw std::runtime_error("JW_DecoderAct Failed at first time ");
    }
    int_reason = 0;
    // TODO 检查实际解码的图像大小和设置大小是否一样
    LOG_INFO("finally decoder output:{}x{}", context.initialInfo.picWidth, context.initialInfo.picHeight);
    return success;
}

bool jpu_decoder_gift::Decode() {
    // 处理 缓存边界问题
    if (m_jpu_wrap_mode_status == 1) {
        m_jpu_wrap_mode_status = 2;
        return false;
    }
    // 这是根据 int_reason的状态机
    if (int_reason == 0 || context.ret == JPG_RET_BIT_EMPTY) {
        //
        if (m_is_dev_video) {
            if (context.outputInfo.consumedByte != 0) {
                context.outputInfo.consumedByte = 0;
                auto pDecInfo = &context.handle->JpgInfo->decInfo;
                pDecInfo->streamWrPtr = pDecInfo->streamBufStartAddr;
                pDecInfo->consumeByte = 0;
                pDecInfo->frameOffset = 0;
                context.ret = JPG_RET_BIT_EMPTY;
                return false;
            }
        }
        if (m_jpu_wrap_mode_status == 2) {
            m_jpu_wrap_mode_status = 0;
            context.outputInfo.consumedByte = 0;
            auto pDecInfo = &context.handle->JpgInfo->decInfo;
            pDecInfo->streamWrPtr = pDecInfo->streamBufStartAddr;
            pDecInfo->streamRdPtr = pDecInfo->streamBufStartAddr;
            pDecInfo->consumeByte = 0;
            pDecInfo->frameOffset = 0;
        }

        // 常规状态，上次解码已经完成
        auto ret_bak = context.ret;
        JW_DecoderRegisterFrameBuffer(&context, &yuvwrite->base);
        if (ret_bak == JPG_RET_BIT_EMPTY) {
            JPU_DecUpdateBitstreamBuffer(context.handle, bitfeed.rdptr);
            JpuWriteInstReg(context.handle->instIndex, MJPEG_BBC_RD_PTR_REG, context.bsFeed->vtable->GetPhysAddr(context.bsFeed));
        }
        context.ret = JDEC_DecStartOneFrame(context.handle, &context.decParam);
        if (context.ret == JPG_RET_BIT_EMPTY) {
            // 周到这个分支如果不抛异常可能会进入到异常的循环中
            if (!m_is_dev_video)
                throw std::runtime_error(" JPU_DecStartOneFrame failed,BitStream is Empty \r\n");
            return false;
        }
        if (context.ret != JPG_RET_SUCCESS) {
            LOG_ERROR("JPU_DecStartOneFrame failed Error code is 0x{:x} \n", context.ret);
            throw std::runtime_error(fmt::format("JPU_DecStartOneFrame failed Error code is 0x{:x} \n", context.ret));
            return false;
        }
    } else {
        if (bitfeed.rdptr == 0) {
            LOG_ERROR("bitfeed.rdptr is 0 (No data in bitfeed)");
            throw std::runtime_error("bitfeed.rdptr is 0 (No data in bitfeed)");
        }
        JpgLeaveLock();
        // return InitAndDecode();
        // JPU_DecSetRdPtrEx(context.handle, context.bsFeed->vtable->GetPhysAddr(context.bsFeed), TRUE);
        // JPU_DecUpdateBitstreamBuffer(context.handle, (bitfeed.rdptr) % 256 == 0 ? bitfeed.rdptr + 255 : bitfeed.rdptr);
        JPU_DecUpdateBitstreamBuffer(context.handle, bitfeed.rdptr);
        // JpuWriteInstReg(context.handle->instIndex, MJPEG_BBC_RD_PTR_REG,
        //  context.bsFeed->vtable->GetPhysAddr(context.bsFeed)); // 这一行不知道要不要😢
        JPU_ClrStatus(context.handle, (1 << INT_JPU_BIT_BUF_EMPTY));
    }

    int_reason = JPU_WaitInterrupt(context.handle, 500);
    if (int_reason == -1) {
        context.ret = JPU_DecGetOutputInfo(context.handle, &context.outputInfo);
        LOG_ERROR("JPU_WaitInterrupt timeout");
        if (m_is_dev_video)
            throw std::runtime_error("JPU_WaitInterrupt timeout");
        // 在切换缓存块的时候容易出现超时的情况，这时候没救，回滚初始状态然后寻找当前缓存的开头发送的wrap_data
        auto pDecInfo = &context.handle->JpgInfo->decInfo;
        size_t offset = pDecInfo->streamBufSize;
        for (size_t i = 0; i < pDecInfo->streamBufSize - 1; i++) {
            if (pDecInfo->pBitStream[i] == 0xFF && pDecInfo->pBitStream[i + 1] == 0xD8) {
                offset = i;
                break;
            }
        }
        auto copy_size = pDecInfo->streamBufSize - offset;
        m_jpu_wrap_data.resize(copy_size);
        memcpy(m_jpu_wrap_data.data(), (char *)bitfeed.cma.virt_addr + offset, copy_size);
        JW_DecoderDeInit(&context);
        m_is_open = false;
        return false;
    } else if (int_reason == -2) {
        LOG_ERROR("Interrupt occurred. but this interrupt is not for my instance");
        throw std::runtime_error(fmt::format("Interrupt occurred. but this interrupt is not for my instance"));
    } else if (int_reason & ((1 << INT_JPU_ERROR))) {
        LOG_ERROR("\tINSTANCE #{} int_reason: {}", context.handle->instIndex, int_reason);

        context.ret = JPU_DecGetOutputInfo(context.handle, &context.outputInfo);
        rdPtr = JpuReadInstReg(context.handle->instIndex, MJPEG_BBC_RD_PTR_REG);
        wrPtr = JpuReadInstReg(context.handle->instIndex, MJPEG_BBC_WR_PTR_REG);
        LOG_INFO("bitstream buffer empty, rdPtr:{:x}, wrPtr:{:x}", rdPtr, wrPtr);
        LOG_INFO("cmabuf , rdPtr:{:x}, wrPtr:{:x}", bitfeed.cma.phys_addr + bitfeed.rdptr, bitfeed.cma.phys_addr);
        if (context.outputInfo.numOfErrMBs) {
            Int32 errRstIdx, errPosX, errPosY;
            errRstIdx = (context.outputInfo.numOfErrMBs & 0x0F000000) >> 24;
            errPosX = (context.outputInfo.numOfErrMBs & 0x00FFF000) >> 12;
            errPosY = (context.outputInfo.numOfErrMBs & 0x00000FFF);
            printf("Error restart Idx : %d, MCU x:%d, y:%d, in Frame : %d \r\n", errRstIdx, errPosX, errPosY, context.frameIdx);
        }
        JW_DecoderDeInit(&context);
        m_is_open = false;
        return false;
        // throw std::runtime_error(fmt::format("\tINSTANCE #{} int_reason: {}", context.handle->instIndex, int_reason));
    } else if (int_reason & ((1 << INT_JPU_BIT_BUF_EMPTY))) {
        rdPtr = JpuReadInstReg(context.handle->instIndex, MJPEG_BBC_RD_PTR_REG);
        wrPtr = JpuReadInstReg(context.handle->instIndex, MJPEG_BBC_WR_PTR_REG);
        LOG_VERBOSE("bitstream buffer empty, rdPtr:{:x}, wrPtr:{:x}", rdPtr, wrPtr);
        LOG_VERBOSE("cmabuf , rdPtr:{:x}, wrPtr:{:x}", bitfeed.cma.phys_addr + bitfeed.rdptr, bitfeed.cma.phys_addr);
        bitfeed.rdptr = 0; // 标记为空
        // JGIFT_DecGetOutputInfo(context.handle, &context.outputInfo);
        return false;
    } else if (int_reason & ((1 << INT_JPU_DONE))) {
        int_reason = 0; // 清0
        context.ret = JPU_DecGetOutputInfo(context.handle, &context.outputInfo);
        LOG_VERBOSE("consumedByte = {}", context.outputInfo.consumedByte);
        // jpu不能处理在缓存边界上的一小部分图片数据，要把这部分收集起来拷贝到下一个缓存区域
        // 仅mjpeg模式需要处理
        if (!m_is_dev_video) {
            auto pDecInfo = &context.handle->JpgInfo->decInfo;
            auto cur_ptr = pDecInfo->frameOffset + pDecInfo->consumeByte;
            if (cur_ptr + 2048 >= pDecInfo->streamBufSize && cur_ptr < pDecInfo->streamBufSize) {
                auto copy_size = pDecInfo->streamBufSize - cur_ptr;
                m_jpu_wrap_data.resize(copy_size);
                memcpy(m_jpu_wrap_data.data(), (char *)bitfeed.cma.virt_addr + cur_ptr, copy_size);
                m_jpu_wrap_mode_status = 1;
            }
        }
        // USB摄像头的每次read_frame都是一张完整、独立的MJPEG图像。
        // 解码完成后必须把输入标记为空，否则下一轮会反复解码并向网页
        // 发布同一张旧JPEG，表现为推理在运行但网页画面永远停在首帧。
        if (m_is_dev_video) {
            bitfeed.rdptr = 0;
        }
        return true;
    }
    return false;
}

JpgRet jpu_decoder_gift::JDEC_DecStartOneFrame(JpgDecHandle handle, JpgDecParam *param) {
    JpgInst *pJpgInst;
    JpgDecInfo *pDecInfo;
    JpgRet ret;
    Uint32 val;
    PhysicalAddress rdPtr, wrPtr;
    BOOL is12Bit = FALSE;
    BOOL ppuEnable = FALSE;
    Int32 instRegIndex;
    BOOL bTableInfoUpdate;
    Uint32 strmCntOfEos;
    ret = CheckJpgInstValidity(handle);
    if (ret != JPG_RET_SUCCESS)
        return ret;

    pJpgInst = handle;
    pDecInfo = &pJpgInst->JpgInfo->decInfo;
    is12Bit = (pDecInfo->bitDepth == 8) ? FALSE : TRUE;

    if (pDecInfo->frameBufPool == 0) { // This means frame buffers have not been registered.
        return JPG_RET_WRONG_CALL_SEQUENCE;
    }

    if (pJpgInst->sliceInstMode == TRUE) {
        instRegIndex = pJpgInst->instIndex;
    } else {
        instRegIndex = 0;
    }

    JpgEnterLock();
    if (GetJpgPendingInstEx(pJpgInst->instIndex) == pJpgInst) {
        JpgLeaveLock();
        return JPG_RET_FRAME_NOT_COMPLETE;
    }

    if (pDecInfo->frameOffset < 0) {
        SetJpgPendingInstEx(0, pJpgInst->instIndex);
        return JPG_RET_EOS;
    }

    pDecInfo->q_prec0 = 0;
    pDecInfo->q_prec1 = 0;
    pDecInfo->q_prec2 = 0;
    pDecInfo->q_prec3 = 0;

    // check for stream empty case
    if (pDecInfo->streamEndflag == 0) {
        rdPtr = pDecInfo->streamRdPtr;
        wrPtr = pDecInfo->streamWrPtr;
        if (wrPtr == pDecInfo->streamBufStartAddr)
            wrPtr = pDecInfo->streamBufEndAddr;
        if (rdPtr > wrPtr) { // wrap-around case
        } else {
            if (wrPtr - rdPtr < JPU_GBU_WRAP_SIZE) {
                JpgLeaveLock();
                return JPG_RET_BIT_EMPTY;
            }
        }
    }

    {
        // 这里JpegDecodeHeader有bug，不能处理环绕的情况
        if (pDecInfo->frameOffset + pDecInfo->consumeByte > pDecInfo->streamBufSize) {
            pDecInfo->frameOffset -= pDecInfo->streamBufSize;
        }
        val = JpegDecodeHeader(pDecInfo);
        // if (val == -1) {
        //     pDecInfo->frameOffset = 0;
        //     pDecInfo->consumeByte = 0;
        //     val = JpegDecodeHeader(pDecInfo);
        // }
        if (val == 0) {
            JpgLeaveLock();
            return JPG_RET_FAILURE;
        }

        if (val == (Uint32)-4) {
            JpgLeaveLock();
            return JPG_RET_INSUFFICIENT_BITSTREAM_BUFFER; // bitstream buffer siz is not enough.
        }
        if (val == (Uint32)-2) { // wrap around case
            pDecInfo->frameOffset = 0;
            pDecInfo->ecsPtr = 0;
            val = JpegDecodeHeader(pDecInfo);
            if (val == 0) {
                JpgLeaveLock();
                return JPG_RET_FAILURE;
            }
        }

        if (val == (Uint32)-1) { // stream empty case
            if (pDecInfo->streamEndflag == 1) {
                SetJpgPendingInstEx(0, pJpgInst->instIndex);
                pDecInfo->frameOffset = -1;
                if (pJpgInst->sliceInstMode == TRUE) {
                    JpgLeaveLock();
                }
                return JPG_RET_EOS;
            }
            JpgLeaveLock();
            return JPG_RET_BIT_EMPTY;
        }
        if (val == (Uint32)-4) {
            JpgLeaveLock();
            return JPG_RET_INSUFFICIENT_BITSTREAM_BUFFER;
        }
    }

    JpuWriteInstReg(instRegIndex, MJPEG_INTR_MASK_REG, ((~pDecInfo->intrEnableBit) & 0x3ff));
    /* The registers related to the slice encoding should be clear */
    JpuWriteInstReg(instRegIndex, MJPEG_SLICE_INFO_REG, pDecInfo->alignedHeight);
    JpuWriteInstReg(instRegIndex, MJPEG_SLICE_DPB_POS_REG, pDecInfo->alignedHeight);
    JpuWriteInstReg(instRegIndex, MJPEG_SLICE_POS_REG, 0);
    JpuWriteInstReg(instRegIndex, MJPEG_PIC_SETMB_REG, 0);

    if (pDecInfo->streamRdPtr == pDecInfo->streamBufEndAddr) {
        JpuWriteInstReg(instRegIndex, MJPEG_BBC_CUR_POS_REG, 0);
        JpuWriteInstReg(instRegIndex, MJPEG_GBU_TCNT_REG, 0);
        JpuWriteInstReg(instRegIndex, (MJPEG_GBU_TCNT_REG + 4), 0);
    }

    JpuWriteInstReg(instRegIndex, MJPEG_BBC_WR_PTR_REG, pDecInfo->streamWrPtr);
    if (pDecInfo->streamWrPtr == pDecInfo->streamBufStartAddr) {
        JpuWriteInstReg(instRegIndex, MJPEG_BBC_END_ADDR_REG, pDecInfo->streamBufEndAddr);
    } else {
        JpuWriteInstReg(instRegIndex, MJPEG_BBC_END_ADDR_REG, JPU_CEIL(256, pDecInfo->streamWrPtr));
    }

    JpuWriteInstReg(instRegIndex, MJPEG_BBC_BAS_ADDR_REG, pDecInfo->streamBufStartAddr);
    JpuWriteInstReg(instRegIndex, MJPEG_GBU_TCNT_REG, 0);
    JpuWriteInstReg(instRegIndex, (MJPEG_GBU_TCNT_REG + 4), 0);
    JpuWriteInstReg(instRegIndex, MJPEG_PIC_ERRMB_REG, 0);

    JpuWriteInstReg(instRegIndex, MJPEG_PIC_CTRL_REG, is12Bit << 31 | pDecInfo->q_prec0 << 30 | pDecInfo->q_prec1 << 29 | pDecInfo->q_prec2 << 28 | pDecInfo->q_prec3 << 27 | pDecInfo->huffAcIdx << 13 | pDecInfo->huffDcIdx << 7 | pDecInfo->userHuffTab << 6 | (JPU_CHECK_WRITE_RESPONSE_BVALID_SIGNAL << 2) | 0);

    JpuWriteInstReg(instRegIndex, MJPEG_PIC_SIZE_REG, (pDecInfo->alignedWidth << 16) | pDecInfo->alignedHeight);

    JpuWriteInstReg(instRegIndex, MJPEG_OP_INFO_REG, pDecInfo->busReqNum);
    JpuWriteInstReg(instRegIndex, MJPEG_MCU_INFO_REG, (pDecInfo->mcuBlockNum & 0x0f) << 17 | (pDecInfo->compNum & 0x07) << 14 | (pDecInfo->compInfo[0] & 0x3f) << 8 | (pDecInfo->compInfo[1] & 0x0f) << 4 | (pDecInfo->compInfo[2] & 0x0f));

    // enable intlv NV12: 10, NV21: 11
    // packedFormat:0 => 4'd0
    // packedFormat:1,2,3,4 => 4, 5, 6, 7,
    // packedFormat:5 => 8
    // packedFormat:6 => 9
    val = (pDecInfo->ofmt << 9) | (pDecInfo->frameEndian << 6) | ((pDecInfo->chromaInterleave == 0) ? 0 : (pDecInfo->chromaInterleave == 1) ? 2 :
                                                                                                                                              3);
    if (pDecInfo->packedFormat == PACKED_FORMAT_NONE) {
        val |= (0 << 5) | (0 << 4);
    } else if (pDecInfo->packedFormat == PACKED_FORMAT_444) {
        val |= (1 << 5) | (0 << 4) | (0 << 2);
    } else {
        val |= (0 << 5) | (1 << 4) | ((pDecInfo->packedFormat - 1) << 2);
    }
    val |= (pDecInfo->pixelJustification << 11);
    JpuWriteInstReg(instRegIndex, MJPEG_DPB_CONFIG_REG, val);
    JpuWriteInstReg(instRegIndex, MJPEG_RST_INTVAL_REG, pDecInfo->rstIntval);

    if (param) {
        if (param->scaleDownRatioWidth > 0)
            pDecInfo->iHorScaleMode = param->scaleDownRatioWidth;
        if (param->scaleDownRatioHeight > 0)
            pDecInfo->iVerScaleMode = param->scaleDownRatioHeight;
    }
    if (pDecInfo->iHorScaleMode | pDecInfo->iVerScaleMode)
        val = ((pDecInfo->iHorScaleMode & 0x3) << 2) | ((pDecInfo->iVerScaleMode & 0x3)) | 0x10;
    else {
        val = 0;
    }
    JpuWriteInstReg(instRegIndex, MJPEG_SCL_INFO_REG, val);

    bTableInfoUpdate = FALSE;
    if (pDecInfo->userHuffTab) {
        bTableInfoUpdate = TRUE;
    }

    if (bTableInfoUpdate == TRUE) {
        if (is12Bit == TRUE) {
            if (!JpgDecHuffTabSetUp_12b(pDecInfo, instRegIndex)) {
                JpgLeaveLock();
                return JPG_RET_INVALID_PARAM;
            }
        } else {
            if (!JpgDecHuffTabSetUp(pDecInfo, instRegIndex)) {
                JpgLeaveLock();
                return JPG_RET_INVALID_PARAM;
            }
        }
    }

    bTableInfoUpdate = TRUE; // it always should be TRUE for multi-instance
    if (bTableInfoUpdate == TRUE) {
        if (!JpgDecQMatTabSetUp(pDecInfo, instRegIndex)) {
            JpgLeaveLock();
            return JPG_RET_INVALID_PARAM;
        }
    }

    JpgDecGramSetup(pDecInfo, instRegIndex);

    if (pDecInfo->streamEndflag == 1) {
        val = JpuReadInstReg(instRegIndex, MJPEG_BBC_STRM_CTRL_REG);
        if ((val & (1UL << 31)) == 0) {
            strmCntOfEos = (pDecInfo->streamWrPtr - pDecInfo->streamBufStartAddr) / 256;
            if ((pDecInfo->streamWrPtr - pDecInfo->streamBufStartAddr) % 256) {
                strmCntOfEos = strmCntOfEos + 1;
            }
            val |= (1UL << 31);
            val &= ~(0xffffff);
            val |= (strmCntOfEos & 0xffffff);
            JpuWriteInstReg(instRegIndex, MJPEG_BBC_STRM_CTRL_REG, val);
        }
    } else {
        JpuWriteInstReg(instRegIndex, MJPEG_BBC_STRM_CTRL_REG, 0);
    }

    JpuWriteInstReg(instRegIndex, MJPEG_RST_INDEX_REG, 0); // RST index at the beginning.
    JpuWriteInstReg(instRegIndex, MJPEG_RST_COUNT_REG, 0);

    JpuWriteInstReg(instRegIndex, MJPEG_DPCM_DIFF_Y_REG, 0);
    JpuWriteInstReg(instRegIndex, MJPEG_DPCM_DIFF_CB_REG, 0);
    JpuWriteInstReg(instRegIndex, MJPEG_DPCM_DIFF_CR_REG, 0);

    JpuWriteInstReg(instRegIndex, MJPEG_GBU_FF_RPTR_REG, pDecInfo->bitPtr);
    JpuWriteInstReg(instRegIndex, MJPEG_GBU_CTRL_REG, 3);

    ppuEnable = (pDecInfo->rotationIndex > 0) || (pDecInfo->mirrorIndex > 0);
    JpuWriteInstReg(instRegIndex, MJPEG_ROT_INFO_REG, (ppuEnable << 4) | (pDecInfo->mirrorIndex << 2) | pDecInfo->rotationIndex);

    val = (pDecInfo->frameIdx % pDecInfo->numFrameBuffers);
    JpuWriteInstReg(instRegIndex, MJPEG_DPB_BASE00_REG, pDecInfo->frameBufPool[val].bufY);
    JpuWriteInstReg(instRegIndex, MJPEG_DPB_BASE01_REG, pDecInfo->frameBufPool[val].bufCb);
    JpuWriteInstReg(instRegIndex, MJPEG_DPB_BASE02_REG, pDecInfo->frameBufPool[val].bufCr);
    JpuWriteInstReg(instRegIndex, MJPEG_DPB_YSTRIDE_REG, pDecInfo->stride);
    JpuWriteInstReg(instRegIndex, MJPEG_DPB_CSTRIDE_REG, pDecInfo->stride_c);

    if (pDecInfo->roiEnable) {
        JpuWriteInstReg(instRegIndex, MJPEG_CLP_INFO_REG, 1);
        JpuWriteInstReg(instRegIndex, MJPEG_CLP_BASE_REG, pDecInfo->roiOffsetX << 16 | pDecInfo->roiOffsetY);          // pixel unit
        JpuWriteInstReg(instRegIndex, MJPEG_CLP_SIZE_REG, (pDecInfo->roiFrameWidth << 16) | pDecInfo->roiFrameHeight); // pixel Unit
    } else {
        JpuWriteInstReg(instRegIndex, MJPEG_CLP_INFO_REG, 0);
    }

    JpuWriteInstReg(instRegIndex, MJPEG_PIC_STATUS_REG, 0x3ff);

    if (pJpgInst->loggingEnable)
        jdi_log(JDI_LOG_CMD_PICRUN, 1, instRegIndex);

    if (pJpgInst->sliceInstMode == TRUE) {
        JpuWriteInstReg(instRegIndex, MJPEG_PIC_START_REG, (1 << JPG_DEC_SLICE_ENABLE_START_PIC));
    } else {
        JpuWriteInstReg(instRegIndex, MJPEG_PIC_START_REG, (1 << JPG_ENABLE_START_PIC));
    }

    pDecInfo->decIdx++;

    SetJpgPendingInstEx(pJpgInst, pJpgInst->instIndex);
    if (pJpgInst->sliceInstMode == TRUE) {
        JpgLeaveLock();
    }
    return JPG_RET_SUCCESS;
}
