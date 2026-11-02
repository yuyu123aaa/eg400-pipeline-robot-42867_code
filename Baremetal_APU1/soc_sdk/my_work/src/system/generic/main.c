/**
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * main.c —— APU1 裸机侧主程序（OpenAMP / RPMsg 从核）
 * ===========================================================================
 *  【只有一条路径：RPMsg】
 *
 *   上电 → 平台初始化（共享区 non-cacheable + 地址自检）
 *        → 本核 GIC 拉起（SLAVE 语义，绝不碰 Distributor）
 *        → platform_init / platform_create_rpmsg_vdev / rpmsg_create_ept
 *        → 主循环：platform_poll → time_poll → periodic → app_periodic
 *                                      → testlink_poll（串口控制台）
 *
 *   ★ 2026-09-25：原「独立测试模式（APU_LINK_MODE_TEST）」+ `@link:test/prod`
 *     运行期切换 + `app_standalone()` **已整段删除**。理由：同一份固件不能有
 *     两个互斥的消息出口，否则"上电到底在跑哪条路"要靠猜 —— 那就是变量控不住。
 *     飞腾传承工程也只有 send_str_to_linux() 一条出口，为对齐也必须只留一条。
 *     详见 apu_link.h 顶部。
 *
 *  【串口控制台还在，但它不是第二条出口】
 *     apu_testlink.c 的 `@` 命令保留（@help @mode @stat @hb @nav @ctrl @wdt
 *     @wdt:regs @i2c @i2cprobe @sonar:read @sonar:stat @sonar:reset
 *     @imu:stat @imu:raw @imu:loop @esc:stop @esc:regs @rep @shutdown）。
 *     它只是把命令喂给业务层，走的是和 rpmsg 收到 DEVICE_CORE_CHECK 时
 *     **完全相同的函数**（apu_app_handle_command），不会往 Linux 发东西。
 *     用途：Linux 还没起来 / 起不来时，唯一能看外设真实状态的路。
 *
 *  ⚠️ 重要前提：**APU1 必须被"放核"才会运行**。
 *      Linux 的 remoteproc 通过 SMC 放核（甲模式，运行时加载）：
 *          echo apu1_proto_v1.elf > /sys/class/remoteproc/remoteproc0/firmware
 *          echo start             > /sys/class/remoteproc/remoteproc0/state
 * ===========================================================================
 */

#include "main.h"

#include "al_core.h"
#include <stdlib.h>
#include <stdio.h>
#include <openamp/open_amp.h>
#include <openamp/version.h>
#include <metal/alloc.h>
#include <metal/version.h>
#include "platform_info.h"
#include "rsc_table.h"

/* 传输层 + 链路层 + 业务层 + 时间基（正式头文件声明，不手写 extern） */
#include "apu_msg_api.h"
#include "apu_link.h"
#include "apu_app.h"
#include "apu_time.h"
#include "apu_testlink.h"
#include "apu_platform.h"     /* ★ 平台初始化：共享区缓存属性 + 地址自检 */
#include "apu_gic.h"          /* ★ GIC：本核 Rdist/CPU 接口拉起（SLAVE 语义） */
#include "apu_wdt.h"          /* ★ 主循环进度诊断用：apu_wdt_feed() */
#include "apu_env.h"          /* ★ 三路环境传感器：采集调度 + rpmsg 上报 */
#include "apu_gasflow.h"      /* ★ 气路控制：电磁阀 + 吸气泵（严格开泵/关阀顺序）*/

/* ⚪ 2026-09-25：原 APU_PROTOCOL_MODE 开关已删除。
 *   它用来在"协议版（心跳/命令/上报）"和"纯 echo 版"之间切；纯 echo 那条
 *   从 APU1 的端点把报文原样发回去，**内核 rpmsg 层会直接丢弃**
 *   （Linux 侧由 rpmsg_ctrl 建端点，回程必须发往该端点地址），
 *   所以那条路本来就是假的，留着只会误导联调。现在固定走协议版。 */

/* 1 = 每收到一包就在串口打一行（联调期很有用，能证明"去程收到了"）
 * 0 = 静音（压测上千包时建议关掉，否则串口会拖慢速度） */
#define APU_DBG_PRINT_RX    1

#define SHUTDOWN_MSG    0xEF56A55A

