/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apu_i2c.c —— APU1 裸机侧 I2C 总线抽象实现
 * ===========================================================================
 *  【★ 2026-09-18 重要修正：必须用 Block 版（带超时），不能用 Polling 版】
 *
 *  上板实测：用 `AlIic_Hal_MasterSendDataPolling()` / `MasterRecvDataPolling()`
 *  做"写寄存器地址 → 读数据"的复合读时，**整个固件会卡死在
 *  al_iic_dev.c:705 的 `while(AlIic_ll_IsMasterActivity(IicBaseAddr));`**
 *  —— 那是个**没有超时**的死循环，一旦进去就永远出不来
 *  （表现：串口停在 "scanning bus0" 之后，主循环起不来，串口命令也没反应）。
 *
 *  为什么会进去（链条）：
 *    ① 我们设 CmdOption = RESTART（想拼成一次 repeated-START 复合读）
 *    ② MasterSendDataPolling() **把字节写进 TX FIFO 就返回了** ——
 *       SDK 的 RESTART 分支不等传输结束（只有 STOP 分支才等，见 484-486 行）
 *    ③ 我们紧接着读 abort 寄存器 → 传输还没结束 → 读到 0 → **误判成功**
 *    ④ 于是调 MasterRecvDataPolling()，而它第一件事是 AlIic_Dev_MasterSetTar()
 *       做 "关控制器 → 改 IC_TAR → 开控制器"
 *    ⑤ ⚠️ 此刻总线**还被上一步占着** → 关不掉 / 主状态机状态错乱
 *    ⑥ 最后卡在 705 行那个无超时等待上
 *
 *  ⇒ **改用官方例程的写法**（`09_ps_iic_eeprom/.../i2cps_polled.c` 就是这么写的）：
 *      · `AlIic_Hal_MasterSendDataBlock(...)`  /  `AlIic_Hal_MasterRecvDataBlock(...)`
 *        —— 这两个**带 Timeout 参数**，内部 `AlIic_Hal_WaitMasterTxDoneOrTimeout()`
 *           会等到 TX_DONE 或 TX_ABRT，最差超时返回，**绝不会永久卡住**
 *        —— 而且**返回真实错误码**（Polling 版永远返回 AL_OK）
 *      · 复合读拆成**两次独立事务**（写地址一趟、读数据一趟），绝不 overlapping
 *
 *  注意那个文件名叫 `i2cps_polled.c`，但内部用的就是 Block 版 —— 别被名字骗了。
 *
 *  【为什么仍然要自己读 abort 寄存器】
 *
 *  Block 版虽然会返回错误码，但**具体是"没人应答"还是"总线被拉死"分不出来**，
 *  诊断时要看这个。所以两个机制**并存**：
 *      · Block 版返回值  → 判断"这次成不成"（且有时间上限）
 *      · IC_TX_ABRT_SOURCE → 判断"为什么不成"（NACK / SDA 卡死 / …）
 *
 *  ⚠️ 读之前要先 `AlIic_ll_ClrTxAbrt()` 清掉上一次的旧状态，
 *    否则会把历史错误当成这次的。
 *
 *  ⚠️ 必须忽略 bit16 (USER_ABRT)：SDK 的 AlIic_Dev_MasterSetTar() 每次都会
 *    "disable -> 改地址 -> enable"，这个动作本身就会置 USER_ABRT，
 *    不忽略的话**每次传输都会被误判为失败**。
 * ===========================================================================
 */

#include "apu_i2c.h"

#include <string.h>

#include "al_core.h"
#include "al_iic_hal.h"
#include "al_osal.h"          /* AlOsal_Mb_Send（自写事件回调要投队列） */
#include "al_iic_dev.h"
#include "al_iic_ll.h"

/* ---------------------------------------------------------------------------
 * 每条总线的状态
 * --------------------------------------------------------------------------- */
typedef struct {
	AL_IIC_HalStruct *handle;
	uint32_t          last_abort;
	uint32_t          tx_count;
	uint32_t          err_count;
} apu_i2c_bus_ctx_t;

static apu_i2c_bus_ctx_t g_bus[APU_I2C_NUM_BUS];
static int g_i2c_ready;

