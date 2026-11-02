/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#include "flow_common_types.hpp"
#include "armnnALUtils/cma_mem_init.hpp"

CmaBuf::CmaBuf(size_t size_)
    : std::shared_ptr<cma_buffer_t>(new cma_buffer_t, [](cma_buffer_t *p) {
        if (p->phys_addr != 0)
            cma_mem_free(p);
        delete p;
    }) {
    get()->size = size_;
    get()->flags = 0;
    get()->phys_addr = 0;
    get()->base = 0;
    get()->virt_addr = 0;
    cma_mem_alloc(get());
}
void CmaBuf::sync_cpu() {
    if (get()->phys_addr != 0) {
        cma_mem_sync_for_cpu(get()->phys_addr, get()->size);
    }
}
void CmaBuf::sync_device() {
    if (get()->phys_addr != 0) {
        cma_mem_sync_for_device(get()->phys_addr, get()->size);
    }
}