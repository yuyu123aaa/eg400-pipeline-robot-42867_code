/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#ifndef AL_FLOW_COMMON_TYPES
#define AL_FLOW_COMMON_TYPES

#include <cstdint>
#include <memory>
#include <stdexcept>
#include "armnnALUtils/cma_mem.hpp"
#include "armnnALUtils/cma_mem_init.hpp"

#ifndef DISBALE_COPY_MOVE
#define DISBALE_COPY_MOVE(T)          \
    T(const T &) = delete;            \
    T(T &&) = delete;                 \
    T &operator=(const T &) = delete; \
    T &operator=(T &&) = delete;
#endif
#ifndef DISBALE_COPY
#define DISBALE_COPY(T)    \
    T(const T &) = delete; \
    T &operator=(T &&) = delete;
#endif

struct Size
{
    uint32_t m_width;
    uint32_t m_height;

    Size() : Size(0, 0) {}

    Size(uint32_t width, uint32_t height) :
            m_width{width}, m_height{height} {}

    Size(const Size& other)
            : Size(other.m_width, other.m_height) {}

    ~Size() = default;

    Size &operator=(const Size& other) = default;
};

/**
 * @brief cma_buffer management class
 * 
 */
class CmaBuf : public std::shared_ptr<cma_buffer_t> {
public:
    CmaBuf() = default;
    CmaBuf(size_t size_);
    using std::shared_ptr<cma_buffer_t>::operator=;

    /**
     * @brief synchronize data on physical address to virtual address
     * 
     */
    void sync_cpu();

    /**
     * @brief synchronize data on virtual address to physical address
     * 
     */
    void sync_device();
};

class CmaResourceZone {
public:
    DISBALE_COPY_MOVE(CmaResourceZone);

    CmaResourceZone() {
        error_code = cma_mem_init();
        if (error_code != 0) {
            throw std::runtime_error("cma mem init failed");
        }
    }
    ~CmaResourceZone() {
        if (0 == error_code) {
            cma_mem_deinit();
        }
    }

private:
    int error_code;
};

#endif 