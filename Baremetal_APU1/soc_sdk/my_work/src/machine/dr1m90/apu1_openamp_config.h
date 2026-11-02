/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu1_openamp_config.h —— APU0(Linux) 与 APU1(裸机) 之间所有"硬约定"的唯一真源
 * ===========================================================================
 *  这个文件是**唯一**需要你改数字的地方。
 *  改完这里，platform_info.h / rsc_table.c / 链接脚本要跟着一起核对。
 * ===========================================================================
 */

#ifndef APU1_OPENAMP_CONFIG_H_
#define APU1_OPENAMP_CONFIG_H_

/* ===========================================================================
 * 一、内存布局
 * ---------------------------------------------------------------------------
 *   0x08000000 +------------------------+  <-- APU1 固件 + 资源表（16MB）
 *              | .resource_table (最前) |
 *              | .text .rodata .data    |
 *              | .stack (末尾 256KB)    |
 *   0x09000000 +------------------------+  <-- vdev0buffer 共享内存池（32MB）
 *   0x0F030000 +------------------------+  <-- vdev0vring0（16KB）
 *   0x0F034000 +------------------------+  <-- vdev0vring1（16KB）
 *
 * 这四块**必须**在 Linux 设备树里声明为 reserved-memory（带 no-map），
 * 否则 Linux 会把它们当普通内存分配出去，通信必然失败。
 * 设备树片段见 docs/20_linux_handoff.md 第 3 节。
 * =========================================================================== */

/* APU1 固件基址：也是资源表地址（链接脚本里 .resource_table 放在镜像最前） */
#define APU1_FW_BASE            0x08000000UL
/* APU1 固件区大小：必须 >= 链接脚本 MEMORY ram 的 LENGTH */
#define APU1_FW_SIZE            0x01000000UL   /* 16MB */

/* vdev0buffer：RPMsg 报文实际存放位置（32MB） */
#define APU1_SHARED_MEM_PA      0x09000000UL
#define APU1_SHARED_MEM_SIZE    0x02000000UL

/* 两条 vring 的物理地址，必须与 rsc_table.c 里 RING_TX / RING_RX 一致 */
#define APU1_VRING0_PA          0x0F030000UL   /* TX: APU1 -> APU0 */
#define APU1_VRING1_PA          0x0F034000UL   /* RX: APU0 -> APU1 */

/* vring 描述符数量与对齐 */
#define APU1_VRING_BD_NUM       128
#define APU1_VRING_ALIGN        0x1000

