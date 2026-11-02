/*
 * Copyright (c) 2023, Anlogic Inc. and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

OUTPUT_FORMAT("elf64-littleaarch64", "elf64-littleaarch64", "elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

MEMORY
{
    ram    : ORIGIN = 0x8000000, LENGTH = 0x1000000
}

SECTIONS
{
    __stack_size = DEFINED(__stack_size) ? __stack_size : 6M;
    __async_print_ring_buf_size = DEFINED(async_print_ring_buf_size) ? async_print_ring_buf_size : 1M;

    /* ======================================================================
     * ★ 资源表：必须放在镜像**最前面**，并且 1MB 对齐
     *
     *   为什么：
     *     · rsc_table.h 里 `struct remote_resource_table` 声明为
     *       __attribute__((packed, aligned(0x100000)))，即 1MB 对齐；
     *     · Linux 的 dr1m90_remoteproc 用 find_loaded_rsc_table 在
     *       **固件 carveout 基址 0x08000000** 处解析资源表。
     *   两条件合起来 => 资源表必须正好落在 0x08000000。
     *
     *   注意：这里跟官方脚本（把 .resource_table 放在 .bss 之后）**不同**。
     *   官方那样放，段地址会是某个随机值；不过 Linux 驱动是按 ELF 段找表的，
     *   所以官方也能跑。我们放段首更可预测，也便于上板核对。
     * ====================================================================== */
    .resource_table ORIGIN(ram) :
    {
        . = ALIGN(0x100000);
        PROVIDE(_resource_table_start = .);
        KEEP(*(.resource_table))
        . = ALIGN(0x100000);
        PROVIDE(_resource_table_end = .);
    } > ram AT>ram

    /* ★ 资源表本身必须装得进那 1MB 空洞（2026-09-25 加）。
     *   上面两处 ALIGN(0x100000) 让 .resource_table 段固定占 1MB：
     *   表内容从 0x08000000 起，段尾被硬推到 0x08100000。
     *   一旦表内容超过 1MB，ALIGN 会把它推到 0x08200000，
     *   于是**入口地址跟着漂**（实测正常为 0x08100000），
     *   而 Linux 侧是按 carveout 基址 0x08000000 去解析资源表的 —— 直接失联。 */
    ASSERT(SIZEOF(.resource_table) <= 0x100000,
           "resource table exceeds 1MB - it would push the entry point off 0x08100000")

    /* all text segment = startup64.o data segment + others text segment */
    .text :
    {
        PROVIDE(_text_start  = .);
        *al_aarch64_start.S.o(.text)
        *(.text)
        PROVIDE(_text_end  = .);
    }  > ram AT>ram

    .ilalign         :
    {
        . = ALIGN(4);
        PROVIDE( _ilm_lma = . );
    } > ram AT>ram

    .ialign         :
    {
        PROVIDE( _ilm = . );
    } > ram AT>ram

    /* read only data segment */
    .rodata :
    {
        . = ALIGN(16);
        PROVIDE(_rodata_start = .);
        *(.rodata)
        PROVIDE(_rodata_end   = .);
        /* section information for initial. */
        . = ALIGN(4);
        __rt_init_start = .;
        KEEP(*(SORT(.rti_fn*)))
        __rt_init_end = .;
        /* section information for finsh shell */
        . = ALIGN(4);
        __fsymtab_start = .;
        KEEP(*(FSymTab))
        __fsymtab_end = .;
        . = ALIGN(4);
        __vsymtab_start = .;
        KEEP(*(VSymTab))
        __vsymtab_end = .;
        . = ALIGN(4);
        __rt_utest_tc_tab_start = .;
        KEEP(*(UtestTcTab))
        __rt_utest_tc_tab_end = .;
        *(.gnu.linkonce.r.*)
        /* section information for usbh class */
        . = ALIGN(4);
        __usbh_class_info_start__ = .;
        KEEP(*(.usbh_class_info))
        __usbh_class_info_end__ = .;
    }  > ram AT>ram

    .lalign         :
    {
        . = ALIGN(4);
        PROVIDE( _data_lma = LOADADDR(.data) );
    } >ram AT>ram

    .dalign         :
    {
        . = ALIGN(4);
        PROVIDE( _data = ADDR(.data));
    } >ram AT>ram

    .pagetable :
    {
        . = ALIGN(4096);
        PROVIDE(_pagetable_start = .);
        *(.page_table)
        PROVIDE(_pagetable_end   = .);
    }  > ram AT>ram

    .fini           :
    {
        KEEP (*(SORT_NONE(.fini)))
    } >ram AT>ram

    . = ALIGN(4);

    .preinit_array  :
    {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } >ram AT>ram

    .init_array     :
    {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
        PROVIDE_HIDDEN (__init_array_end = .);
    } >ram AT>ram

    .fini_array     :
    {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } >ram AT>ram

    .ctors          :
    {
        /* gcc uses crtbegin.o to find the start of
        the constructors, so we make sure it is
        first.  Because this is a wildcard, it
        doesn't matter if the user does not
        actually link against crtbegin.o; the
        linker won't look for a file to match a
        wildcard.  The wildcard also means that it
        doesn't matter which directory crtbegin.o
        is in.  */
        KEEP (*crtbegin.o(.ctors))
        KEEP (*crtbegin?.o(.ctors))
        /* We don't want to include the .ctor section from
        the crtend.o file until after the sorted ctors.
        The .ctor section from the crtend file contains the
        end of ctors marker and it must be last */
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
        KEEP (*(SORT(.ctors.*)))
        KEEP (*(.ctors))

        PROVIDE(__ctors_start__ = .);
        /* new GCC version uses .init_array */
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE(__ctors_end__ = .);
    } >ram AT>ram

    .dtors          :
    {
        KEEP (*crtbegin.o(.dtors))
        KEEP (*crtbegin?.o(.dtors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
        KEEP (*(SORT(.dtors.*)))
        KEEP (*(.dtors))
    } >ram AT>ram

    .eh_frame : {
       *(.eh_frame)
    } >ram AT>ram

    .eh_framehdr : {
       __eh_framehdr_start = .;
       *(.eh_framehdr)
       __eh_framehdr_end = .;
    } >ram AT>ram

    .gcc_except_table : {
       *(.gcc_except_table)
    } >ram AT>ram

      . = ALIGN(4096);
    .cli_section :
    {
        . = ALIGN(4096);
        _cli_section_start = .;
        KEEP(*(cli_section))
        _cli_section_end   = .;
    }  > ram AT>ram

    . = ALIGN(4096);
    .unity_section :
    {
        . = ALIGN(4096);
        _unity_test_start = .;
        KEEP(*(unity_section))
        _unity_test_end   = .;
    }  > ram AT>ram

    .gbe_phy_section :
    {
        _gbe_phy_section_start = .;
        KEEP(*(gbe_phy_section))
        _gbe_phy_section_end   = .;
    }  > ram AT>ram

    PROVIDE (__etext = .);
    PROVIDE (_etext = .);
    PROVIDE (etext = .);
    PROVIDE( _eilm = . );

    /* all data segment */
    . = ALIGN(16);
    .data :
    {
        . = ALIGN(16);
        PROVIDE(_data_start = .);
        *(.data*)
        PROVIDE(_data_end = .);
    }  > ram AT>ram

    . = ALIGN(2M);
    .noncacheable (NOLOAD):
    {
        . = ALIGN(2M);
        _no_cache_section_start = .;
        KEEP(*(.noncacheable))
        . = ALIGN(2M);
        _no_cache_section_end   = .;
    }  > ram AT>ram

    . = ALIGN(64);
    .async_print_ring_buf (NOLOAD):
    {
        . = ALIGN(64);
        PROVIDE(_async_print_ring_buf_start = .);
        . = __async_print_ring_buf_size;
        PROVIDE(_async_print_ring_buf_end = .);
        . = ALIGN(64);
    }  > ram AT>ram

    PROVIDE( _edata = . );
    PROVIDE( edata = . );

    . = ALIGN(16);
    /* all bss segment */
    .bss (NOLOAD):
    {
        . = ALIGN(16);
        PROVIDE(__bss_start__ = .);
        PROVIDE(_bss_start = .);
        *(*bss .bss*)
        *(COMMON)
        PROVIDE(_bss_end = .);
        . = ALIGN(16);
        PROVIDE(__bss_end__ = .);
    }  > ram AT>ram

    /* 注意：.resource_table 段已经在文件**开头**定义过了（放在镜像最前），
     *       这里不要再定义一次，否则会重复。 */

    /* end */
    . = ALIGN(16);
    _end = .;
    _heap_start = .;

    .stack (ORIGIN(ram) + LENGTH(ram) - __stack_size):
    {
        . = ALIGN(16);
        PROVIDE(_heap_end = .);
        PROVIDE(stack_start = .);
        . = __stack_size;
        PROVIDE(stack_top = .);
        PROVIDE(_sp = .);
        . = ALIGN(16);

    } > ram AT>ram
}

/* ======================================================================
 * ★ 容量自检（2026-09-25 加）
 *
 * 为什么必须有：APU1 需要"住进" 0x08000000..0x09000000 这段，而**同一段里
 * 还必须装得下** RPMsg 的 vdev0buffer ——
 *   apu1_openamp_config.h 的 APU1_SHARED_MEM_PA = 0x09000000（32MB，正好从
 *   0x09000000 起）。也就是说：
 *     固件（含 6MB 栈）一旦长过 0x09000000，就直接踩掉 RPMsg 共享缓冲。
 *   现象是"Linux 侧能起来、但收发随机出错/偶发死机"，极难定位。
 *   之前脚本里没有任何检查，全靠人肉看 .map。
 *
 * 同时防"ram 的 LENGTH 被改大"这种手滑：整个 ram 区必须在 0x09000000 以下结束。
 * ====================================================================== */
ASSERT(ORIGIN(ram) + LENGTH(ram) <= 0x09000000,
       "APU1 firmware region runs past 0x09000000 - it would overlap the RPMsg vdev0buffer")
ASSERT(_sp <= 0x09000000,
       "APU1 stack top (_sp) is above 0x09000000 - stack would corrupt the RPMsg vdev0buffer")
ASSERT(_end <= stack_start,
       "APU1 .bss/heap overlaps the stack - increase __stack_size or shrink the image")
