/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#ifdef __linux__
#ifndef __MIPICAM_MODULE_H__
#define __MIPICAM_MODULE_H__
#include <linux/fs.h>
#include <linux/types.h>
#ifndef __KERNEL__
#include <stdint.h>
#endif // !__KERNEL__

#define MIPICAM_IOCTL_MAGIC 'S'

#define MIPICAM_IOCTL_WAIT_INTERRUPT _IO(MIPICAM_IOCTL_MAGIC, 0)
#define MIPICAM_IOCTL_GET_REGISTER_INFO _IO(MIPICAM_IOCTL_MAGIC, 1)
#define MIPICAM_IOCTL_WRITE_REG _IO(MIPICAM_IOCTL_MAGIC, 2)
#define MIPICAM_IOCTL_READ_REG _IO(MIPICAM_IOCTL_MAGIC, 3)
#define MIPICAM_IOCTL_CLEAR_IRQ_REG_OFFSET _IO(MIPICAM_IOCTL_MAGIC, 4)

typedef struct __mipicam_intr_info {
	uint32_t timeout; // unit: ms
	uint32_t intr_flag;
	uint32_t work_status;
} mipicam_intr_info_t;

typedef struct __mipicam_rw_info_t {
	uint32_t addr;
	uint32_t data;
} mipicam_rw_info_t;

typedef struct __mipicam_reg_info_t {
	uint64_t addr;
	uint64_t size;
} mipicam_reg_info_t;

typedef struct mipicam_drv_buffer_t {
	unsigned int size;
	unsigned long phys_addr;
	void *virt_addr; /* virtual kernel space address */
} mipicam_drv_buffer_t;

#endif // !__MIPICAM_MODULE_H__
#endif
