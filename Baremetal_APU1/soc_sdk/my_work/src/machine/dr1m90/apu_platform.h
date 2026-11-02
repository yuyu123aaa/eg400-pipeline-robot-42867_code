/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_platform.h —— APU1 平台级一次性初始化（内存属性、开机平台自检）
 * ===========================================================================
 */

#ifndef APU_PLATFORM_H
#define APU_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * apu_platform_init() —— 平台级初始化。**必须在任何共享内存访问之前**调用。
 *
 * 现在做两件事：
 *   ① 把 RPMsg 的 vdev0buffer / vring0 / vring1 三块从 Cacheable 改成
 *      Normal-Non-Cacheable（Linux 侧那三块是 non-cacheable / Device 映射，
 *      两端属性不一致会读到旧值或半帧）；
 *   ② 打印一行地址自检，确认镜像本身没越过 0x09000000（踩到共享缓冲）。
 *
 * 建议位置：main.c 的 openamp_sample() 第一行（早于 apu_time_init()/print_config()）。
 * 幂等，可重复调用。
 */
int apu_platform_init(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_PLATFORM_H */
