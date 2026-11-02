/*
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef MAIN_H
#define MAIN_H

#include "apu1_openamp_config.h"

/* 端点名字：必须和 Linux 端 /sys/bus/rpmsg 里看到的一致 */
#define RPMSG_SERVICE_NAME         APU1_RPMSG_SERVICE_NAME

/* ---------------------------------------------------------------------------
 * 应用入口
 * ---------------------------------------------------------------------------
 *  SDK 的启动代码（components_init）会调用它；
 *  main() 里也调一次（见 main.c 末尾）。
 *
 *  ⚠️ 这里声明一下是有意义的：不声明时它会触发
 *     -Wmissing-prototypes（"no previous prototype"），
 *     而且万一将来参数签名变了，声明处会报错提醒，不会静默不一致。
 * --------------------------------------------------------------------------- */
void openamp_sample(void *Parameters);

#endif /* MAIN_H */
