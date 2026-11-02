/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#ifndef _FAST_IO_H_ASDA_
#define _FAST_IO_H_ASDA_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool fast_io_init();
void fast_io_deinit();

/**
 * @brief 通过/dev/mem映射物理地址到用户空间
 *
 * @param phy_addr 物理地址
 * @return uint32_t* 映射后的虚拟地址
 */
uint32_t *fio(uint32_t phy_addr);

#ifdef __cplusplus
}
#endif
#endif // !_FAST_IO_H_ASDA_