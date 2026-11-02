#include "web_state.hpp"
#include <cstdio>
#include <sstream>

void web_state::publish(const jpeg_frame &jpeg,
                        int camera_width, int camera_height,
                        int model_width, int model_height,
                        const std::vector<DetectObject> &objects) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_publish_.time_since_epoch().count() != 0) {
        const double seconds = std::chrono::duration<double>(now - last_publish_).count();
        if (seconds > 0.0) {
            const double instant_fps = 1.0 / seconds;
            pipeline_fps_ = pipeline_fps_ == 0.0 ? instant_fps : pipeline_fps_ * 0.85 + instant_fps * 0.15;
        }
    }
    last_publish_ = now;
    ++sequence_;
    jpeg_ = jpeg;
    camera_width_ = camera_width;
    camera_height_ = camera_height;
    model_width_ = model_width;
    model_height_ = model_height;
    detections_json_ = make_detections_json(sequence_, camera_width_, camera_height_,
                                             model_width_, model_height_, pipeline_fps_, objects);
}

bool web_state::latest_frame(jpeg_frame &jpeg, uint64_t &sequence) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (jpeg_frame_empty(jpeg_)) return false;
    jpeg = jpeg_;
    sequence = sequence_;
    return true;
}

std::string web_state::status_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "{\"ready\":%s,\"camera\":%s,\"npu_backend\":\"Alnpu\","
                  "\"sequence\":%llu,\"pipeline_fps\":%.2f,"
                  "\"camera_width\":%d,\"camera_height\":%d,"
                  "\"model_width\":%d,\"model_height\":%d}",
                  sequence_ ? "true" : "false", jpeg_frame_empty(jpeg_) ? "false" : "true",
                  static_cast<unsigned long long>(sequence_), pipeline_fps_,
                  camera_width_, camera_height_, model_width_, model_height_);
    return buffer;
}

std::string web_state::detections_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return detections_json_;
}

std::string web_state::make_detections_json(
    uint64_t sequence, int camera_width, int camera_height,
    int model_width, int model_height, double pipeline_fps,
    const std::vector<DetectObject> &objects) {
    std::ostringstream out;
    out << "{\"ready\":true,\"sequence\":" << sequence
        << ",\"pipeline_fps\":" << pipeline_fps
        << ",\"camera_width\":" << camera_width
        << ",\"camera_height\":" << camera_height
        << ",\"model_width\":" << model_width
        << ",\"model_height\":" << model_height
        << ",\"objects\":[";
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        if (i) out << ',';
        out << "{\"label\":" << obj.label
            << ",\"prob\":" << obj.prob
            << ",\"x\":" << obj.x
            << ",\"y\":" << obj.y
            << ",\"w\":" << obj.w
            << ",\"h\":" << obj.h << '}';
    }
    out << "]}";
    return out.str();
}
