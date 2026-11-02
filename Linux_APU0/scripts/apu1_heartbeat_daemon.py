#!/usr/bin/env python3
"""APU1 心跳守护 —— Linux 侧主动"敲门"并持续发心跳。

===============================================================================
 这个脚本解决什么问题
===============================================================================
  裸机核 APU1 上电后**不会自己动**：它进主循环第一件事就是等 Linux 敲一下门
  （等一个叫 SGI 14 的核间中断）。这一脚敲门只有 Linux 侧**真的写出一帧
  RPMsg 数据**时才会由内核产生。

  但飞腾传承工程的做法是（reference/legacy_phytium/rpmsg-demo-single.c:427-475）：
      ① open("/dev/rpmsg_ctrl0")            开控制节点
      ② ioctl(RPMSG_CREATE_EPT_IOCTL)       建端点
      ③ open("/dev/rpmsg0")                 开数据节点
      ④ write(DEVICE_CORE_CHECK, "WakeUp")  ★ 立刻发一帧，主动踢醒裸机
      ⑤ pthread_create(heartbeat_thread)    之后每 500ms 发 PING:n

  第 ④ 步是**第一个 SGI 的来源**，第 ⑤ 步是**持续喂活裸机心跳门**的来源。
  我们原来的 Linux 侧两者都没有 —— 所以 APU1 日志里 `SGI rx=0` 一直不动，
  7.5 秒后触发"启动后未收到主机心跳"安全停车。

  本脚本把 ①~⑤ 完整搬过来，并且只用 Python 标准库（板上不用装任何东西）。

===============================================================================
 与飞腾的一致性（常量都对齐，交接时好对照）
===============================================================================
  飞腾 rpmsg-demo-single.c              本脚本
  ---------------------------------------------------------------------------
  HEARTBEAT_STARTUP_GRACE_SECONDS = 5   --grace 5.0
  HEARTBEAT_PERIOD_MS          = 500    --period 0.5
  HEARTBEAT_REPLY_TIMEOUT_MS   = 80     --reply-timeout 0.08
  HEARTBEAT_MAX_MISSES         = 5      --max-misses 5
  "PING:%u" / "PONG:%u"                 LegacyPacketCodec（同一个编解码）
  DEVICE_CORE_CHECK "WakeUp"            --wake-text WakeUp

===============================================================================
 用法
===============================================================================
  # 最常用：自动找节点、发 WakeUp、然后一直发心跳
  python3 scripts/apu1_heartbeat_daemon.py

  # 只看一次连通性（发一帧 WakeUp 就退出，用来快速验证 SGI 通不通）
  python3 scripts/apu1_heartbeat_daemon.py --once

  # 前台跑 + 详细日志（第一次联调建议这样）
  python3 scripts/apu1_heartbeat_daemon.py --foreground -v

  # 不开 UDP 命令口，只发心跳（最小干扰）
  python3 scripts/apu1_heartbeat_daemon.py --no-udp

  退出码：0 正常；1 找不到 rpmsg 设备节点；2 打不开设备
===============================================================================
"""

import argparse
import json
import os
import re
import signal
import socket
import struct
import sys
import threading
import time

# ---------------------------------------------------------------------------
# 让脚本能 import app/core 下的编解码器（和现有 bridge 共用一份，避免两套协议）
# ---------------------------------------------------------------------------
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
APP_DIR = os.path.join(PROJECT_ROOT, "app")
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

try:
    from core.rpmsg_transport import (  # noqa: E402
        DEVICE_CORE_CHECK,
        DEVICE_CORE_HEARTBEAT,
        LegacyPacketCodec,
        PacketFormatError,
        status_from_packet,
    )
except ImportError as exc:  # pragma: no cover - 交付时路径不对才走到
    sys.stderr.write(
        "[HB][FATAL] 无法导入 app/core/rpmsg_transport.py: {}\n"
        "  请确认本脚本在 <项目根>/scripts/ 下，且 app/core/rpmsg_transport.py 存在。\n".format(exc)
    )
    raise SystemExit(2)


HEARTBEAT_PING_PREFIX = "PING:"
HEARTBEAT_PONG_PREFIX = "PONG:"
_PONG_RE = re.compile(r"^PONG:(\d+)$")


