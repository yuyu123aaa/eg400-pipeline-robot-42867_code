#include "usb_camera.hpp"
#include "logger_forward.hpp"
#include "stream_input.hpp"
#include <chrono>

struct usb_camera_t::usb_camera_impl {
    usb_camera_t *P;
    usb_camera_impl(usb_camera_t *P_)
        : P(P_) {}

    stream_input stream_input_ins;
    jpu_decoder_gift gift;
    jpu_cma_buffer buf; // 输出buf，从外面导入
};

usb_camera_t::usb_camera_t()
    : impl(new usb_camera_impl(this)) {
}
usb_camera_t::usb_camera_t(int w, int h)
    : impl(new usb_camera_impl(this))
    , m_width(w)
    , m_height(h) {
    m_widthPad16 = (w + 15) / 16 * 16;
    m_heightPad16 = (h + 15) / 16 * 16;
}
usb_camera_t::~usb_camera_t() {
    impl.reset();
    open_err_code = -1;
}

int usb_camera_t::init(const char *dev_path) {
    impl->stream_input_ins.open(dev_path, m_width, m_height);
    if (impl->stream_input_ins.is_open()) {
        // LOG_INFO("stream_input open success");
        return 0;
    } else {
        LOG_ERROR("{} open failed", dev_path);
        return -1;
    }
}
void usb_camera_t::SubmitAndStart(uint64_t dma_rx_buf, void *rx_buf) {
    using clock = std::chrono::steady_clock;
    const auto total_begin = clock::now();
    std::chrono::microseconds feed_time(0);
    std::chrono::microseconds copy_time(0);
    std::chrono::microseconds decode_time(0);
    if (!impl->stream_input_ins.is_open() || impl->stream_input_ins.is_eof()) {
        error_code = -1;
        return;
    }
    cma_buffer_t bf;
    bf.phys_addr = dma_rx_buf;
    bf.size = buffer_size();
    impl->buf.import(bf);
    while (true) {
        if (impl->gift.get_bitfeed().rdptr == 0) {
            const auto begin = clock::now();
            impl->gift.emplace_jpeg_bits_from(impl->stream_input_ins);
            feed_time += std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - begin);
        }
        const auto &bitfeed = impl->gift.get_bitfeed();
        if (bitfeed.rdptr > 0) {
            const auto begin = clock::now();
#ifdef WEB_SHARED_JPEG
            auto latest = make_jpeg_frame(static_cast<const uint8_t *>(bitfeed.cma.virt_addr),
                                          static_cast<size_t>(bitfeed.rdptr));
            std::lock_guard<std::mutex> lock(m_jpeg_mutex);
            m_latest_jpeg = std::move(latest);
#else
            std::lock_guard<std::mutex> lock(m_jpeg_mutex);
            m_latest_jpeg.resize(static_cast<size_t>(bitfeed.rdptr));
            std::memcpy(m_latest_jpeg.data(), bitfeed.cma.virt_addr, m_latest_jpeg.size());
#endif
            copy_time += std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - begin);
        }
        const auto decode_begin = clock::now();
        const bool decoded = impl->gift.decode_frame_to(impl->buf);
        decode_time += std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - decode_begin);
        if (decoded) break;
        if (impl->stream_input_ins.is_eof()) {
            error_code = -1;
            return;
        }
    }
    error_code = 0;

    static uint64_t profile_count = 0;
    static std::chrono::microseconds profile_feed(0), profile_copy(0), profile_decode(0), profile_total(0);
    static auto profile_begin = clock::now();
    ++profile_count;
    profile_feed += feed_time;
    profile_copy += copy_time;
    profile_decode += decode_time;
    profile_total += std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - total_begin);
    if (clock::now() - profile_begin >= std::chrono::seconds(10)) {
        const double count = static_cast<double>(profile_count);
        LOG_INFO("CAMERA-STAGE avg: capture_feed={:.2f}ms jpeg_copy={:.2f}ms jpu_decode={:.2f}ms total={:.2f}ms frames={}",
                 profile_feed.count() / count / 1000.0,
                 profile_copy.count() / count / 1000.0,
                 profile_decode.count() / count / 1000.0,
                 profile_total.count() / count / 1000.0,
                 profile_count);
        profile_count = 0;
        profile_feed = profile_copy = profile_decode = profile_total = std::chrono::microseconds(0);
        profile_begin = clock::now();
    }
}

jpeg_frame usb_camera_t::GetJpegStream() {
    std::lock_guard<std::mutex> lock(m_jpeg_mutex);
    return m_latest_jpeg;
}

void usb_camera_t::SaveJpegStream(std::ostream &fs) {
    fs.write(reinterpret_cast<const char *>(impl->gift.get_bitfeed().cma.virt_addr), impl->gift.get_bitfeed().rdptr);
}

bool usb_camera_t::is_dev_video() const {
    return impl->stream_input_ins.is_dev_video();
}
