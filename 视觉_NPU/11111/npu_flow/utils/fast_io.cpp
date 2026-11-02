/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#include <cstdint>
#include <memory>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "fast_io.h"
class fast_io {
public:
    fast_io() = default;
    ~fast_io() {
        if (fd >= 0) {
            for (auto &i : regs_mmap) {
                munmap(i.second, 1U << 12);
            }
            close(fd);
        }
    }
    bool Init() {
        fd = open("/dev/mem", O_RDWR | O_NDELAY);
        if (fd < 0) {
            printf("open /dev/mem error!\n");
            return false;
        }
        return true;
    }

    uint32_t *fio(uint32_t phy_addr) {
        uint32_t key = (phy_addr) >> 12;
        uint32_t offst = (phy_addr) & ((1U << 12) - 1);
        if (
            regs_mmap.find(key) != regs_mmap.end()) {
            return regs_mmap[key] + offst / sizeof(uint32_t);
        } else {
            regs_mmap[key] = (uint32_t *)mmap(NULL, 1U << 12, PROT_READ | PROT_WRITE, MAP_SHARED, fd, key << 12);
            if (regs_mmap[key] == MAP_FAILED) {
                throw std::runtime_error("MAP_FAILED for fast_io");
            }
            return regs_mmap[key] + offst / sizeof(uint32_t);
        }
    }

private:
    std::map<uint32_t, uint32_t *> regs_mmap;

    int fd = -1;
};
static std::shared_ptr<fast_io> handle;

bool fast_io_init() {
    if (handle == nullptr) {
        handle.reset(new fast_io);
        if (!handle->Init()) {
            handle.reset();
        }
    }
    return handle != nullptr;
}

void fast_io_deinit() {
    handle.reset();
}

uint32_t *fio(uint32_t phy_addr) {
    if (handle != nullptr) {
        return handle->fio(phy_addr);
    } else {
        if (fast_io_init())
            return handle->fio(phy_addr);
    }
    return nullptr;
}