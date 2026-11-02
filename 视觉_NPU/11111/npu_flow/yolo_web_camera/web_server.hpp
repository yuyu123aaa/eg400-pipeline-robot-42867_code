#pragma once

#include "web_state.hpp"
#include <atomic>
#include <memory>
#include <string>
#include <thread>

class web_server {
public:
    web_server(std::shared_ptr<web_state> state, unsigned short port, std::string document_root);
    ~web_server();
    bool start();

private:
    void accept_loop();
    void handle_client(int client_fd);
    void serve_stream(int client_fd);
    void serve_file(int client_fd, const std::string &request_path);
    static bool send_all(int fd, const void *data, size_t size);
    static void send_response(int fd, int code, const char *reason,
                              const char *content_type, const std::string &body);

    std::shared_ptr<web_state> state_;
    unsigned short port_;
    std::string document_root_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stream_active_{false};
    int listen_fd_ = -1;
    std::thread accept_thread_;
};
