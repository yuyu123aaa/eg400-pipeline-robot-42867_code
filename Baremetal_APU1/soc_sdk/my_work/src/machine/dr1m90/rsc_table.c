/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 资源表（resource table） —— APU1 裸机侧
 * ---------------------------------------------------------------------------
 * 这是 APU1 给 Linux 端 remoteproc 看的“自述文件”：
 *   1. 声明要用哪块共享内存收发消息；
 *   2. 声明 vring0 / vring1 的物理地址、对齐、描述符数量；
 *   3. Linux 的 dr1m90_remoteproc 驱动从固件 ELF 里读出它，据此初始化 virtio/rpmsg。
 *
 * 改这里的任何地址，必须同步改 Linux 设备树（见 docs/20_linux_handoff.md）。
 */

#include <openamp/open_amp.h>
#include "rsc_table.h"
#include "platform_info.h"

#define __section_t(S)          __attribute__((__section__(#S)))
#define __resource              __section_t(.resource_table)

/* 使能 RPMSG name service：Linux 靠它把同名端点对接起来 */
#define RPMSG_VDEV_DFEATURES        (1 << VIRTIO_RPMSG_F_NS)

#define VIRTIO_ID_RPMSG_             7

#define NUM_VRINGS                  0x02

/* ---------------------------------------------------------------------------
 * 下面这几个值统一定义在 apu1_openamp_config.h（经 platform_info.h 引入），
 * 本文件不再各写一份，避免"改了一处忘了另一处"。
 *
 *   RING_TX      <-> Linux DTS vdev0vring0@f030000  (APU1 -> APU0)
 *   RING_RX      <-> Linux DTS vdev0vring1@f034000  (APU0 -> APU1)
 *   VRING_ALIGN / VRING_BD_NUM 也必须两端一致
 * --------------------------------------------------------------------------- */

#define NUM_TABLE_ENTRIES           1

struct remote_resource_table __resource resources = {
	/* Version */
	1,
	/* 条目数量 */
	NUM_TABLE_ENTRIES,
	/* reserved */
	{0, 0,},
	/* 条目偏移 */
	{
	 offsetof(struct remote_resource_table, rpmsg_vdev),
	},
	/* VirtIO 设备条目 */
	{
	 RSC_VDEV, VIRTIO_ID_RPMSG_, 31, RPMSG_VDEV_DFEATURES, 0, 0, 0,
	 NUM_VRINGS, {0, 0},
	},
	/* 两条 vring 条目 */
	{RING_TX, VRING_ALIGN, VRING_BD_NUM, 1, 0},
	{RING_RX, VRING_ALIGN, VRING_BD_NUM, 2, 0},
};

void *get_resource_table(int rsc_id, int *len)
{
	(void)rsc_id;
	*len = sizeof(resources);
	return &resources;
}
