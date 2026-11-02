#########################################################################
# Copyright (c) 2023, Anlogic Inc. and Contributors. All rights reserved.

# SPDX-License-Identifier: BSD-3-Clause
#########################################################################
DOWNLOAD = ddr


#########################################################################
# 1. define SDK_ROOT, BOARD info, CHIP, COMPILE_PREFIX
#########################################################################
SDK_ROOT ?= 

CHIP ?= dr1m90

#{{APP_OBJ_DIR}}

APP_CUR_DIR := $(abspath .)
APP_TAR_DIR ?= $(abspath .)/build
#{{APP_TAR_DIR}}
APP_OBJ_DIR ?= $(APP_TAR_DIR)/obj


COMPILE_PREFIX ?=



#########################################################################
# 2. define SRC_DIR, INC_DIR, LD_LIBS, LIB_DIR
#    LD_LIBS: External library files that the app depends on
#    LIB_DIR: The path where the external library file resides
#    C_SRCS:   c sources for application
#    CXX_SRCS: cpp sources for application
#    ASM_SRCS: asm sources for application
#########################################################################

APP_SRC_DIR += $(abspath $(sort $(dir $(foreach dir, $(shell find "./" -type d), $(wildcard $(dir)/*.c)))))

APP_INC_DIR += $(abspath $(sort $(dir $(foreach dir, $(shell find "./" -type d), $(wildcard $(dir)/*.h)))))



LD_LIBS  =
LIB_DIR  =

CXX_SRCS :=
ASM_SRCS :=


#########################################################################
# 3. CFLAGS
#########################################################################
CFLAGS := -O2 -g


#########################################################################
# 4. PINMUX_MODE: sdk internal define, pinmux setting need to remove;
#    Fixme: where to add board init source files and headers files
#########################################################################
PLAT_DIR ?= $(SDK_ROOT)/platform


#########################################################################
# 5. define build target
#########################################################################
# ★ 2026-10-15：改成 my_work —— **全队统一只用这一个固件名**。
#
#   原来默认是 apu1_proto_v1，结果同一个固件在工程里出现过三个名字：
#       apu1_proto_v1.elf            构建默认名
#       apu1_proto_v1_liveclock.elf  板上启动脚本要的名字
#       my_work.elf                  手工拷出来用的名字
#   三份内容其实一样，但**极易烧错**（真发生过：改了代码却跑了旧固件，
#   白排查好几轮）。现在统一成 my_work.elf，构建默认就产出它。
#
#   ⚠️ 部署到板子时仍然要改名，因为板上启动脚本写死了：
#        ~/eg400_start_all.sh 里 FW=apu1_proto_v1_liveclock.elf
#      所以拷到 SD 卡时：
#        cp my_work.elf /lib/firmware/apu1_proto_v1_liveclock.elf
#########################################################################
TARGET ?= my_work


#########################################################################
# 6. define ARMv8_CORE for apu
#
#  【这个变量决定什么】
#    它是本核"在双核里扮演什么角色"的开关，会变成平台库的
#    -DARM_CORE_SLAVE（soc_base/dr1m90_fpsoc_cpu1/Makefile:91-93）：
#
#      MASTER  -> 不定义 ARM_CORE_SLAVE
#      SLAVE   -> 定义   ARM_CORE_SLAVE
#
#    受影响的位置（本 SDK 内全部搜索所得，只有这几处）：
#      1. chip/dr1x90/dr1m90/src/al_gic.c:75
#           #ifndef ARM_CORE_SLAVE  -> AlGicv3_DistInit()        自己配 Distributor
#           #else                   -> AlGicv3_Rdist_WaitMasterAwake()
#                                       ★ 一个**没有超时**的死循环，死等 apu0
#                                         初始化 GIC Distributor
#      2. platform/src/soc_plat.c:2013   AMP 初始化分支选择
#      3. lib/log/src/al_printf.c:40/357/372   异步日志环形缓冲的 cache 维护
#      4. chip/dr1x90/common/src/al_chip.c:57   IPC PS 驱动
#
#  【当前工程的实际影响（2026-10-13 复核）】
#    · al_gic.c 那条死循环**走不到**：只有 SUPPORT_NONSECURE 被定义时
#      al_aarch64_start.S:203 才会 `bl AlGic_Init`，而 SUPPORT_NONSECURE
#      要求 ARMv8_EL=EL1 且 ARMv8_SECURE=NONSECURE；本工程用的是默认的
#      EL3 + SECURE ⇒ 该函数不执行。（这也是 apu_gic.h 里那段说明的依据。）
#    · soc_plat.c:2013 那条 `#if (defined HAVE_OPENAMP) || (defined ARM_CORE_SLAVE)`
#      在本工程里**两个条件都成立**（SLAVE 时 ARM_CORE_SLAVE 也定义了），
#      所以走的一定是 __Soc_PlatInit_AMP()；MASTER 时靠 HAVE_OPENAMP 走同一条。
#      ⇒ 这两点确实**不受本变量影响**，但下面两点受：
#        · al_chip.c:57  `#if (defined HAVE_IPCPS_DRIVER) && (!defined ARM_CORE_SLAVE)`
#          —— 本工程没有 HAVE_IPCPS_DRIVER，所以这里也无差别（如实记录，不夸大）。
#        · al_printf.c:40/357/372  异步日志环形缓冲的 cache 维护**只在 SLAVE 分支**里做
#          （AL_PRINT_ASYNC 未定义时这些分支不编入，同样无差别）。
#
#  ★★★ 本行 = SLAVE：与 Linux remoteproc 联调 / SD 卡启动 Linux 时的正确值 ★★★
#      apu0(Linux) 是主核，APU1 是它的从核 —— 这与 main.c 里
#      platform_create_rpmsg_vdev(..., VIRTIO_DEV_DEVICE, ...) 的从设备角色一致。
#      MASTER 只是 2026-10-10 独立 JTAG 调试期的临时值（当时没有 apu0 在跑），
#      现按《README_状态总览与交接索引.md》§6 硬约束第 5 条改回 SLAVE。
#
#  【改动后必须全量重编 —— 本次已按要求做】
#      这个变量控制的是**平台库(libalbsp.a / libplat.a)**的编译宏，
#      而 make 只看文件时间戳 —— 只改这一行、时间戳没变的源文件不会重编，
#      改动静默不生效（现象：text 段一个字节都没变）。
#      ⇒ 本次已删除 build\obj 和 build\lib 后全量重编，
#        并用 my_work.map 里的符号（AlGicv3_Rdist_WaitMasterAwake 出现 /
#        AlGicv3_DistInit 消失）实证 SLAVE 生效。
#########################################################################
ARMv8_CORE = SLAVE
#  ↑ 2026-10-13 改回 SLAVE：供 Linux(SD 卡启动) 通过 remoteproc 加载并 rpmsg 通信

#########################################################################
# 7. linker scripts
#########################################################################
ARMv8_STATE ?= 64
ifeq ($(CHIP), dr1v90)
ARCH_ABI := lp64d
else ifeq ($(ARMv8_STATE),64)
ARCH_ABI := aarch64
else ifeq ($(ARMv8_STATE),32)
ARCH_ABI := aarch32
endif

LINKER_SCRIPT := ./lds/gcc_$(CHIP)_$(DOWNLOAD)_$(ARCH_ABI).ld

#########################################################################
# 7.5 GICv3 头文件目录（裸机分支必须自己加）
#
#  【为什么必须加】
#    SDK 的 tools/make/rules.mk 只在 RTOS=freertos 分支里把这三个目录加进
#    PUBLIC_INC_DIR（见 rules.mk:178 / :187），**裸机（RTOS 为空）分支一条都不加**。
#    而本工程 src/machine/dr1m90/ 下有两个文件直接用它们：
#        apu_gic.c:15        #include "al_gicv3.h"
#        dr1m90_rproc.c:19   #include "al_gicv3.h"
#    不加的后果（2026-10-01 实测）：
#        fatal error: al_gicv3.h: No such file or directory
#
#  【三条要一起加 —— 它们是一条 #include 链】
#    al_gicv3.h -> #include "al_aarch64_core.h" -> #include "al_arch_sysops.h"
#    少一条就会顺着报下一个 No such file。
#
#  ★ 2026-10-01：改用 $(SDK_ROOT) 拼**相对**路径。
#    原来的写法把 C:/qiansai_FPGA/my_work/... 硬编码进去了 ——
#    工程一旦改名/换盘就编不过（这正是本次构建失败的根因）。
#########################################################################
INC_DIR += $(SDK_ROOT)/dr1m90_fpsoc_cpu1/arch/arm/common/gic_v3/inc \
           $(SDK_ROOT)/dr1m90_fpsoc_cpu1/arch/arm/armv8/aarch64/cortex-a/inc \
           $(SDK_ROOT)/dr1m90_fpsoc_cpu1/arch/common/inc

# include rules.mk
include $(SDK_ROOT)/tools/make/rules.mk

# vim: syntax=make
