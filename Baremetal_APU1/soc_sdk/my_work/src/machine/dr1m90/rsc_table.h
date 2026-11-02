/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * rsc_table.h —— 资源表结构体定义（APU1 裸机侧）
 * ===========================================================================
 *  ⚠️ 这个头文件必须**定义 `struct remote_resource_table` 本身**，
 *     不能只声明 get_resource_table()。
 *     否则 rsc_table.c 里的 `struct remote_resource_table resources = {...}`
 *     会报 "variable 'resources' has initializer but incomplete type"。
 *
 *  结构体定义与安路 SDK 自带官方实现逐字一致：
 *      <SDK>/3rdparty/lib/open-amp/apps/machine/dr1m90/rsc_table.h
 *      <SDK>/solutions/demo/baremetal/openamp_rpmsg_echo/src/machine/dr1m90/rsc_table.h
 * ===========================================================================
 */

#ifndef RSC_TABLE_H_
#define RSC_TABLE_H_

#include <stddef.h>
#include <openamp/open_amp.h>

#if defined __cplusplus
extern "C" {
#endif

#define NO_RESOURCE_ENTRIES         1

/* Resource table for the given remote
 *
 * ★ `aligned(0x100000)` = 1MB 对齐，这是硬要求：
 *   Linux 的 dr1m90_remoteproc 在固件 carveout 基址（0x08000000）处解析资源表，
 *   所以这个结构体必须正好落在 1MB 边界上。
 *   我们的链接脚本 lds/gcc_dr1m90_ddr_aarch64.ld 里也把 .resource_table 段
 *   做了 ALIGN(0x100000)（段首本来就是 0x08000000），两处配合保证对齐。
 */
struct remote_resource_table {
	unsigned int version;
	unsigned int num;
	unsigned int reserved[2];
	unsigned int offset[NO_RESOURCE_ENTRIES];
	/* rpmsg vdev entry */
	struct fw_rsc_vdev rpmsg_vdev;
	struct fw_rsc_vdev_vring rpmsg_vring0;
	struct fw_rsc_vdev_vring rpmsg_vring1;
} __attribute__((packed, aligned(0x100000)));

void *get_resource_table(int rsc_id, int *len);

#if defined __cplusplus
}
#endif

#endif /* RSC_TABLE_H_ */