/* ---------------------------------------------------------------------------
 * 事务参数（★ 换成 Block 版 API 之后才有"超时"这个概念）
 * ---------------------------------------------------------------------------
 *  APU_I2C_TIMEOUT_MS —— 单次 I2C 事务的最长等待时间。
 *      正常设备几微秒就应答；没设备时 NACK 也是微秒级。
 *      这个超时只是"兜底"：保证任何异常都不会把主循环拖住。
 *      扫描 112 个地址，最坏 112×(超时+延时)；20ms 时约 2.5 秒/总线，可接受。
 *
 *  APU_I2C_STEP_DELAY_MS —— "写寄存器地址"与"读数据"两次事务之间的间隔。
 *      官方例程给的是 100ms（那是给 EEPROM 写周期留的余量）；
 *      我们这里只是分开两次事务，2ms 足够。
 * --------------------------------------------------------------------------- */
#define APU_I2C_TIMEOUT_MS      20U
#define APU_I2C_STEP_DELAY_MS   2U

/* IC_TX_ABRT_SOURCE 里与"从机不响应 / 传输被中止"相关的位。
 * 参考 al_iic_ll.h 的 AL_IIC_ABRT_* 定义。
 * ★ 故意不包含 USER_ABRT(bit16) —— 见文件头说明。
 *
 * ⚠️ 注意：宏体里**不能**再夹块注释 ——
 *    注释在宏替换之前就被剥掉了，里面的 '*' 会让这个表达式直接编译不过。
 *    位含义写在这里：
 *        bit0  ABRT_7B_ADDR_NOACK   7 位从机地址无应答
 *        bit1  ABRT_10ADDR1_NOACK   10 位地址第 1 字节无应答
 *        bit2  ABRT_10ADDR2_NOACK   10 位地址第 2 字节无应答
 *        bit3  ABRT_TXDATA_NOACK    数据字节无应答
 *        bit4  ABRT_GCALL_NOACK     广播地址无应答
 *        bit5  ABRT_GCALL_READ      广播读不允许
 *        bit11 ABRT_MASTER_DIS      主机被关闭
 *        bit17 ABRT_SDA_STUCK_LOW   SDA 被拉死
 *        bit18 ABRT_DEVICE_NOACK    从机无应答
 *        bit19 ABRT_SLAVEADDR_NOACK 从机地址无应答
 */
#define APU_I2C_ABORT_MASK      ((1UL << 0)  | \
				 (1UL << 1)  | \
				 (1UL << 2)  | \
				 (1UL << 3)  | \
				 (1UL << 4)  | \
				 (1UL << 5)  | \
				 (1UL << 11) | \
				 (1UL << 17) | \
				 (1UL << 18) | \
				 (1UL << 19))

/* ---------------------------------------------------------------------------
 * 内部：清错误 / 读错误
 * --------------------------------------------------------------------------- */
static void i2c_clear_abort(AL_IIC_HalStruct *h)
{
	(void)AlIic_ll_ClrTxAbrt((AL_REG)h->Dev.HwConfig.BaseAddress);
}

static uint32_t i2c_read_abort_raw(AL_IIC_HalStruct *h)
{
	return AL_REG32_READ((AL_REG)h->Dev.HwConfig.BaseAddress +
			     I2C__IC_TX_ABRT_SOURCE__OFFSET);
}

