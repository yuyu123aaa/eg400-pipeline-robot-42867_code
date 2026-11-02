/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#pragma once
#include <functional>
#include <mutex>
#include <queue>
#include <memory>
#include <thread>
#include <condition_variable>
// #define _BHVB5654145VHasdasVSHDGUVY48484_DIABLSE_THREAD
/// \brief 此队列用于多生产单消费
/// \tparam D 生产消费的数据类型
template <class SHARED_PTR, bool enable_threads = true>
class ThreadDataInProcessing : protected std::queue<SHARED_PTR> {
public:
    /// \brief
    /// \param call 工作接口
    /// \param product_limit 消费者这边允许堆积的货物量，最小为1
    explicit ThreadDataInProcessing(std::function<void(SHARED_PTR)> call, size_t product_limit = 1)
        : m_product_limit(std::max(product_limit, size_t(1UL))) {
        SetCall(std::move(call));
    }

    /// \brief
    /// \param product_limit 消费者这边允许堆积的货物量，最小为1
    explicit ThreadDataInProcessing(size_t product_limit = 1)
        : m_product_limit(std::max(product_limit, size_t(1UL))) {}

    /// \brief 发送生产完的数据
    /// \param data
    void mt_emplace(SHARED_PTR data) {
        if constexpr (enable_threads && global_enable_threads) {
            if (m_stop || (!m_call))
                return;
            else {
                std::unique_lock<std::mutex> lock(m_mutex);
                std::queue<SHARED_PTR>::emplace(data);
                m_cond.notify_one();
                m_cond_product.wait(lock, [this] { return m_stop || size() < m_product_limit; });
            }
        } else {
            if (m_call) {
                m_call(data);
            }
        }
    }

    /// \brief 剔除旧数据
    void mt_pop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        std::queue<SHARED_PTR>::pop();
    }

    /// \brief 设置消费数据的步骤
    /// \param call
    void SetCall(std::function<void(SHARED_PTR)> call) {
        m_call.swap(call);
        if constexpr (enable_threads && global_enable_threads) {
            m_thread = std::thread(&ThreadDataInProcessing::process, this);
        }
    }
    void close() {
        m_stop = true;
        m_cond.notify_all();
        m_cond_product.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }
    ~ThreadDataInProcessing() {
        close();
    }
    using std::queue<SHARED_PTR>::empty;
    using std::queue<SHARED_PTR>::size;

protected:
    void process() {
        while (true) {
            if (m_call) {
                SHARED_PTR data;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cond.wait(lock, [this] { return m_stop || !empty(); }); // 条件变量的wait函数 防止虚假唤醒
                    if (m_stop && empty()) return;
                    data = std::move(std::queue<SHARED_PTR>::front()); // 将队列的头部移动给task
                    std::queue<SHARED_PTR>::pop();                     // 记得出队列
                    m_cond_product.notify_one();
                }
                m_call(data);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    }

private:
    std::thread m_thread;
    std::function<void(SHARED_PTR)> m_call;
    std::condition_variable m_cond;
    std::condition_variable m_cond_product;
    std::mutex m_mutex;
    bool m_stop = false;
    size_t m_product_limit = 1;
#if defined(_BHVB5654145VHasdasVSHDGUVY48484_DIABLSE_THREAD) || !defined(__linux__)
    constexpr static bool global_enable_threads = false;
#else
    constexpr static bool global_enable_threads = true;
#endif
};