#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#ifdef WEB_SHARED_JPEG
using jpeg_frame = std::shared_ptr<const std::vector<uint8_t>>;

inline jpeg_frame make_jpeg_frame(const uint8_t *data, size_t size) {
    auto frame = std::make_shared<std::vector<uint8_t>>(size);
    if (size != 0) std::memcpy(frame->data(), data, size);
    return frame;
}

inline bool jpeg_frame_empty(const jpeg_frame &frame) {
    return !frame || frame->empty();
}

inline const std::vector<uint8_t> &jpeg_frame_bytes(const jpeg_frame &frame) {
    static const std::vector<uint8_t> empty;
    return frame ? *frame : empty;
}
#else
using jpeg_frame = std::vector<uint8_t>;

inline jpeg_frame make_jpeg_frame(const uint8_t *data, size_t size) {
    return jpeg_frame(data, data + size);
}

inline bool jpeg_frame_empty(const jpeg_frame &frame) {
    return frame.empty();
}

inline const std::vector<uint8_t> &jpeg_frame_bytes(const jpeg_frame &frame) {
    return frame;
}
#endif