/* 返回 1 = 这次传输被中止（NACK 等），0 = 正常 */
static int i2c_check_abort(apu_i2c_bus_t bus, AL_IIC_HalStruct *h)
{
	uint32_t raw = i2c_read_abort_raw(h);
	uint32_t bad = raw & APU_I2C_ABORT_MASK;

	g_bus[bus].last_abort = raw;
	return (bad != 0U) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * 初始化
 * --------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * I2C 事件回调（★ 2026-09-18 晚加，替代 SDK 的默认回调）
 * ---------------------------------------------------------------------------
 *  为什么要自己写一个：
 *    SDK 的 AlIic_DefEventCallBack()（al_iic_hal.c:45）在**每一笔 TX_ABRT** 上都打
 *      AL_LOG(..., "AL_IIC_EVENT_TX_ABRT source:0x%x\r\n", ...)
 *    而扫一条 I2C 总线要探 112 个地址（0x08..0x77），每个不存在的地址都是一次
 *    NACK → ABRT → 一轮扫描刷 112 行，而且是在 ISR 里打。超声重试/PCA9685
 *    初始化失败也会各来一行。
 *
 *  ⚠️ 但这**不是能随便换成空函数的东西**：默认回调真正的工作是把事件投进
 *     Handle->TxRxEventQueue —— Block 版 API（AlIic_Hal_MasterSendDataBlock /
 *     MasterRecvDataBlock）就是在这个队列上等事件。
 *     所以这里**一模一样地投队列**，只是不打日志。
 *     谁改这个函数都必须保留 AlOsal_Mb_Send；漏了它，所有 Block 调用都会超时。
 *
 *  事件投递范围与 SDK 默认回调保持完全一致（RD_REQ 空分支、其余忽略）。
 * --------------------------------------------------------------------------- */
static AL_VOID apu_iic_evt_cb(AL_IIC_EventStruct *evt, AL_VOID *ref)
{
	AL_IIC_HalStruct *h = (AL_IIC_HalStruct *)ref;

	if (h == AL_NULL || evt == AL_NULL)
		return;

	switch (evt->Events) {
	case AL_IIC_EVENT_RX_UNDER:
	case AL_IIC_EVENT_RX_OVER:
	case AL_IIC_EVENT_TX_OVER:
	case AL_IIC_EVENT_TX_DONE:
	case AL_IIC_EVENT_RX_DONE:
	case AL_IIC_EVENT_TX_ABRT:
		/* ★ 必须投队列（Block 版 API 在等它）；只是不再刷屏打印 */
		(void)AlOsal_Mb_Send(&h->TxRxEventQueue, evt);
		break;

	case AL_IIC_EVENT_READY_TO_RX:
	case AL_IIC_EVENT_READY_TO_TX:
	case AL_IIC_EVENT_RD_REQ:
	case AL_IIC_EVENT_ACTIVITY:
	case AL_IIC_EVENT_STOP_DET:
	case AL_IIC_EVENT_START_DET:
	case AL_IIC_EVENT_GEN_CALL:
	default:
		break;
	}
}

static int i2c_init_one(apu_i2c_bus_t bus, uint32_t dev_id)
{
	AL_IIC_InitStruct cfg = {
		.Mode     = AL_IIC_MODE_MASTER,
		.AddrMode = AL_IIC_ADDR_7BIT,
		.SlaveAddr = 0U,
	};
	AL_S32 ret;
	AL_IIC_HwConfigStruct *hw;

	/* 先确认这条总线在平台配置里真的存在（防"假表项"陷阱） */
	hw = AlIic_Dev_LookupConfig(dev_id);
	if (hw == AL_NULL || hw->BaseAddress == 0U) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][i2c] bus%u: LookupConfig(%u) returned no valid config "
		       "(BaseAddr=0) - is I2C%uPS_ENABLE on in soc_plat.h?",
		       (unsigned int)bus, (unsigned int)dev_id, (unsigned int)dev_id);
		return -1;
	}

	ret = AlIic_Hal_Init(&g_bus[bus].handle, dev_id, &cfg, apu_iic_evt_cb);
	if (ret != AL_OK) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][i2c] bus%u: AlIic_Hal_Init(DevId=%u) failed, ret=0x%x",
		       (unsigned int)bus, (unsigned int)dev_id, (unsigned int)ret);
		g_bus[bus].handle = AL_NULL;
		return -1;
	}

	i2c_clear_abort(g_bus[bus].handle);
	g_bus[bus].tx_count = 0U;
	g_bus[bus].err_count = 0U;
	g_bus[bus].last_abort = 0U;

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][i2c] bus%u ready: DevId=%u base=0x%08X rate=%uHz (%s)",
	       (unsigned int)bus, (unsigned int)dev_id,
	       (unsigned int)hw->BaseAddress, (unsigned int)hw->RateHz,
	       (bus == APU_I2C_BUS0) ? "PCA9685 0x40" : "STM32 sonar 0x40");
	return 0;
}

