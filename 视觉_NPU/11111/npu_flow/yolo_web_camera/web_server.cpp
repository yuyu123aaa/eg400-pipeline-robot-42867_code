#include "web_server.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

web_server::web_server(std::shared_ptr<web_state> state, unsigned short port, std::string document_root)
    : state_(std::move(state)), port_(port), document_root_(std::move(document_root)) {}

web_server::~web_server() {
    running_ = false;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
    }
    if (accept_thread_.joinable()) accept_thread_.join();
}

bool web_server::start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        listen(listen_fd_, 8) != 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    running_ = true;
    accept_thread_ = std::thread(&web_server::accept_loop, this);
    return true;
}

void web_server::accept_loop() {
    while (running_) {
        const int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) continue;
        std::thread(&web_server::handle_client, this, fd).detach();
    }
}

void web_server::handle_client(int client_fd) {
    char buffer[4096];
    const ssize_t got = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (got <= 0) { close(client_fd); return; }
    buffer[got] = '\0';
    std::istringstream first_line(buffer);
    std::string method, path, version;
    first_line >> method >> path >> version;
    const auto query = path.find('?');
    if (query != std::string::npos) path.resize(query);

    if (method != "GET") {
        send_response(client_fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "GET only\n");
    } else if (path == "/api/status") {
        send_response(client_fd, 200, "OK", "application/json; charset=utf-8", state_->status_json());
    } else if (path == "/api/detections") {
        send_response(client_fd, 200, "OK", "application/json; charset=utf-8", state_->detections_json());
    } else if (path == "/stream.mjpg") {
        serve_stream(client_fd);
        return;
    } else {
        serve_file(client_fd, path);
    }
    close(client_fd);
}

void web_server::serve_stream(int client_fd) {
    // 该板的WiFi驱动在多个1080P MJPEG连接并发发送时会持续占用
    // SUnreclaim内存。视觉演示只保留一个活动视频连接。
    if (stream_active_.exchange(true)) {
        send_response(client_fd, 503, "Busy", "text/plain; charset=utf-8",
                      "Only one video stream is allowed\n");
        close(client_fd);
        return;
    }
    // 浏览器或WiFi链路不再接收时，避免一个detach线程永久阻塞在send。
    timeval send_timeout{};
    send_timeout.tv_sec = 3;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    int send_buffer = 128 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
    const std::string header =
        "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nConnection: close\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
    if (!send_all(client_fd, header.data(), header.size())) {
        stream_active_ = false;
        close(client_fd);
        return;
    }
    uint64_t sent_sequence = 0;
    while (running_) {
        jpeg_frame jpeg;
        uint64_t sequence = 0;
        if (!state_->latest_frame(jpeg, sequence) || sequence == sent_sequence) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        sent_sequence = sequence;
        const auto &jpeg_bytes = jpeg_frame_bytes(jpeg);
        std::ostringstream part;
        part << "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
             << jpeg_bytes.size() << "\r\n\r\n";
        const std::string part_header = part.str();
        if (!send_all(client_fd, part_header.data(), part_header.size()) ||
            !send_all(client_fd, jpeg_bytes.data(), jpeg_bytes.size()) ||
            !send_all(client_fd, "\r\n", 2)) break;
        // 网页限制约5fps；NPU仍独立保持原推理速度。
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    stream_active_ = false;
    close(client_fd);
}

void web_server::serve_file(int client_fd, const std::string &request_path) {
    std::string name;
    const char *type = "text/plain; charset=utf-8";
    if (request_path == "/" || request_path == "/index.html") {
        name = "index.html"; type = "text/html; charset=utf-8";
    } else if (request_path == "/app.js") {
        name = "app.js"; type = "application/javascript; charset=utf-8";
    } else if (request_path == "/style.css") {
        name = "style.css"; type = "text/css; charset=utf-8";
    } else {
        send_response(client_fd, 404, "Not Found", type, "Not found\n"); return;
    }
    std::ifstream file(document_root_ + "/" + name, std::ios::binary);
    if (!file) { send_response(client_fd, 404, "Not Found", type, "File missing\n"); return; }
    std::ostringstream body;
    body << file.rdbuf();
    send_response(client_fd, 200, "OK", type, body.str());
}

bool web_server::send_all(int fd, const void *data, size_t size) {
    const char *p = static_cast<const char *>(data);
    while (size) {
        const ssize_t sent = send(fd, p, size, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        p += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

void web_server::send_response(int fd, int code, const char *reason,
                               const char *content_type, const std::string &body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << code << ' ' << reason
        << "\r\nContent-Type: " << content_type
        << "\r\nContent-Length: " << body.size()
        << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    const std::string header = out.str();
    send_all(fd, header.data(), header.size());
    send_all(fd, body.data(), body.size());
}