# ===========================================================================
# 一、设备节点发现（对应飞腾 ①②③ 三步，但更宽容）
# ===========================================================================
#   飞腾直接写死 /dev/rpmsg_ctrl0 + /dev/rpmsg0。
#   我们板上实际是 rpmsg0..N 里的哪一个不确定（取决于内核给动态端点的编号），
#   所以这里按"数字顺序"扫一遍，谁先能被打开就用谁。
# ===========================================================================

def _numeric_suffix(path, prefix):
    try:
        return int(path[len(prefix):])
    except ValueError:
        return 1 << 30


def discover_rpmsg_devices(dev_dir="/dev"):
    """返回 (数据节点列表, 控制节点列表)，各自按编号升序。"""
    try:
        names = os.listdir(dev_dir)
    except OSError:
        return [], []

    eps, ctrls = [], []
    for name in names:
        full = os.path.join(dev_dir, name)
        if name.startswith("rpmsg_ctrl"):
            ctrls.append(full)
        elif name.startswith("rpmsg") and name[len("rpmsg"):].isdigit():
            eps.append(full)

    eps.sort(key=lambda p: _numeric_suffix(p, "rpmsg"))
    ctrls.sort(key=lambda p: _numeric_suffix(p, "rpmsg_ctrl"))
    return eps, ctrls


# ===========================================================================
# 二、端点创建（对应飞腾 ② ioctl RPMSG_CREATE_EPT_IOCTL）
# ===========================================================================
#   ⚠️ 这一步在 DR1M90 上**通常不需要**：APU1 固件启动时会做 ns announce
#      （apu1_openamp_config.h:166 的 "rpmsg-openamp-demo-channel"，
#       main.c:180 用 src=0 建端点），内核收到 announce 后会**自动**生成
#       /dev/rpmsg0。所以正常情况下节点已经在了。
#
#   保留本功能是为了两种情况：
#     · 内核还没收到 announce，节点没生成 → 我们自己建一个，顺便产生第一脚 SGI
#     · 想复刻飞腾"ioctl 建端点"的原样流程做对比实验
#
#   ioctl 号按 Linux 标准 uapi/linux/rpmsg.h 的宏**自己算**，别抄文档：
#     #define RPMSG_CREATE_EPT_IOCTL  _IOW(0xb5, 0x1, struct rpmsg_endpoint_info)
#     #define _IOC(dir,type,nr,size)  ((dir << 30) | (size << 16) | (type << 8) | nr)
#     struct rpmsg_endpoint_info { char name[32]; __u32 src; __u32 dst; };  = 40 字节
#
#     _IOW = 方向位 write → (1 << 30) = 0x40000000
#     size = 40 = 0x28  → 0x00280000
#     type = 0xb5       → 0x0000B500
#     nr   = 0x1        → 0x00000001
#     ────────────────────────────────
#     合计              = 0x4028B501
#
#   ⚠️ 2026-09-25 修正：本文件原来写的是 0xC028B501（从某篇博客抄来，没自己算，
#      那篇写的是 44 字节所以高位不同）。**队友实测指出并按宏复算改成 0x4028B501，
#      他是对的。** 教训：ioctl 号必须按宏算，不能抄。
#
#   在板子的内核头文件上核对：
#     grep -rn "define RPMSG_CREATE_EPT_IOCTL" /usr/include/linux/rpmsg.h
# ===========================================================================

RPMSG_CREATE_EPT_IOCTL = 0x4028B501
_EPT_INFO = struct.Struct("<32sII")


def create_rpmsg_endpoint(ctrl_path, name, src=0, dst=0):
    """在 ctrl_path 上建一个端点。返回 (成功?, 说明文本)。"""
    name_bytes = name.encode("utf-8")[:31]        # 留一个 NUL
    payload = _EPT_INFO.pack(name_bytes, int(src), int(dst))
    try:
        import fcntl
    except ImportError:
        return False, "本平台没有 fcntl 模块，无法 ioctl"
    try:
        fd = os.open(ctrl_path, os.O_RDWR)
    except OSError as exc:
        return False, "打不开 {}: {}".format(ctrl_path, exc)
    try:
        fcntl.ioctl(fd, RPMSG_CREATE_EPT_IOCTL, payload)
        return True, "ioctl 成功"
    except OSError as exc:
        return False, "ioctl 失败: {}".format(exc)
    finally:
        os.close(fd)


