#pragma once

/*
本文件代码功能

1 提供解析服务器IP和UDP端口号的函数
2 格式化目标信息到相应的发送格式
3 处理时间戳
4 发送UDP数据包到指定服务器


*/

#include "armnnALUtils/DataProcess.hpp"
#include <cstdio>
#include <memory>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>

class udp_send {
public:
    /**
     * @brief

    发送数据示例
    {
        "objects": [
            {
                "label": 1,
                "prob": 0.95,
                "x": 100,
                "y": 150,
                "w": 50,
                "h": 80
            },
            ...
        ],
        "input_width": 640,
        "input_height": 480,
        "timestamp": 1625247600.123456
    }
     *
     * @param input_w
     * @param input_h
     * @param objects
     * @return true
     * @return false
     */
    bool send_objects(int input_w, int input_h,
                      const std::vector<DetectObject> &objects) {
        auto period_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - m_last_time).count();
        if (objects.size() == 0 && period_ms < 3000) {
            // 确保能发出空消息避免服务器没数据
            return true;
        }
        m_last_time = std::chrono::system_clock::now();
        // 格式化数据到发送缓冲区
        buffer_expand(1024);
        size_t offset = 0;
        offset += snprintf(&m_send_buffer[offset], m_send_buffer.size() - offset, "{\"objects\":[");
        for (size_t i = 0; i < objects.size(); i++) {
            const auto &obj = objects[i];
            offset += snprintf(&m_send_buffer[offset], m_send_buffer.size() - offset,
                               "{\"label\":%d,\"prob\":%.6f,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}%s",
                               obj.label, obj.prob, obj.x, obj.y, obj.w, obj.h,
                               (i == objects.size() - 1) ? "" : ",");
            buffer_expand(offset + 512); // 预留空间
        }
        offset += snprintf(&m_send_buffer[offset], m_send_buffer.size() - offset,
                           "],\"input_width\":%d,\"input_height\":%d,\"timestamp\":%.6f}",
                           input_w, input_h,
                           static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                   std::chrono::system_clock::now().time_since_epoch())
                                                   .count())
                               / 1e6);
        m_send_buffer[offset] = '\0';
        if (0) {
            printf("UDP Send Data: %s\n", m_send_buffer.data());
            return true;
        } else {
            // 发送UDP数据包
            ssize_t sent_bytes = sendto(m_socket_fd, m_send_buffer.data(), offset, 0,
                                        (struct sockaddr *)&m_dst_addr, sizeof(m_dst_addr));
            return sent_bytes == static_cast<ssize_t>(offset);
        }
    }

    static std::shared_ptr<udp_send> create_udp_sender(const std::string &server_addr_port) {
        // 解析IP和端口号
        size_t colon_pos = server_addr_port.find(':');
        if (colon_pos == std::string::npos) {
            throw std::invalid_argument("Invalid server address format. Expected <IP>:<PORT>");
        }
        std::string ip = server_addr_port.substr(0, colon_pos);
        int port = std::stoi(server_addr_port.substr(colon_pos + 1));

        // 创建UDP socket
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            goto ERROR_HANDLER;
        }

        // 设置服务器地址结构
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr) <= 0) {
            goto ERROR_HANDLER;
        }
        return std::make_shared<udp_send>(sockfd, server_addr);
    ERROR_HANDLER:
        perror("Error occurred while creating UDP sender");
        fprintf(stderr, "Error occurred while creating UDP sender。maybe Invalid server address format. Expected <IP>:<PORT> got %s\n", server_addr_port.c_str());
        if (sockfd >= 0) {
            close(sockfd);
            sockfd = -1;
        }
        return nullptr;
    }

    udp_send(int opened_socket_fd, struct sockaddr_in dst_addr)
        : m_socket_fd(opened_socket_fd)
        , m_dst_addr(dst_addr) {}

    ~udp_send() {
        if (m_socket_fd >= 0) {
            close(m_socket_fd);
            m_socket_fd = -1;
        }
    }

private:
    int m_socket_fd = -1;
    struct sockaddr_in m_dst_addr;
    std::vector<char> m_send_buffer;
    std::chrono::system_clock::time_point m_last_time;
    void buffer_expand(size_t required_size) {
        if (m_send_buffer.size() < required_size) {
            m_send_buffer.resize(required_size);
        }
    }
};