int apu_i2c_init(void)
{
	int failed = 0;

	failed += (i2c_init_one(APU_I2C_BUS0, 0U) != 0) ? 1 : 0;
	failed += (i2c_init_one(APU_I2C_BUS1, 1U) != 0) ? 1 : 0;

	g_i2c_ready = (failed == 0);

	if (failed != 0) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][i2c] %d of %d bus(es) failed to init",
		       failed, APU_I2C_NUM_BUS);
		return -failed;
	}

	/* 官方例程初始化后延时 100ms 让总线稳定（i2cps_polled.c 也有这一步） */
	AlSys_MDelay(100U);

	AL_LOG(AL_LOG_LEVEL_INFO, "[APU1][i2c] both buses initialised OK");
	return 0;
}

int apu_i2c_bus_reset(apu_i2c_bus_t bus)
{
	if (bus >= APU_I2C_NUM_BUS || g_bus[bus].handle == AL_NULL)
		return -2;

	i2c_clear_abort(g_bus[bus].handle);
	g_bus[bus].last_abort = 0U;
	AlSys_MDelay(5U);
	return 0;
}

/* ---------------------------------------------------------------------------
 * 写寄存器
 * --------------------------------------------------------------------------- */
int apu_i2c_write_regs(apu_i2c_bus_t bus, uint8_t addr, uint8_t reg,
		       const uint8_t *buf, uint32_t len)
{
	AL_IIC_HalStruct *h;
	uint8_t tmp[8 + 1];
	AL_S32 ret;

	if (bus >= APU_I2C_NUM_BUS)
		return -1;
	if (len > 8U || (len != 0U && buf == AL_NULL))
		return -1;

	h = g_bus[bus].handle;
	if (h == AL_NULL)
		return -2;

	/* 拼成一个 buffer 一次发出：[reg][data...] */
	tmp[0] = reg;
	if (len != 0U)
		(void)memcpy(&tmp[1], buf, len);

	/* ★ Block 版：内部会等到 TX_DONE 或 TX_ABRT，最差超时返回，
	 *   绝不会像 Polling 版那样卡在无超时的 while 里 */
	(void)AlIic_Hal_MasterSetCmdOption(h, AL_IIC_CMD_OPTION_STOP);
	i2c_clear_abort(h);

	ret = AlIic_Hal_MasterSendDataBlock(h, (AL_U16)addr, tmp, len + 1U,
					    APU_I2C_TIMEOUT_MS);

	g_bus[bus].tx_count++;

	if (ret != AL_OK) {
		g_bus[bus].err_count++;
		(void)i2c_check_abort(bus, h);   /* 记下来，供 @i2c 诊断打印 */
		return -4;
	}
	if (i2c_check_abort(bus, h)) {
		g_bus[bus].err_count++;
		return -3;      /* NACK / abort */
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * 读寄存器（复合读：写地址 -> 停 -> 读数据）
 * ---------------------------------------------------------------------------
 *  ★ 2026-09-18 修正：**拆成两次独立事务**，不再用 CmdOption=RESTART 拼成一次。
 *
 *  官方例程 `09_ps_iic_eeprom/.../i2cps_polled.c` 的 i2cps_rd8() 就是这么写的：
 *      MasterSendDataBlock(addr, reg_addr, 1, timeout);   // 先发寄存器地址，末尾 STOP
 *      delay;
 *      MasterRecvDataBlock(addr, buf, n, timeout);        // 再读数据，末尾 STOP
 *
 *  为什么不能再用 RESTART 拼：见本文件头 "★ 2026-09-18 重要修正"。
 *  一句话：RESTART 分支下 SDK 不等传输结束就返回，会把后一次调用里的
 *  MasterSetTar()（关控制器→改地址→开控制器）插到"总线还占着"的时刻，
 *  直接把主状态机搞死，然后卡在无超时的 while 里。
 *
 *  PCA9685 / STM32 超声板都支持"STOP 之后重新 START"这种分开的读法，
 *  效果和 repeated-START 一样。
 * --------------------------------------------------------------------------- */
int apu_i2c_read_regs(apu_i2c_bus_t bus, uint8_t addr, uint8_t reg,
		      uint8_t *buf, uint32_t len)
{
	AL_IIC_HalStruct *h;
	uint8_t reg_only = reg;
	AL_S32 ret;

	if (bus >= APU_I2C_NUM_BUS)
		return -1;
	if (buf == AL_NULL || len == 0U)
		return -1;

	h = g_bus[bus].handle;
	if (h == AL_NULL)
		return -2;

	/* ① 先发寄存器地址（独立事务，末尾发 STOP） */
	(void)AlIic_Hal_MasterSetCmdOption(h, AL_IIC_CMD_OPTION_STOP);
	i2c_clear_abort(h);

	ret = AlIic_Hal_MasterSendDataBlock(h, (AL_U16)addr, &reg_only, 1U,
					    APU_I2C_TIMEOUT_MS);
	g_bus[bus].tx_count++;

	if (ret != AL_OK) {
		g_bus[bus].err_count++;
		(void)i2c_check_abort(bus, h);   /* 记下来，供 @i2c 诊断打印 */
		return -4;
	}
	if (i2c_check_abort(bus, h)) {
		/* ★ 这里的 abort 就是"从机没应答" —— 总线扫描就靠它 */
		g_bus[bus].err_count++;
		return -3;
	}

	/* 两次事务之间留一点间隔（官方给 100ms，我们只读，2ms 够） */
	AlSys_MDelay(APU_I2C_STEP_DELAY_MS);

	/* ② 再读数据（独立事务，末尾发 STOP） */
	(void)AlIic_Hal_MasterSetCmdOption(h, AL_IIC_CMD_OPTION_STOP);
	i2c_clear_abort(h);

	ret = AlIic_Hal_MasterRecvDataBlock(h, (AL_U16)addr, buf, len,
					    APU_I2C_TIMEOUT_MS);
	g_bus[bus].tx_count++;

	if (ret != AL_OK) {
		g_bus[bus].err_count++;
		(void)i2c_check_abort(bus, h);
		return -4;
	}
	if (i2c_check_abort(bus, h)) {
		g_bus[bus].err_count++;
		return -3;
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * 单寄存器便捷版
 * --------------------------------------------------------------------------- */
int apu_i2c_write_reg(apu_i2c_bus_t bus, uint8_t addr, uint8_t reg, uint8_t val)
{
	return apu_i2c_write_regs(bus, addr, reg, &val, 1U);
}

int apu_i2c_read_reg(apu_i2c_bus_t bus, uint8_t addr, uint8_t reg, uint8_t *val)
{
	if (val == AL_NULL)
		return -1;
	return apu_i2c_read_regs(bus, addr, reg, val, 1U);
}

/* ---------------------------------------------------------------------------
 * 总线扫描
 * ---------------------------------------------------------------------------
 *  判据：对该地址做一次"读 1 字节"。从机会 ACK 地址 -> 无 abort；
 *        没有设备       -> NACK       -> IC_TX_ABRT_SOURCE 置位。
 *  这就是 apu_i2c_read_regs() 返回 -3 的情形，我们把它当成"没人"。
 * --------------------------------------------------------------------------- */
int apu_i2c_scan(apu_i2c_bus_t bus)
{
	int found = 0;
	uint8_t dummy = 0U;
	uint32_t addr;

	if (bus >= APU_I2C_NUM_BUS)
		return -1;
	if (g_bus[bus].handle == AL_NULL) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][i2c] scan bus%u: bus not initialised",
		       (unsigned int)bus);
		return -2;
	}

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][i2c] --- scanning bus%u (0x08..0x77) ---",
	       (unsigned int)bus);

	for (addr = 0x08U; addr <= 0x77U; addr++) {
		int ret = apu_i2c_read_regs(bus, (uint8_t)addr, 0x00U, &dummy, 1U);

		if (ret == 0) {
			found++;
			AL_LOG(AL_LOG_LEVEL_INFO,
			       "[APU1][i2c]   bus%u: found 0x%02X  %s",
			       (unsigned int)bus, (unsigned int)addr,
			       (addr == APU_I2C_ADDR_PCA9685 &&
				bus == APU_I2C_BUS0) ? "<- PCA9685" :
			       (addr == APU_I2C_ADDR_STM32_SONAR &&
				bus == APU_I2C_BUS1) ? "<- STM32 sonar board" : "");
		}
	}

	if (found == 0) {
		AL_LOG(AL_LOG_LEVEL_ERROR,
		       "[APU1][i2c]   bus%u: NOTHING found. "
		       "Check: SCL/SDA wiring, 3.3V power, GND common, pull-ups, "
		       "and whether FSBL/BOOT.bin took effect (see selfcheck above)",
		       (unsigned int)bus);
		/* 顺手把 abort 原始值打出来，能看出是 NACK 还是 SDA 被拉死 */
		apu_i2c_dump_last_abort(bus);
	} else {
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][i2c] bus%u scan done: %d device(s) responded",
		       (unsigned int)bus, found);
	}
	return found;
}

