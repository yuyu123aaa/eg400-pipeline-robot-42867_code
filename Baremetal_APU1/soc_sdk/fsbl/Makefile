#########################################################################
# Copyright (c) 2023, Anlogic Inc. and Contributors. All rights reserved.

# SPDX-License-Identifier: BSD-3-Clause
#########################################################################
ALFSBL_PJTAG_ENABLE_VAL = 0
ALFSBL_PMU_EXCLUDE_VAL = 0
ALFSBL_WDT_EXCLUDE_VAL = 0
ARMv8_EL = EL3
DDR_2M_MAPPING = 0
DOWNLOAD = ocm


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
TARGET ?= FSBL


#########################################################################
# 6. define ARMv8_CORE for apu
#
#  ★★★ FSBL 必须是 MASTER ★★★（2026-10-13 修正，原为 SLAVE）
#
#  这个变量会变成平台库的 -DARM_CORE_SLAVE（见
#  soc_base/dr1m90_fpsoc_cpu1/Makefile:91-93）：
#      MASTER -> 不定义 ARM_CORE_SLAVE
#      SLAVE  -> 定义   ARM_CORE_SLAVE
#
#  它决定 chip/dr1x90/dr1m90/src/al_gic.c:75-80 走哪条分支：
#      #ifndef ARM_CORE_SLAVE
#          AlGicv3_DistInit();                    ← 自己初始化 GIC Distributor
#      #else
#          AlGicv3_Rdist_WaitMasterAwake(...);    ← 死等"主核"把 GIC 配好
#          AlGicv3_DisableOwnSpiInterrupt();
#      #endif
#
#  为什么写成 SLAVE 会死机（2026-09-22 踩过、2026-10-13 又踩了一次）：
#    FSBL 跑在 APU0 上，**它自己就是主核** —— 它就是那个该去配 GIC 的人。
#    写成 SLAVE 后它却去调 AlGicv3_Rdist_WaitMasterAwake()，而那个函数是
#    一个**没有超时、没有计数器**的原地自旋：
#        ldr  w1, [x0]          ; 读 GICR_WAKER.ChildrenAsleep
#        tbnz w1, #2, .-4       ; 没醒就永远转下去
#    没有人会来叫醒它（主核就是它自己），于是板子卡死在 GIC 初始化里，
#    连一句串口打印都出不来 —— 现象就是"**串口一个字不打**"。
#
#  实测可证的判据（反汇编 fsbl.elf 看 AlGic_Init 调了谁）：
#      MASTER -> bl AlGicv3_DistInit + bl AlGicv3_RdistInit，无 WaitMasterAwake
#      SLAVE  -> bl AlGicv3_Rdist_WaitMasterAwake，且 AlGicv3_DistInit 符号缺失
#    交接包里那份已验证能启 Linux 的 FSBL 就是 MASTER：
#        61007a58: bl AlGicv3_DriverInit
#        61007a5c: bl AlGicv3_DistInit
#        61007a78: bl AlGicv3_RdistInit
#
#  ⚠️ 注意区别，别改错地方：
#    · 本文件（fsbl）= APU0 引导程序 → 必须 MASTER
#    · my_work/Makefile  = APU1 裸机固件 → 必须 SLAVE
#      （APU1 是从核，由 Linux 经 SMC 拉起，就该"等主核唤醒本核"）
#    这是两套独立构建，同名变量、相反取值，互不影响。
#
#  ⚠️ 改完必须【全量重编】：这个宏影响的是平台库(libalbsp.a/libplat.a)，
#     而 make 只看文件时间戳 —— 只改这一行、源文件没动的话不会重编，
#     改动会**静默不生效**（现象：text 段一个字节都没变）。
#     ⇒ 先删 build\obj 和 build\lib 再编。
#########################################################################
ARMv8_CORE = MASTER

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

# include rules.mk
include $(SDK_ROOT)/tools/make/rules.mk

# vim: syntax=make