/* ===========================================================================
 * 二、SGI（软件中断）方向 —— 结论已定案，但这套代码的宏名是个陷阱
 * ===========================================================================
 *
 * 【重要声明】本节结论来自**两份实际源码的交叉验证**，不是猜的：
 *   证据 A：内核 drivers/remoteproc/dr1m90_remoteproc.c（6.1.111）
 *   证据 B：创龙裸机 dr1m90_rproc.c + platform_info.h
 *
 * ---------------------------------------------------------------------------
 * 证据 A：Linux 侧
 * ---------------------------------------------------------------------------
 *     #define REMOTE_SGI   0
 *     #define HOST_SGI     1
 *     static int vring_sgis[MAX_NUM_VRINGS] = {14, 15};   // 默认值
 *
 *     for (i = 0; i < n_vring_sgis; i++) {
 *         sgi_fwspec.param[0] = vring_sgis[i];
 *         local->irq[i] = irq_create_fwspec_mapping(&sgi_fwspec);
 *     }
 *     // 于是 irq[0] = SGI 14, irq[1] = SGI 15
 *
 *     // Linux 把 irq[1] 注册成"收"中断：
 *     virq = local->irq[HOST_SGI];      // irq[1] = SGI 15
 *     request_percpu_irq(virq, ..., "vring0", ...);
 *
 *     // Linux 用 irq[0] 去"发"：
 *     ipi_send_single(rpirq->irq[REMOTE_SGI], slave);   // irq[0] = SGI 14
 *
 *   => Linux：**发 14，收 15**
 *
 * ---------------------------------------------------------------------------
 * 证据 B：创龙裸机侧（dr1m90_rproc.c 的真实调用点）
 * ---------------------------------------------------------------------------
 *     // 收：注册的是 irq_notification
 *     irq_vect = prproc->irq_notification;          // = SGI_NOTIFICATION = 14
 *     metal_irq_register(irq_vect, dr1m90_proc_irq_handler, rproc);
 *     metal_irq_enable(irq_vect);
 *
 *     // 发：用的是 SGI_TO_NOTIFY
 *     AlGicv3_RaiseSgi(SGI_TO_NOTIFY, AL_GICV3_G1NS, prproc->cpu_id);   // = 15
 *
 *   => 裸机：**收 14，发 15**
 *
 * ---------------------------------------------------------------------------
 * 两边完全互补，所以创龙官方例程是能通的：
 *
 *     Linux  --SGI 14-->  APU1        在 SGI 14 上收   ← 必须监听 14
 *     APU1   --SGI 15-->  Linux       在 SGI 15 上收   ← 必须发送 15
 *
 * ⚠️⚠️ **命名陷阱（不是配置错误）** ⚠️⚠️
 *   创龙那套宏名**字面语义和实际用途是反的**：
 *     - `SGI_TO_NOTIFY  = 15`  ← 名字像"我要发通知"，但它被赋给了
 *                                 `irq_notification` 之外……真正被注册成
 *                                 **接收**中断的是 `SGI_NOTIFICATION = 14`
 *     - 也就是说：读代码时**不要看宏名猜方向**，只看它被赋给哪个字段：
 *          赋给 `irq_notification` 的 = **接收**
 *          传给 `AlGicv3_RaiseSgi()` 的 = **发送**
 *   本工程为了让你不再被名字骗，改用**语义明确**的宏：
 *          APU1_SGI_FROM_APU0  = 监听（等 Linux 通知）
 *          APU1_SGI_TO_APU0    = 发送（通知 Linux）
 *
 * ---------------------------------------------------------------------------
 * 另外两个能省你时间的事实
 * ---------------------------------------------------------------------------
 *   1. 驱动**不读** DTS 里的 `vring0` / `vring1` 属性
 *      （源码里只有 module_param_array(vring_sgis, ...)，没有任何 of_property_read
 *        去解析 vring0/vring1）。所以创龙 DTS 里 `vring0 = <15>; vring1 = <14>;`
 *      那两行是**无效装饰**，写不写都不改变行为——`vring_sgis` 的默认值就是 {14,15}。
 *
 *      【已用官方源码交叉验证】我把创龙的 dtb 反解出来的字符串表里确实同时存在
 *      `vring0` / `vring1` / `memory-region` / `firmware` 属性名，而驱动源码里
 *      搜不到任何 `of_property_read_...(node, "vring0"...)`。两相对照可以确定：
 *      DTS 写了但驱动没收，等于没写。
 *      换个角度也能自证：若驱动真读 DTS（vring0=host收=15），官方例程反而通不了；
 *      它之所以能通，正说明用的是驱动默认值 {14,15}。
 *
 *      真正能改的是内核命令行参数：
 *          dr1m90_remoteproc.vring_sgis=14,15
 *      数组顺序是 { REMOTE_SGI, HOST_SGI }：
 *        第 1 个数 = Linux 发给从核用的（= APU1 的"收"）
 *        第 2 个数 = Linux 收从核通知用的（= APU1 的"发"）
 *   2. 无论怎么配，**两边必须成对**：
 *        APU1 监听的号 == Linux 的 irq[HOST_SGI]      （默认 15）
 *        APU1 发送的号 == Linux 的 irq[REMOTE_SGI]    （默认 14）
 *      只改一边，必然一个方向通、另一个方向不通。
 *
 * 【实测确认方法】（联调时第一件事就做这个）
 *   在 dr1m90_rproc.c 的 dr1m90_proc_irq_handler() 里加打印：
 *        al_printf("SGI %d received from APU0\r\n", vect_id);
 *   然后 Linux 侧 `echo start > /sys/class/remoteproc/remoteproc0/state`：
 *     - 看到 "SGI 15 received"  -> 配置正确，继续往下调
 *     - 看到 "SGI 14 received"  -> 把下面 APU1_SGI_SWAP 改成 1 重新编译
 *     - 一条都看不到            -> 问题不在 SGI 号，去看第 4 层 GIC 初始化
 * =========================================================================== */

/* 正常情况下保持 0；实测收到的是 15 而不是 14 时才改成 1 重编一次 */
#define APU1_SGI_SWAP           0

#if APU1_SGI_SWAP
  /* 备选方案：APU1 监听 15、发送 14 */
  #define APU1_SGI_FROM_APU0    15   /* APU1 监听（等 Linux 通知） */
  #define APU1_SGI_TO_APU0      14   /* APU1 发送（通知 Linux） */
#else
  /* 默认方案：与内核驱动 vring_sgis={14,15} 及创龙官方裸机固件的
   * 实际行为一致 —— APU1 监听 14、发送 15。
   * （注意：创龙那套宏名字面相反，详见上文"命名陷阱"） */
  #define APU1_SGI_FROM_APU0    14   /* APU1 监听：等 Linux 通知 */
  #define APU1_SGI_TO_APU0      15   /* APU1 发送：通知 Linux */
#endif

/* ===========================================================================
 * 三、GIC
 * =========================================================================== */
#define APU1_GIC_DIST_BASE      0xDD000000UL   /* 已与内核 dtsi 的 interrupt-controller@dd000000 核对 */
#define APU1_GIC_DIST_SIZE      0x1000UL

/* ===========================================================================
 * 四、RPMsg 端点名（两端必须一致）
 * =========================================================================== */
#define APU1_RPMSG_SERVICE_NAME "rpmsg-openamp-demo-channel"

#endif /* APU1_OPENAMP_CONFIG_H_ */