/* ---------------------------------------------------------------------------
 * 诊断
 * --------------------------------------------------------------------------- */
uint32_t apu_i2c_last_abort_source(apu_i2c_bus_t bus)
{
	if (bus >= APU_I2C_NUM_BUS)
		return 0U;
	return g_bus[bus].last_abort;
}

void apu_i2c_dump_last_abort(apu_i2c_bus_t bus)
{
	uint32_t raw;

	if (bus >= APU_I2C_NUM_BUS)
		return;

	raw = g_bus[bus].last_abort;

	AL_LOG(AL_LOG_LEVEL_INFO,
	       "[APU1][i2c] bus%u IC_TX_ABRT_SOURCE = 0x%08X  (tx=%u err=%u)",
	       (unsigned int)bus, (unsigned int)raw,
	       (unsigned int)g_bus[bus].tx_count,
	       (unsigned int)g_bus[bus].err_count);

	if (raw == 0U) {
		/* ★ 别被这行骗了：Block 版 API 内部是"等中断事件"，
		 *   SDK 的中断处理程序在处理 TX_ABRT 事件时**已经把 abort 寄存器清掉了**。
		 *   所以"传输明明失败、这里却读到 0"是**正常现象**。
		 *   判断成败请看返回值 / err 计数，不要看这一行。 */
		AL_LOG(AL_LOG_LEVEL_INFO,
		       "[APU1][i2c]   abort 源已被 SDK 中断处理程序清掉"
		       "（失败时读到 0 属正常，请看 err 计数）");
		return;
	}
	if (raw & (1UL << 0))
		AL_LOG(AL_LOG_LEVEL_ERROR, "[APU1][i2c]   bit0  ABRT_7B_ADDR_NOACK  <- 7位地址无应答（没人或地址错）");
	if (raw & (1UL << 3))
		AL_LOG(AL_LOG_LEVEL_ERROR, "[APU1][i2c]   bit3  ABRT_TXDATA_NOACK   <- 数据字节无应答");
	if (raw & (1UL << 11))
		AL_LOG(AL_LOG_LEVEL_ERROR, "[APU1][i2c]   bit11 ABRT_MASTER_DIS     <- 主机被关闭");
	if (raw & (1UL << 17))
		AL_LOG(AL_LOG_LEVEL_ERROR, "[APU1][i2c]   bit17 ABRT_SDA_STUCK_LOW  <- SDA 被拉死（接线短路/上拉过强/设备拉死总线）");
	if (raw & (1UL << 18))
		AL_LOG(AL_LOG_LEVEL_ERROR, "[APU1][i2c]   bit18 ABRT_DEVICE_NOACK   <- 从机无应答（总线扩展版判据）");
	if (raw & (1UL << 19))
		AL_LOG(AL_LOG_LEVEL_ERROR, "[APU1][i2c]   bit19 ABRT_SLAVEADDR_NOACK<- 从机地址无应答");
}

/* ===========================================================================
 * 说明：为什么不在初始化时设速率
 * ===========================================================================
 *  AlIic_Hal_Init() 内部已经按 HwConfig->RateHz 设好了速率和 SDA hold，
 *  而这个 RateHz 来自平台配置里的 I2C0_RATE / I2C1_RATE
 *  （FD 工具已自动生成，都是 100000 = 100kHz）。
 *  所以**不需要**再调 AlIic_Hal_IoCtl(SET_RATE)，也就少一个出错点。
 * =========================================================================== */
