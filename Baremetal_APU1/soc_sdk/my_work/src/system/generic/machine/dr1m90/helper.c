/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * helper.c —— libmetal / GIC 系统初始化（APU1 裸机侧）
 *
 * 关键：metal_anlogic_irq_isr 是安路 SDK 提供的"把 GIC 中断转发给 libmetal"
 *       的桥接函数。它的原型必须在 SDK 里能查到；若你的 SDK 版本函数名不同，
 *       编译报错时按 SDK 头文件改成对应的名字即可。
 */

#include <metal/sys.h>
#include <metal/irq.h>
#include "platform_info.h"
#include "al_intr.h"
#include "al_errno.h"
#include "al_chip.h"
#include "al_reg_io.h"

extern void metal_anlogic_irq_isr(void *arg);

static int app_gic_initialize(void)
{
    al_printf("Compile time: %s %s\r\n", __DATE__, __TIME__);

    /* 把 SGI_NOTIFICATION 的中断入口接到 libmetal 的 ISR 分发上 */
    (AL_VOID)AlIntr_RegHandler(SGI_NOTIFICATION, AL_NULL,
                               metal_anlogic_irq_isr,
                               (void *)(SGI_NOTIFICATION));

    (AL_VOID)AlIntr_SetLocalInterrupt(AL_FUNC_ENABLE);

    return 0;
}

int init_system(void)
{
    int ret;
    struct metal_init_params metal_param = METAL_INIT_DEFAULTS;

    metal_init(&metal_param);

    app_gic_initialize();

    ret = metal_anlogic_irq_init();
    if (ret) {
        al_printf("%s: Anlogic metal IRQ controller init failed.\r\n", __func__);
    }

    return ret;
}

void cleanup_system(void)
{
    metal_finish();
}