# ===========================================================================
# 三、心跳守护主体
# ===========================================================================

class HeartbeatDaemon:
    def __init__(self, args):
        self.args = args
        self.stop_event = threading.Event()
        self.write_lock = threading.Lock()

        self.fd = None
        self.device_path = None
        self.last_sent_seq = 0

        # ---- 统计（对着 APU1 串口一起看，判断方向）----
        self.tx_heartbeat = 0        # 发出去的 PING 数
        self.rx_total = 0            # 收到的任何帧数
        self.pong_ok = 0             # 收到的 PONG 且序号匹配
        self.consecutive_misses = 0
        self.fault_reported = False
        self.last_pong_at = None
        self.last_rx_text = ""

        # ---- 失联判定（用飞腾的两个常量，别再让参数白给）----
        #   飞腾 heartbeat_thread 的判据是：
        #     send 之后等 HEARTBEAT_REPLY_TIMEOUT_MS(80ms) 看有没有 PONG；
        #     连续 HEARTBEAT_MAX_MISSES(5) 次都没有 → 报"裸机连续无响应"。
        #   本脚本单线程收发，所以把"80ms 内没等到"换成等价判据：
        #     上一个 PING 距今已超过 reply_timeout，且期间没收到匹配的 PONG。
        self.pending_seq = 0          # 已发出、还没被 PONG 确认的序号
        self.pending_since = 0.0      # 该序号发出的时刻

        # ---- UDP：状态上行（沿用现有 Web 契约）+ 可选本地指令口 ----
        self.status_socket = None
        self.command_socket = None
        self.status_target = None

        # ---- Unix socket：给 Web 桥转指令用（本进程是设备唯一所有者）----
        self.unix_socket = None
        self.unix_conn = None

    # ---------------------------------------------------------------- 日志
    def log(self, msg, force=False):
        if self.args.verbose or force:
            stamp = time.strftime("%H:%M:%S")
            sys.stdout.write("[HB {}] {}\n".format(stamp, msg))
            sys.stdout.flush()

    # ---------------------------------------------------- 打开 rpmsg 节点
    def open_device(self):
        """按 --device / 自动扫描 的顺序找节点并打开。

        为什么要重试：`echo start` 之后内核需要一点时间才会把 virtio/rpmsg
        设备挂出来。早于它就打开会得到 ENOENT —— 那不是错误，只是"还没好"。
        """
        deadline = time.monotonic() + self.args.wait_device
        attempt = 0

        while not self.stop_event.is_set():
            attempt += 1

            if self.args.device:
                cands = [self.args.device]
            else:
                cands, _ = discover_rpmsg_devices()

            for path in cands:
                if not os.path.exists(path):
                    continue
                try:
                    self.fd = os.open(path, os.O_RDWR)
                except OSError as exc:
                    self.log("打开 {} 失败: {}".format(path, exc))
                    continue
                self.device_path = path
                self.log("已打开 {}".format(path), force=True)
                return True

            if time.monotonic() >= deadline:
                break

            remaining = max(0.0, deadline - time.monotonic())
            if attempt == 1 or (attempt % 5) == 0:
                self.log(
                    "等待 rpmsg 设备节点出现... 剩余 {:.0f}s".format(remaining),
                    force=True,
                )
            self.stop_event.wait(0.2)

        return False

    # ---------------------------------------------------------- 发送一帧
    def send_packet(self, command, text):
        packet = LegacyPacketCodec.encode(command, text)
        with self.write_lock:
            total = 0
            while total < len(packet):
                try:
                    written = os.write(self.fd, packet[total:])
                except InterruptedError:
                    continue
                except OSError as exc:
                    raise RuntimeError("写 {} 失败: {}".format(self.device_path, exc))
                if written <= 0:
                    raise RuntimeError("写 {} 返回 0 字节".format(self.device_path))
                total += written
        return total

    # ================================================================
    #  ★ 关键一步：主动"敲门"（对应飞腾 main() 里那句 WakeUp）
    # ================================================================
    def send_wakeup(self):
        """发一帧 DEVICE_CORE_CHECK，把 APU1 从等 SGI 的状态里踢出来。

        为什么用 DEVICE_CORE_CHECK(0x0003) 而不是 HEARTBEAT：
          与飞腾 rpmsg-demo-single.c:468-470 完全一致 —— 它就是发
          command=DEVICE_CORE_CHECK、data="WakeUp"。
          业务层收到会当成文本命令走 apu_app_handle_command()，
          "WakeUp" 不是任何已知命令 → 回一条 ACK，无副作用。
        """
        text = self.args.wake_text
        try:
            n = self.send_packet(DEVICE_CORE_CHECK, text)
        except RuntimeError as exc:
            self.log("WakeUp 发送失败: {}".format(exc), force=True)
            return False
        self.log(
            "★ 已发出 WakeUp（DEVICE_CORE_CHECK \"{}\"，{} 字节）"
            "—— 这一帧就是 APU1 收到的第一个 SGI 的来源".format(text, n),
            force=True,
        )
        return True

    # ------------------------------------------------- 心跳（飞腾同款时序）
    def heartbeat_loop(self):
        """每 period 秒发一次 PING:<seq>。

        与飞腾 heartbeat_thread 的差别（有意为之，写清楚免得队友以为漏了）：
          飞腾是 send 之后**阻塞等 80ms** 看有没有 PONG，再 sleep 到下一个周期。
          本脚本是**单线程收发**（poll 里同时收消息），所以改成：
            只发不阻塞等 → 收到 PONG 时按序号记账 → 周期性报告"几次没回"。
          效果等价（都能判失联），但不会因为等 PONG 而卡住命令下行。
        """
        # 启动宽限：给裸机初始化留时间，宽限期内不报故障（飞腾也是 5s）
        if self.args.grace > 0:
            self.log("启动宽限 {:.1f}s（宽限期内不报失联）".format(self.args.grace))
            if self.stop_event.wait(self.args.grace):
                return

        seq = 1
        next_send = time.monotonic()
        last_report = time.monotonic()

        while not self.stop_event.is_set():
            now = time.monotonic()

            # ---- 失联判定（等价飞腾的"80ms 没 PONG，连续 5 次"）----
            if self.pending_seq and (now - self.pending_since) > self.args.reply_timeout:
                # 这一拍确实没在超时窗口内被 PONG 确认
                if self.consecutive_misses < self.args.max_misses:
                    self.consecutive_misses += 1
                if self.consecutive_misses == 1:
                    self.log(
                        "未收到裸机回信（PING:{}），开始重试（最多 {} 次）".format(
                            self.pending_seq, self.args.max_misses
                        ),
                        force=True,
                    )
                if (self.consecutive_misses >= self.args.max_misses
                        and not self.fault_reported):
                    self.log("★★ 裸机连续无响应（{} 次未回 PONG）★★".format(
                        self.args.max_misses), force=True)
                    self.fault_reported = True
                self.pending_seq = 0        # 这一拍结案，等下一拍

            try:
                self.send_packet(
                    DEVICE_CORE_HEARTBEAT,
                    "{}{}".format(HEARTBEAT_PING_PREFIX, seq),
                )
                self.tx_heartbeat += 1
                self.last_sent_seq = seq
                self.pending_seq = seq
                self.pending_since = time.monotonic()
            except RuntimeError as exc:
                self.log("心跳发送失败: {}".format(exc), force=True)
                self.stop_event.set()
                return

            seq += 1

            # 每 5 秒报一次心跳状态（对着 APU1 串口的 PONG 一起看）
            now = time.monotonic()
            if self.args.verbose or (now - last_report) >= 5.0:
                last_report = now
                self.report_status()

            next_send += self.args.period
            delay = next_send - time.monotonic()
            if delay > 0:
                if self.stop_event.wait(delay):
                    return
            else:
                # 发得太慢（周期被超过），重新对齐，避免"追赶式"连发
                next_send = time.monotonic()

    def report_status(self):
        if self.last_pong_at is None:
            pong_txt = "还没收到过 PONG"
        else:
            pong_txt = "最近 PONG {:.1f}s 前".format(time.monotonic() - self.last_pong_at)
        self.log(
            "tx_ping={} rx_total={} pong_ok={} | {}".format(
                self.tx_heartbeat, self.rx_total, self.pong_ok, pong_txt
            ),
            force=True,
        )

    # ------------------------------------------------------ 收消息主循环
    def receive_loop(self):
        """★ 本进程是 /dev/rpmsgN 的**唯一所有者**。

        为什么必须独占（我一开始把这条写错了，这里把真正的规矩钉死）：
          内核 rpmsg_char 的规定是 —— **一个端点设备只能被一个进程 open**。
          `rpmsg_eptdev_open()` 里有：
              if (eptdev->ept) return -EBUSY;
          而且 `open()` 这一下本身就会去 create_ept，所以第二次 open 必然失败。
          就算绕过 open 检查（老内核没这个保护）也不安全：
          所有进程的 read 都从**同一个** skb_queue 里 skb_dequeue，
          两个 reader 会各抢走一半消息 —— PONG 可能被桥吃掉、ACK 可能被本进程吃掉。

          （顺带澄清：并发 **write** 其实是安全的，内核用 eptdev->ept_lock
            串行化了 rpmsg_sendto。所以"多写"不是问题，"多开"才是。）

          飞腾是怎么做的：它是**同一个进程内**三个线程共用一个 rpmsg_fd
          （rpmsg-demo-single.c:472-475），靠 rpmsg_write_lock 串行化发送。
          ⇒ 一个进程、一个 fd、一把锁 = 单所有者。
          我们这边心跳和 Web 桥是两个独立进程，所以本进程当所有者，
          桥通过 Unix socket（--cmd-unix-base）把指令交给我们代发。

        本循环同时监听三路输入：
          · rpmsg 设备  → 读回帧（PONG / ACK / SENSOR / FAULT）
          · UDP 命令口  → 可选的本地直投（--no-udp 可关，桥在用时关掉更干净）
          · Unix socket → Web 桥转发过来的指令
        """
        import select

        while not self.stop_event.is_set():
            watch = [self.fd]
            if self.command_socket is not None:
                watch.append(self.command_socket)
            if self.unix_socket is not None:
                watch.append(self.unix_socket)
            if self.unix_conn is not None:
                watch.append(self.unix_conn)

            try:
                ready, _, _ = select.select(watch, [], [], 0.1)
            except (OSError, ValueError):
                break

            if not ready:
                continue

            for sock in ready:
                if sock is self.fd:
                    self._on_device_readable()
                elif sock is self.unix_socket:
                    self._accept_unix_conn()
                elif sock is self.unix_conn:
                    self._on_unix_readable()
                elif sock is self.command_socket:
                    self._on_udp_command()

        self.stop_event.set()

    # --------------------------------------------------- rpmsg 设备可读
    def _on_device_readable(self):
        try:
            raw = os.read(self.fd, 4096)
        except BlockingIOError:
            return
        except OSError as exc:
            self.log("读 {} 失败: {}".format(self.device_path, exc), force=True)
            self.stop_event.set()
            return

        if not raw:
            return

        self.rx_total += 1
        self._handle_frame(raw)

    # --------------------------------------------------- UDP 命令下行
    def _on_udp_command(self):
        try:
            data, _ = self.command_socket.recvfrom(1024)
        except OSError:
            return
        self._forward_command(data.decode("utf-8", "replace").strip(), "UDP")

    # --------------------------------------------------- Unix socket 通道
    def _bind_cmd_socket(self):
        """建一个 Unix stream socket，让 Web 桥把指令转发进来。

        为什么要它：见 receive_loop 的说明 —— 桥不能自己 open /dev/rpmsgN，
        必须把指令交给本进程代发。协议很简单，一行一条：
            桥 → 守护 :  RFC 的文本指令 + "\\n"
            守护 → 桥 :  无需回应（真正的回帧由守护自己收并转成 UDP 状态）
        """
        import socket as _socket

        base = self.args.cmd_unix_base
        try:
            if os.path.exists(base):
                os.unlink(base)
        except OSError:
            pass

        try:
            self.unix_socket = _socket.socket(_socket.AF_UNIX, _socket.SOCK_STREAM)
            self.unix_socket.bind(base)
            self.unix_socket.listen(4)
            # 让桥那个用户也能连（板上两边都是 root，这里只是保险）
            try:
                os.chmod(base, 0o666)
            except OSError:
                pass
            self.log("Unix 指令通道已就绪: {}".format(base), force=True)
        except OSError as exc:
            self.unix_socket = None
            self.log(
                "Unix 指令通道创建失败({}) —— Web 桥将无法转发指令；"
                "若不需要桥，可加 --no-cmd-unix 消除本提示".format(exc),
                force=True,
            )

    def _accept_unix_conn(self):
        try:
            conn, _ = self.unix_socket.accept()
        except OSError:
            return
        # 同时只留一个桥，新的顶掉旧的（避免半死连接占着）
        if self.unix_conn is not None:
            try:
                self.unix_conn.close()
            except OSError:
                pass
        conn.setblocking(False)
        self.unix_conn = conn
        self.log("Web 桥已连接上 Unix 指令通道")

    def _on_unix_readable(self):
        try:
            data = self.unix_conn.recv(4096)
        except BlockingIOError:
            return
        except OSError:
            data = b""

        if not data:
            # 桥断开了
            try:
                self.unix_conn.close()
            except OSError:
                pass
            self.unix_conn = None
            self.log("Web 桥已断开")
            return

        for line in data.decode("utf-8", "replace").splitlines():
            self._forward_command(line.strip(), "桥")

    # --------------------------------------------------- 指令统一入口
    def _forward_command(self, text, source):
        if not text:
            return
        try:
            self.send_packet(DEVICE_CORE_CHECK, text)
            self.log("→ 指令已转发给 APU1（来自{}）: {!r}".format(source, text), force=True)
        except RuntimeError as exc:
            self.log("指令转发失败: {}".format(exc), force=True)

    def _send_status(self, payload):
        """把 APU1 的回帧转成 Web 契约后从 UDP 状态口发出去。

        ⚠️ 2026-09-25：**故意不回写到 Unix 指令通道**。
           Unix 通道是**单向**的（桥 → 守护，只传指令）。
           原因：桥那边把这条连接当"只写"用，不会去读；
           如果守护往这里 sendall，数据会在对端缓冲区堆积，
           堆满之后 sendall 把守护**阻塞住** —— 心跳就停了。
           状态本来就由本进程自己用 UDP 发出，桥/Web 从 status_port 收即可，
           不需要走 Unix 通道。
        """
        if self.status_socket is None or self.status_target is None:
            return
        encoded = (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8")
        try:
            self.status_socket.sendto(encoded, self.status_target)
        except OSError as exc:
            self.log("状态上报(UDP)失败: {}".format(exc))

    def _handle_frame(self, raw):
        try:
            command, text = LegacyPacketCodec.decode(raw)
        except PacketFormatError as exc:
            self.log("丢弃非法帧: {} ({!r})".format(exc, raw[:32]), force=True)
            return

        self.last_rx_text = text

        # ---- PONG：心跳回执（飞腾用 sscanf("PONG:%u%c")==1 严格校验）----
        if command == DEVICE_CORE_HEARTBEAT:
            m = _PONG_RE.match(text)
            if m:
                self.pong_ok += 1
                self.consecutive_misses = 0
                self.fault_reported = False
                self.last_pong_at = time.monotonic()
                # ★ 关键：收到 PONG 就清掉 pending，否则这一拍会在超时窗口后
                #   被误算成一次 miss（等于"回了也算没回"）。
                self.pending_seq = 0
                self.log("← PONG {}".format(m.group(1)))
            else:
                self.log("← 心跳类帧但格式不对: {!r}".format(text), force=True)
            return

        # ---- 其它：ACK / SENSOR / FAULT，转成 Web 契约后上报 ----
        self.log("← cmd=0x{:04X} {!r}".format(command, text), force=True)

        payload = status_from_packet(command, text)
        if payload is not None:
            self._send_status(payload)

    # ------------------------------------------------------------- 主流程
    def run(self):
        # ---- 飞腾 ① ②：先尝试建端点（失败不致命，多数情况节点已存在）----
        if not self.args.device and not self.args.no_create_ept:
            eps, ctrls = discover_rpmsg_devices()
            if not eps and ctrls:
                self.log("没有数据节点，但发现控制节点 {}，尝试建端点（飞腾做法）".format(ctrls))
                for ctrl in ctrls:
                    ok, why = create_rpmsg_endpoint(ctrl, self.args.service_name)
                    self.log("  在 {} 建端点: {} ({})".format(ctrl, "成功" if ok else "失败", why))
                    if ok:
                        time.sleep(0.3)
                        break

        # ---- 飞腾 ③：打开数据节点 ----
        if not self.open_device():
            sys.stderr.write(
                "[HB][FATAL] {:.0f} 秒内没有找到可用的 /dev/rpmsg* 节点。\n"
                "  这说明内核还没把 RPMsg 设备挂出来，常见原因：\n"
                "    1) remoteproc 没 start：\n"
                "         cat /sys/class/remoteproc/remoteproc0/state   # 应为 running\n"
                "    2) 固件没做 ns announce（看 APU1 串口有没有 "
                "'Successfully created rpmsg endpoint'）\n"
                "    3) 内核报错：dmesg | grep -i -E 'remoteproc|rpmsg|virtio'\n"
                "  排掉以上问题后重跑本脚本。\n".format(self.args.wait_device)
            )
            return 1

        # ---- 飞腾 ④：立刻敲第一脚 ----
        if not self.args.no_wakeup:
            self.send_wakeup()
            time.sleep(0.2)

        if self.args.once:
            # 只验证连通性：等 1.5s 看有没有回帧，然后退出
            import select
            self.log("--once：等待 1.5s 看是否有回帧...")
            deadline = time.monotonic() + 1.5
            while time.monotonic() < deadline:
                ready, _, _ = select.select([self.fd], [], [], 0.2)
                if ready:
                    raw = os.read(self.fd, 4096)
                    if raw:
                        self.rx_total += 1
                        self._handle_frame(raw)
                        break
            self.log(
                "结果：{}".format(
                    "收到回帧，SGI 通路 OK ★" if self.rx_total else
                    "没收到回帧（请对照 APU1 串口 [APU1][poll] 的 SGI rx 是否变成非 0）"
                ),
                force=True,
            )
            # --once 也要还设备，否则连着跑几次会一直占着节点
            if self.fd is not None:
                try:
                    os.close(self.fd)
                except OSError:
                    pass
                self.fd = None
            return 0

        # ---- 飞腾 ⑤：起心跳 + 单所有者收发线程 ----
        #   ★ 只有一个 receive_loop 线程碰设备（读），写入统一走 write_lock。
        #     多开设备会 -EBUSY，多读会抢消息 —— 详见 receive_loop 的说明。
        if not self.args.no_udp:
            self.status_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.status_target = (self.args.status_host, self.args.status_port)
            if not self.args.no_udp_command:
                self.command_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                self.command_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                self.command_socket.bind((self.args.command_bind, self.args.command_port))

        # Unix 指令通道：让 Web 桥把指令交给我们代发（它不能自己 open 设备）
        if not self.args.no_cmd_unix:
            self._bind_cmd_socket()

        threads = [threading.Thread(target=self.receive_loop, daemon=True)]
        threads.append(threading.Thread(target=self.heartbeat_loop, daemon=True))
        for t in threads:
            t.start()

        self.log(
            "开始发心跳：每 {:.0f}ms 一个 PING，宽限 {:.1f}s，"
            "等 PONG 超时 {:.0f}ms，连续 {} 次没回算失联（与飞腾一致）".format(
                self.args.period * 1000, self.args.grace,
                self.args.reply_timeout * 1000, self.args.max_misses,
            ),
            force=True,
        )

        try:
            while not self.stop_event.wait(0.5):
                pass
        finally:
            self.stop_event.set()
            if self.fd is not None:
                try:
                    os.close(self.fd)
                except OSError:
                    pass
            if self.unix_conn is not None:
                try:
                    self.unix_conn.close()
                except OSError:
                    pass
            if self.unix_socket is not None:
                try:
                    self.unix_socket.close()
                except OSError:
                    pass
                try:
                    os.unlink(self.args.cmd_unix_base)
                except OSError:
                    pass
            if self.status_socket:
                self.status_socket.close()
            if self.command_socket:
                self.command_socket.close()

        return 0


# ===========================================================================
# 四、命令行
# ===========================================================================

def build_parser():
    p = argparse.ArgumentParser(
        description="APU1 心跳守护：主动 WakeUp + 定时 PING（对照飞腾 rpmsg-demo-single.c）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    # 设备
    p.add_argument("--device", default=os.environ.get("EG400_RPMSG_DEVICE", ""),
                   help="rpmsg 数据节点；留空=自动扫描 /dev/rpmsg*")
    p.add_argument("--wait-device", type=float,
                   default=float(os.environ.get("EG400_HB_WAIT_DEVICE", "30")),
                   help="等设备节点出现的秒数")
    p.add_argument("--no-create-ept", action="store_true",
                   help="不用 ioctl 建端点（只在节点已存在时有用）")
    p.add_argument("--service-name",
                   default=os.environ.get("EG400_RPMSG_SERVICE", "rpmsg-openamp-demo-channel"),
                   help="建端点用的服务名（与固件 RPMSG_SERVICE_NAME 一致）")

    # 敲门
    p.add_argument("--no-wakeup", action="store_true",
                   help="不主动发 WakeUp（只发心跳）")
    p.add_argument("--wake-text", default="WakeUp",
                   help="WakeUp 帧的文本（与飞腾一致）")
    p.add_argument("--once", action="store_true",
                   help="发一帧 WakeUp 后等 1.5s 就退出（快速验证连通性）")

    # 心跳时序（默认值 = 飞腾常量）
    p.add_argument("--period", type=float,
                   default=float(os.environ.get("EG400_HB_PERIOD", "0.5")),
                   help="心跳周期（秒）= 飞腾 HEARTBEAT_PERIOD_MS 500")
    p.add_argument("--grace", type=float,
                   default=float(os.environ.get("EG400_HB_GRACE", "5.0")),
                   help="启动宽限（秒）= 飞腾 HEARTBEAT_STARTUP_GRACE_SECONDS 5")
    p.add_argument("--reply-timeout", type=float,
                   default=float(os.environ.get("EG400_HB_REPLY_TIMEOUT", "0.08")),
                   help="等 PONG 超时（秒）= 飞腾 HEARTBEAT_REPLY_TIMEOUT_MS 80")
    p.add_argument("--max-misses", type=int,
                   default=int(os.environ.get("EG400_HB_MAX_MISSES", "5")),
                   help="连续几次没回算失联 = 飞腾 HEARTBEAT_MAX_MISSES 5")

    # UDP
    p.add_argument("--no-udp", action="store_true",
                   help="完全不开 UDP（状态口和命令口都关）")
    p.add_argument("--no-udp-command", action="store_true",
                   help="只关 UDP 命令口（留状态口）。跑 Web 桥时建议开这个，避免两路指令混在一起")
    p.add_argument("--command-bind", default=os.environ.get("EG400_BRIDGE_COMMAND_BIND", "127.0.0.1"))
    p.add_argument("--command-port", type=int, default=int(os.environ.get("EG400_CMD_PORT", "9001")))
    p.add_argument("--status-host", default=os.environ.get("EG400_STATUS_HOST", "127.0.0.1"))
    p.add_argument("--status-port", type=int, default=int(os.environ.get("EG400_UDP_PORT", "9998")))

    # Unix 指令通道（Web 桥经此转发，因为本进程独占 /dev/rpmsgN）
    p.add_argument("--cmd-unix-base", default=os.environ.get("EG400_CMD_UNIX", "/tmp/eg400_apu1_cmd"),
                   help="Unix socket 路径：Web 桥把指令写到这里转给我们代发")
    p.add_argument("--no-cmd-unix", action="store_true",
                   help="不建 Unix 指令通道（不需要 Web 桥时用）")

    p.add_argument("-v", "--verbose", action="store_true", help="打印每一帧收发")
    return p


def main():
    args = build_parser().parse_args()
    daemon = HeartbeatDaemon(args)

    def stop_handler(_signum, _frame):
        daemon.log("收到停止信号，正在退出...", force=True)
        daemon.stop_event.set()

    signal.signal(signal.SIGTERM, stop_handler)
    signal.signal(signal.SIGINT, stop_handler)

    rc = daemon.run()
    daemon.report_status()
    return rc


if __name__ == "__main__":
    sys.exit(main())