#define LPRINTF(format, ...)  AL_LOG(AL_LOG_LEVEL_INFO, format, ##__VA_ARGS__)
#define LPERROR(format, ...)  AL_LOG(AL_LOG_LEVEL_ERROR, format, ##__VA_ARGS__)

static struct rpmsg_endpoint lept;
static int shutdown_req = 0;

/* ★ 2026-09-25 主循环进度标记（诊断用，定位看门狗复位时卡在哪一步）。
 *   1=platform_poll 2=apu_time_poll 3=apu_periodic
 *   4=apu_app_periodic 5=apu_testlink_poll
 *   卡在 wfi 时时间不前进，只有这个"最后进入的步骤号"能留下线索。 */
static volatile uint32_t g_last_step = 0U;

/* 若你的 SDK 提供 psci_cpu_pwrdown，可取消注释下面一行以支持 "echo stop" 后停机 */
/* extern void psci_cpu_pwrdown(void); */
extern AL_VOID AlGicv3_CpuIfDisable(AL_U32 ProcNum);

/* ---------------------------------------------------------------------------
 * ⚠️ AlGic_CorePos() 必须**自己声明** —— 这个 SDK 没有给它公开原型
 * ---------------------------------------------------------------------------
 *  它定义在 chip\dr1x90\dr1m90\src\al_gic.c:35，但**没有任何头文件声明它**：
 *      · al_osal.h:372 那一份被 #ifdef RTOS_FREERTOS + #ifdef SMP 包着，
 *        我们裸机（无 RTOS）根本走不到 -> 等于没有
 *      · SDK 自己的做法就是**在每个调用点 local extern**：
 *          al_gicv3.c:94/113/159/232、al_gicv3_main.c:105、
 *          al_gicv3_private.c:158、al_dr1m90.c:13
 *  所以我们照做（见下面这行），否则 -Wimplicit-function-declaration 会报，
 *  而且万一将来签名变了编译器不会提醒。
 *
 *  （实际不影响运行：隐式声明按 int 处理，而它返回 AL_U32，
 *    在 AArch64 上都是 32 位、走 w0 返回，值是对的。但声明一下更干净。）
 * --------------------------------------------------------------------------- */
extern AL_U32 AlGic_CorePos(void);

/*-----------------------------------------------------------------------------*
 *  RPMSG 端点回调
 *---------------------------------------------------------------------------*/
