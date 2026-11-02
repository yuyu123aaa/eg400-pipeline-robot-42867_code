/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * platform_info.h —— 平台相关常量（F3P / MLK-F3P-DR1M90GEG400, APU1 裸机侧）
 * ---------------------------------------------------------------------------
 * 这个文件是"两边对接"的核心，所有跨核约定的数字都在这里。
 */

#ifndef PLATFORM_INFO_H_
#define PLATFORM_INFO_H_

#include <openamp/remoteproc.h>
#include <openamp/virtio.h>
#include <openamp/rpmsg.h>

#include <stdio.h>
#include <al_core.h>
#include <stdlib.h>

/* 跨核地址与 SGI 约定的唯一真源（所有数字都在那个文件里） */
#include "apu1_openamp_config.h"

#if defined __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * SGI（软件中断）编号
 * ---------------------------------------------------------------------------
 * 具体数值由 apu1_openamp_config.h 定义，这里只做别名。
 *   SGI_TO_NOTIFY    : APU1 -> APU0（裸机通知 Linux"我有消息了"）
 *   SGI_NOTIFICATION : APU0 -> APU1（Linux 通知裸机"来取消息"）
 *
 * ⚠️ 这两个值的方向**极易搞反**，务必读 apu1_openamp_config.h 顶部的推导过程，
 *    并按 docs/30_debug_checklist.md 第 4 层实测确认。
 * --------------------------------------------------------------------------- */
#define SGI_TO_NOTIFY      APU1_SGI_TO_APU0
#define SGI_NOTIFICATION   APU1_SGI_FROM_APU0

/* 内存属性（ARMv8 页表属性描述符） */
#define NORM_NONCACHE      0x11DE2  /* Normal Non-cacheable */
#define STRONG_ORDERED     0xC02    /* Strongly ordered */
#define DEVICE_MEMORY      0xC06    /* Device memory */
#define RESERVED           0x0      /* reserved memory */

/* ---------------------------------------------------------------------------
 * 共享内存（vdev0buffer）—— 与 Linux 设备树 vdev0buffer@9000000 对应
 * --------------------------------------------------------------------------- */
#define SHARED_MEM_PA      APU1_SHARED_MEM_PA
#define SHARED_MEM_SIZE    APU1_SHARED_MEM_SIZE
#define SHARED_BUF_OFFSET  0x0UL

/* 资源表物理地址 —— 与 Linux 设备树 rproc@8000000 对应 */
#define RSC_TABLE_PA       APU1_FW_BASE

/* ---------------------------------------------------------------------------
 * vring —— 与 rsc_table.c 的 RING_TX/RING_RX 是同一份定义的别名，
 *          改地址请改 apu1_openamp_config.h，不要在这里改。
 * --------------------------------------------------------------------------- */
#define RING_TX            APU1_VRING0_PA
#define RING_RX            APU1_VRING1_PA
#define VRING_BD_NUM       APU1_VRING_BD_NUM
#define VRING_ALIGN        APU1_VRING_ALIGN

/* GIC 相关 */
#define SCUGIC_DEV_NAME    "scugic_dev"
#define SCUGIC_BUS_NAME    "generic"
#define SCUGIC_PERIPH_BASE 0xDD000000
#define SCUGIC_DIST_BASE   (SCUGIC_PERIPH_BASE + 0x00001000)

/* 方便打印 */
#define al_printf printf
#define xil_printf al_printf

/* Remoteproc 私有数据结构（与 Linux 端驱动语义对应） */
struct remoteproc_priv {
	const char *gic_name;
	const char *gic_bus_name;
	struct metal_device *gic_dev;
	struct metal_io_region *gic_io;
	unsigned int irq_to_notify;    /* 通知对端用的 SGI */
	unsigned int irq_notification; /* 接收对端通知的 SGI */
	unsigned int cpu_id;           /* 对端（APU0）的 CPU ID */
	atomic_int nokick;
};

int platform_init(int argc, char *argv[], void **platform);

struct rpmsg_device *
platform_create_rpmsg_vdev(void *platform, unsigned int vdev_index,
			   unsigned int role,
			   void (*rst_cb)(struct virtio_device *vdev),
			   rpmsg_ns_bind_cb ns_bind_cb);

int platform_poll(void *platform);

void platform_release_rpmsg_vdev(struct rpmsg_device *rpdev, void *platform);

void platform_cleanup(void *platform);

#if defined __cplusplus
}
#endif

#endif /* PLATFORM_INFO_H_ */