static int rpmsg_endpoint_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
			     uint32_t src, void *priv)
{
	(void)priv;
	(void)src;

	/* 收到 shutdown 消息：回一个 ACK，然后退出主循环 */
	if (len >= sizeof(unsigned int) &&
	    (*(unsigned int *)data) == SHUTDOWN_MSG) {
		LPRINTF("shutdown message is received.");
		if (rpmsg_send(ept, data, len) < 0)
			LPERROR("rpmsg_send failed");
		LPRINTF("Send ACK back to host");
		shutdown_req = 1;
		return RPMSG_SUCCESS;
	}

#if APU_DBG_PRINT_RX
	/* 把收到的内容打到串口（联调时这是"确实收到了"的最直接证据）。
	 * 非可见字符打成 '.'，这样 echo_test 发的 0xA5 二进制也不会刷屏乱码。 */
	{
		char txt[65];
		size_t i, n = (len < sizeof(txt) - 1) ? len : sizeof(txt) - 1;

		for (i = 0; i < n; i++) {
			unsigned char c = ((const unsigned char *)data)[i];
			txt[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
		}
		txt[n] = '\0';
		LPRINTF("APU1 RX %u bytes: \"%s\"", (unsigned int)len, txt);
	}
#endif

	/* 交给传输层解析并分发 */
	return apu_dispatch(ept, data, len);
}

static void rpmsg_service_unbind(struct rpmsg_endpoint *ept)
{
	(void)ept;
	LPRINTF("unexpected Remote endpoint destroy");
	shutdown_req = 1;
}

/*-----------------------------------------------------------------------------*
 *  生产模式主循环
 *---------------------------------------------------------------------------*/
static int app(struct rpmsg_device *rdev, void *priv)
{
	int ret;

	LPRINTF("Try to create rpmsg endpoint.");

	/* ===========================================================================
	 * ★★ 端点地址（2026-09-25 改成与飞腾传承工程**逐字一致**）★★
	 * ===========================================================================
	 *  飞腾 slaver_00_example.c:1177 原文：
	 *      rpmsg_create_ept(&lept, rdev, RPMSG_SERVICE_NAME, 0, RPMSG_ADDR_ANY,
	 *                       rpmsg_endpoint_cb, rpmsg_service_unbind);
	 *                                  src = 0  ^
	 *
	 *  为什么 src=0 也能通（别再被旧注释"必须是 1024"带偏）：
	 *    那条推导针对的是"Linux 侧按设备名 virtio0.rpmsg-xxx.-1.1024 open"的场景。
	 *    而本项目的 Linux 代理（rpmsg-demo-single.c:429-447）走的是另一条路：
	 *        open("/dev/rpmsg_ctrl0") -> ioctl(RPMSG_CREATE_EPT_IOCTL)
	 *        -> open("/dev/rpmsg0")
	 *    它自己建端点、自己拿设备节点，**根本不看从核的 ns 名字/地址**。
	 *    回程靠的是收到报文时把 ept->dest_addr 回填成 src
	 *    （本文件 rpmsg_endpoint_cb -> apu_link_send 用的就是这个）。
	 *  所以 src 填 0 还是 ANY 都能跑；为了"和飞腾一样"，这里取 0。
	 *
	 *  ⚠️ 仍然只创建【一个】端点。Linux 侧按 /dev/rpmsg0 取第一个，
	 *     多建一个端点会让它拿到 1025 而 Linux 还在找 1024。
	 * ===========================================================================
	 */
	ret = rpmsg_create_ept(&lept, rdev, RPMSG_SERVICE_NAME,
			       0, RPMSG_ADDR_ANY,
			       rpmsg_endpoint_cb,
			       rpmsg_service_unbind);
	if (ret) {
		LPERROR("Failed to create endpoint.");
		return -1;
	}

	/* 把实际拿到的地址打出来，上板一眼能确认 */
	LPRINTF("Successfully created rpmsg endpoint. addr=%u (飞腾取 0；ANY 会得到 1024)",
		(unsigned int)lept.addr);

	/* ★ 必须登记端点：链路层要靠它发 PONG / ACK / 上报 */
	apu_set_endpoint(&lept);
	LPRINTF("Protocol mode: data_packet (PING->PONG, CMD->ACK, SENSOR report)");

	/* =====================================================================
	 * ★★ 串口控制台并进主循环（2026-09-25，方案 A）
	 * =====================================================================
	 *  【为什么必须放在这里，不能更早】
	 *    apu_testlink_init() -> apu_console_init() 会用 AlIntr_RegHandler()
	 *    把日志串口(UART1)的 ISR 换成我们自己的 console_irq_handler。
	 *    而让 CPU 真正开始接受 IRQ 的是：
	 *        platform_init() -> init_system() -> app_gic_initialize()
	 *            -> AlIntr_SetLocalInterrupt(AL_FUNC_ENABLE)
	 *    启动汇编 al_aarch64_start.S:27 执行了 msr daifset,0xF，
	 *    第 30 行只清了 A(bit2)，**IRQ 的 I(bit1) 一直是屏蔽的**。
	 *    ⇒ 控制台初始化必须排在 platform_init() **之后**（本函数就在其后），
	 *      否则串口会退回轮询兜底甚至收不到（历史上踩过："串口命令没反应"）。
	 *
	 *  【为什么放在 apu_app_init() 之前】
	 *    apu_app_init() 里有 I2C 扫描 + PCA9685 解锁（含 3 秒延时），
	 *    控制台先起来，这段时间的日志就能被收集；顺序对功能无影响。
	 * ===================================================================== */
	if (apu_testlink_init() != 0)
		LPERROR("apu_testlink_init() failed - UART console unavailable "
			"(rpmsg path is NOT affected)");

	if (apu_app_init() != 0)
		LPERROR("apu_app_init() returned non-zero (some peripheral failed)");

	/* =====================================================================
	 * ★ 三路环境传感器：下潜深度 / 浑浊度 / 四路气体
	 * =====================================================================
	 *  三路的采集调度与 rpmsg 上报统一由 apu_env.c 负责（见该文件顶部说明）：
	 *      · 主循环约 1 秒一圈（**被 APU0 约 1Hz 的 SGI 节流**，循环体无延时）
	 *      · **一圈把三路全部读完** ⇒ 每路约 1 秒更新一次
	 *        （旧版每 2 秒才读一路，每路要 4~6 秒，已废弃）
	 *      · 上报合并成一条 "ENV:..." 消息（格式/单位/无效语义见 apu_msg_api.h）
	 *
	 *  物理端口（板卡 MLK-F3P-CZ02 / DR1M90GEG400）：
	 *      UART-A 0x80400000  气体传感器    CEP pin15(V11)=TX  pin16(V10)=RX  TTL
	 *      UART-B 0x80500000  浑浊度 XM3318B 板载 RS485：L17=TX  N16=RX  N15=DE
	 *      UART-C 0x80600000  下潜超声      CEP pin13(U9)=TX   pin14(U8)=RX   115200
	 *
	 *  ⚠️ 波特率是 **TD 综合时定死的**（AXI UART Lite 无运行期波特率寄存器）：
	 *      A(气体)=9600  B(浊度)=9600  C(超声)=115200 —— 与最新位流一致。
	 *  ⚠️ 初始化失败**不致命**：能用的那几路照常上报，失败的对应 *V 字段为 0。
	 * ===================================================================== */
	{
		int env_rc = apu_env_init();

		if (env_rc & 0x1)
			LPERROR("[ENV] 下潜超声(UART-C) 初始化失败");
		if (env_rc & 0x2)
			LPERROR("[ENV] 浑浊度(UART-B/RS485) 初始化失败");
		if (env_rc & 0x4)
			LPERROR("[ENV] 气体传感器(UART-A) 初始化失败");
		if (env_rc == 0) {
			LPRINTF("[ENV] 三路环境传感器就绪 "
				"(深度UART-C@0x80600000 浊度UART-B@0x80500000 气体UART-A@0x80400000)");
		} else {
			LPRINTF("[ENV] 部分就绪 rc=0x%X（失败的那路本次无数据）",
				(unsigned int)env_rc);
		}
	}

	/* =====================================================================
	 * ★ 气路控制：电磁阀 + 吸气泵（PL_GPIO bit3/bit4 → pin57/58）
	 * =====================================================================
	 *  引脚：V7(pin57) = 电磁阀   W6(pin58) = 吸气泵   均为**高电平有效**
	 *
	 *  ★★ 时序铁律（顺序绝不可颠倒）★★
	 *      开：  阀先开 → 等 2.0s → 泵后开       （防负压冲击损坏传感器）
	 *      关：  泵先关 → 等 0.5s → 阀后关       （防阀关泵转憋压）
	 *      吸气：6.0s      全程 8.5s
	 *  顺序由 apu_gasflow.c 的**非阻塞状态机**保证，绝不出现同时开关或颠倒。
	 *
	 *  ⚠️ 初始化会**先把两路都写 0**（阀关、泵关）。PS 的 GPIO 输出寄存器
	 *     复位值本来就是 0，这里再显式写一次是防御热复位残留 ——
	 *     绝不能让上电瞬间泵误转。硬件上 N-MOS 栅极另有 10k 下拉，双保险。
	 *
	 *  控制方式（队友暂不在，先用本机串口1）：
	 *      @gas           查状态
	 *      @gas:start     启动一次吸气流程
	 *      @gas:stop      急停（立即关泵关阀）
	 *      也接受远程文本 CMD:GAS_SAMPLE_START / CMD:GAS_STOP（走 rpmsg 时同一函数）
	 * ===================================================================== */
	if (apu_gasflow_init() != 0)
		LPERROR("[GASFLOW] 初始化失败 - 电磁阀/吸气泵 GPIO 不可用，"
			"液路不会动作（其余功能不受影响）");

	LPRINTF("[APU1][loop] entering main loop (t=%u ms)", (unsigned int)apu_time_ms());

	/* =====================================================================
	 * ★★ 2026-09-25 新增：主循环进度标记（定位"卡在哪一步"）★★
	 * =====================================================================
	 *  为什么不能只靠 platform_poll() 里那行"等了多少秒"的诊断：
	 *     那个诊断**必须靠 apu_time_ms() 前进**才能触发。
	 *     如果卡在 _rproc_wait()（wfi 休眠等中断）且再也拿不到中断，
	 *     那么时间/循环都不会前进 —— 这类卡点**测不出来、也打不出来**。
	 *
	 *  所以这里改用**不依赖时间**的办法：
	 *     每次进主循环就把 g_last_step 更新成"即将执行哪一步"，
	 *     并立刻喂狗、每秒打一行当前 step。
	 *     这样日志停在哪一步，就说明卡在那一步（或它的下一次进入）。
	 *
	 *  读法（日志里 `[APU1][loop] step=N iter=K`）：
	 *     1 = 卡在 platform_poll()   （无限等 Linux 的 SGI）
	 *     2 = 卡在 apu_time_poll()
	 *     3 = 卡在 apu_periodic()
	 *     4 = 卡在 apu_app_periodic()（心跳门/IMU/超声/控制流水线，唯一常规喂狗点）
	 *     5 = 卡在 apu_testlink_poll()（串口控制台）
	 *     而且是**第几轮**卡的也看得见：iter 的增长速度能看出主循环转得多快。
	 * ===================================================================== */
	{
		uint32_t loop_iter = 0U;
		uint32_t loop_last_print = apu_time_ms();

		while (1) {
			g_last_step = 1U;
			apu_wdt_feed();
			{
				uint32_t now = apu_time_ms();
				if ((uint32_t)(now - loop_last_print) >= 1000U) {
					loop_last_print = now;
					LPRINTF("[APU1][loop] step=%u iter=%u t=%u ms",
						(unsigned int)g_last_step,
						(unsigned int)loop_iter,
						(unsigned int)now);
				}
			}
			platform_poll(priv);

			g_last_step = 2U;
			apu_time_poll();

			g_last_step = 3U;
			apu_periodic();

			g_last_step = 4U;
			apu_app_periodic();

			g_last_step = 5U;
			apu_testlink_poll();

			/* ★ 气路状态机推进（电磁阀/吸气泵）。
			 *   ⚠️ **非阻塞**：只看时间，到点才动作，绝不在这里 sleep。
			 *      这一点是刻意的 —— 主循环还要跑 rpmsg，2026-10-14 那次
			 *      Linux 侧 Connection reset 就是主循环被长时间占住造成的。
			 *   顺序铁律（阀先泵后开、泵先阀后关）由 apu_gasflow.c 保证。 */
			apu_gasflow_poll();

			/* =====================================================
			 * ★ 三路传感器**轮转**：每圈只让一路做收发
			 * =====================================================
			 *  【为什么要轮转】
			 *    本工程主循环约 **1000ms 一圈**，而一帧只花 7~26ms 就
			 *    到齐，IP 的 RX FIFO 只有 16 字节。所以每次收发都必须在
			 *    **一次调用内当场收完**（见 apu_gas.c 的说明），
			 *    单路最长阻塞 100ms。
			 *    三路若都在同一圈里做，就是 300ms 叠在一圈；
			 *    轮转后每圈最多 100ms（占比 10%），rpmsg 不会被饿死
			 *    —— 2026-10-14 那次 Connection reset 就是三路
			 *    **每圈都等满** 60+300+600=960ms 造成的。
			 *
			 *  【顺序】下潜 → 气体 → 浑浊度 → 下潜 …
			 *    各路自己的周期门限仍在各自 _poll() 内部生效，
			 *    轮转只是"错开"，不改变各自的采样率。
			 * ===================================================== */
			/* ★ 三路环境传感器（下潜深度 / 浑浊度 / 四路气体）：
			 *   采集调度（轮转 + 陈旧优先）与 rpmsg 上报都封装在 apu_env.c。
			 *   ⚠️ 这里**不能再**直接调 apu_depth_poll()/apu_gas_poll()/
			 *      apu_turb_poll() —— 那会绕过限速，把一圈拖到 300ms+，
			 *      重新引入 2026-10-14 那次 Connection reset 的隐患。 */
			apu_env_poll();

			loop_iter++;

			if (shutdown_req || apu_shutdown_requested()) {
				LPRINTF("[APU1] shutdown requested (source: %s) -> exiting main loop",
					shutdown_req ? "shutdown msg / ept unbind"
						     : "DEVICE_CORE_STOP");
				break;
			}
		}
	}

	rpmsg_destroy_ept(&lept);

	return 0;
}

/* ⚪ 2026-09-25：原 `app_standalone()` 已整段删除（方案 A）。
 *  它实现的是"独立测试模式主循环"：不初始化 remoteproc/rpmsg，
 *  只跑 time_poll/app_periodic/testlink_poll。
 *  现在固件只有一条路径 —— OpenAMP/RPMsg 主循环（上面那个 app()），
 *  串口控制台已经并进它里面（见 apu_testlink_init/poll 的接入点）。
 *  另外注意：原来只有 app_standalone() 会调
 *      AlIntr_SetLocalInterrupt(AL_FUNC_ENABLE)
 *  ——那是"独立测试模式不碰 platform_init"导致的补丁。现在生产路径走
 *  platform_init() -> init_system() -> app_gic_initialize()，
 *  **已经**在使能本地中断，不需要再补。 */

/*-----------------------------------------------------------------------------*
 *  跨核约定自检（只打印，不做任何 rpmsg 初始化）
 *  （外设/平台配置的体检在 apu_app_selfcheck() 里）
 *---------------------------------------------------------------------------*/
static void print_config(void)
{
	extern struct remote_resource_table resources;
	unsigned long rsc_pa = (unsigned long)&resources;

	LPRINTF("========== APU1 跨核约定自检 ==========");
	LPRINTF("APU1 core id (AlGic_CorePos) : %u",
		(unsigned int)AlGic_CorePos());
	LPRINTF("firmware build time : %s %s", __DATE__, __TIME__);
	LPRINTF("firmware link       : PROTOCOL v1 + RPMsg (single TX path)");
	LPRINTF("firmware base      : 0x%08lX  (期望 0x08000000)",
		RSC_TABLE_PA);
	LPRINTF("resource table @   : 0x%08lX  size=%u",
		rsc_pa, (unsigned int)sizeof(resources));
	LPRINTF("  -> 若这里不是 0x08000000，说明链接脚本没生效");
	LPRINTF("vdev0buffer        : 0x%08lX  size=0x%lX",
		SHARED_MEM_PA, SHARED_MEM_SIZE);
	LPRINTF("vring0 (TX)        : 0x%08lX  (RING_TX)", RING_TX);
	LPRINTF("vring1 (RX)        : 0x%08lX  (RING_RX)", RING_RX);
	LPRINTF("vring bd num/align : %d / 0x%X", VRING_BD_NUM, VRING_ALIGN);
	LPRINTF("SGI listen (from APU0) : %d   <-- 应收到这个号",
		SGI_NOTIFICATION);
	LPRINTF("SGI send   (to   APU0) : %d", SGI_TO_NOTIFY);
	LPRINTF("  -> 若收到的是另一个号，把 apu1_openamp_config.h 里");
	LPRINTF("     APU1_SGI_SWAP 改成 1 重新编译");
	LPRINTF("======================================");
}

/*-----------------------------------------------------------------------------*
 *  入口
 *---------------------------------------------------------------------------*/
void openamp_sample(void *Parameters)
{
	void *platform = NULL;
	struct rpmsg_device *rpdev;
	int ret;

	(void)Parameters;

	/* ★ 先把时间基建起来（后面所有超时判断都靠它） */
	if (apu_time_init() != 0)
		LPERROR("apu_time_init() failed - timing will be imprecise");

	/* =====================================================================
	 * ★★ 平台初始化必须在这里（2026-09-25 接入）★★
	 *   ① 把 vdev0buffer / vring 从 Cacheable 改成 Normal-Non-Cacheable
	 *      —— 必须在**任何**共享内存访问之前；platform_init() 之后的
	 *      remoteproc_mmap()/rsp 解析都算"访问"。
	 *   ② 地址自检（资源表是否真在 0x08000000、镜像有没有踩进共享缓冲）
	 * ===================================================================== */
	(void)apu_platform_init();

	/* =====================================================================
	 * ★★ 本核 GIC 拉起（2026-09-25 接入）★★
	 *   背景见 apu_gic.h：本工程构建下 SDK 的 AlGic_Init() **从未被执行**，
	 *   导致 AlGicv3_RdistInit()/CpuIfEnable() 都没跑 —— SGI 通知链是断的。
	 *   这里显式补上（SLAVE 语义：只配本核，绝不碰 Distributor）。
	 *   失败不致命：只打印，让固件继续跑，方便上板看看到底卡在哪一步。
	 * ===================================================================== */
	if (apu_gic_up() != APU_GIC_OK)
		LPERROR("[APU1] GIC bring-up did NOT complete - rpmsg notify (SGI) "
			"will not work. See log above.");
	apu_gic_dump();

	/* 等一等 APU0 把 remoteproc 启动流程走完（创龙官方也是 200ms） */
	AlSys_UDelay(200000);

	/* 最早的打印：只要串口能看到这一行，说明 APU1 确实被拉起来了 */
	LPRINTF("\r\n\r\n*** APU1 baremetal OpenAMP starting ***");

	/* 链路层（清统计 + 打一行日志；出口只有 rpmsg 一条） */
	(void)apu_link_init();

	print_config();

	/* ★ 外设/平台配置体检：只读寄存器和打印 */
	apu_app_selfcheck();

	/* =====================================================================
	 * ⚪ 2026-10-13 已删除：两个 PL 串口上电测试
	 * =====================================================================
	 *  原先这里有：
	 *    ① APU_PLUART_BOOT_SELFTEST   → apu_pluart_selftest_gas_and_ultra()
	 *       给 A/C 两路发 16 字节再从 RX 读回比对（需 TX/RX 短接），约 0.4s
	 *    ② APU_PLUART_485_BOOT_STREAM → apu_pluart_485_boot_stream()
	 *       在 RS485(B) 上每 50ms 发 96 字节 0x55，**整整 60 秒**，期间
	 *       DE 全程拉高独占总线，而且卡在 platform_init() 之前不做核间通信
	 *
	 *  为什么删：这两段是"手上没有真外设、只想验 FPGA 通路"用的台架手段。
	 *  三路串口已在实测中确认为好，真传感器也已到位，留着只有坏处：
	 *      · 每次上电白等（0.4s + 60s）
	 *      · 60 秒内双核通信完全没建立，极易被误判成"通信坏了"
	 *      · RS485 总线被自己占着，浑浊度传感器没法工作
	 *      · 日志被进度行刷屏，真正的启动信息被淹掉
	 *  以后要复现当初的验证，从工程外的存档里取回那版 apu_pluart.c 即可。
	 *
	 *  ⚠️ 替代做法：三路串口的底层原语仍然保留在 apu_pluart.h/.c
	 *     （apu_pluart_write / _getc / _rx_flush / _rs485_de），
	 *     真传感器驱动直接用这些即可，不必再写寄存器级代码。
	 * ===================================================================== */

	/* =====================================================================
	 *  ★ 2026-09-25：原「独立测试模式 / 生产模式」分支**已整段删除**。
	 *    现在固件只有一条路径 —— OpenAMP/RPMsg（见 apu_link.h 顶部）。
	 *    串口控制台仍然可用（apu_testlink.c 的 @ 命令），但它只是把命令
	 *    喂给业务层，**不是**第二条消息出口。
	 * ===================================================================== */

	LPRINTF("openamp lib version: %s (", openamp_version());
	LPRINTF("Major: %d, ", openamp_version_major());
	LPRINTF("Minor: %d, ", openamp_version_minor());
	LPRINTF("Patch: %d)", openamp_version_patch());

	LPRINTF("libmetal lib version: %s (", metal_ver());
	LPRINTF("Major: %d, ", metal_ver_major());
	LPRINTF("Minor: %d, ", metal_ver_minor());
	LPRINTF("Patch: %d)", metal_ver_patch());

	LPRINTF("Starting application...");

	ret = platform_init(0, NULL, &platform);
	if (ret) {
		LPERROR("Failed to initialize platform.");
	} else {
		rpdev = platform_create_rpmsg_vdev(platform, 0,
						   VIRTIO_DEV_DEVICE,
						   NULL, NULL);
		if (!rpdev) {
			LPERROR("Failed to create rpmsg virtio device.");
		} else {
			app(rpdev, platform);
			platform_release_rpmsg_vdev(rpdev, platform);
		}
	}

	LPRINTF("Stopping application...");

	platform_cleanup(platform);

	/* 如果想支持 Linux 侧 echo stop 后彻底停掉 APU1，恢复下面两行：
	 *   AlGicv3_CpuIfDisable(1);
	 *   psci_cpu_pwrdown();
	 */
	AlGicv3_CpuIfDisable(1);
}

int main(void)
{
	openamp_sample(AL_NULL);
	return 0;
}
