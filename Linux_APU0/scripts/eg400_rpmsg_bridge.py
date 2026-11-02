#!/usr/bin/env python3
"""Linux-side bridge between the existing Web UDP contract and IPC.

The default backend is the explicit mock backend. Use --backend rpmsg only
after the EG400 kernel has exposed and documented a real endpoint device.
"""

import argparse
import json
import os
import signal
import socket
import sys
import threading
import time


PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP_DIR = os.path.join(PROJECT_ROOT, "app")
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from core.rpmsg_transport import (  # noqa: E402
    DEVICE_CORE_CHECK,
    DEVICE_CORE_HEARTBEAT,
    LegacyPacketCodec,
    MockUdpTransport,
    PacketFormatError,
    PosixCharDeviceTransport,
    status_from_packet,
)


class DaemonRelayTransport:
    """把指令经 Unix socket 交给 apu1_heartbeat_daemon.py 代发。

    ===========================================================================
     为什么需要这个类（不是多此一举）
    ===========================================================================
      内核 rpmsg_char 的硬规定：**一个端点设备只能被一个进程 open**。
      drivers/rpmsg/rpmsg_char.c 的 rpmsg_eptdev_open() 里写着：

          if (eptdev->ept)
                  return -EBUSY;

      而且 open() 这一下本身就会去 create_ept，所以第二个进程必然失败。
      就算老内核没这个保护也不安全：所有进程的 read 都从**同一个**
      skb_queue 里 skb_dequeue，两个 reader 会各抢走一半消息
      （PONG 被这边吃掉、ACK 被那边吃掉）。

      所以本桥**不自己 open 设备**，改成把指令写进
      apu1_heartbeat_daemon.py 建的 Unix socket，由它代发。
      心跳、发指令、收回帧全部由那一个进程负责 —— 也就是
      飞腾"单进程多线程共用一个 rpmsg_fd"在我们这里的等价形式。

      （顺带说明：并发 write 本身是安全的，内核用 eptdev->ept_lock
        串行化了 rpmsg_sendto；问题从来不是"多写"，而是"多开/多读"。）

      APU1 的回帧由守护进程收下来，转成 Web 契约后从 UDP 状态口发出，
      所以本桥仍然照常从 status_port 收状态，Web 应用不用改。
    """

    def __init__(self, base_path):
        self._base = base_path
        self._sock = None
        self._lock = threading.Lock()
        self._connect()

    def _connect(self):
        if not os.path.exists(self._base):
            raise RuntimeError(
                "心跳守护的 Unix 指令通道不存在：{}\n"
                "  ⇒ 说明它还没起来。请先执行（顺序不能反）：\n"
                "       sh scripts/apu1_amp_up.sh\n"
                "     再确认 socket 出现：ls -l {}\n"
                "  为什么要这样：内核规定一个 rpmsg 端点设备只能被一个进程打开，"
                "所以心跳守护独占 /dev/rpmsgN，"
                "本桥只能把指令经它转发。".format(self._base, self._base)
            )
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(2.0)
        try:
            sock.connect(self._base)
        except OSError as exc:
            sock.close()
            raise RuntimeError(
                "连不上心跳守护的指令通道 {}：{}\n"
                "  ⇒ 确认 apu1_heartbeat_daemon.py 正在运行："
                "ps | grep apu1_heartbeat_daemon".format(self._base, exc)
            ) from exc
        sock.settimeout(None)
        self._sock = sock

    def _sendall_retry(self, data):
        with self._lock:
            try:
                if self._sock is None:
                    self._connect()
                self._sock.sendall(data)
            except OSError as exc:
                # 守护可能重启了：重连一次再试
                self._sock = None
                try:
                    self._connect()
                    self._sock.sendall(data)
                except OSError:
                    raise RuntimeError(
                        "无法把指令交给心跳守护（{}）；请确认 "
                        "apu1_heartbeat_daemon.py 正在运行".format(self._base)
                    ) from exc
        return len(data)

    def send(self, packet):
        """发原始字节。仅在确实需要整帧透传时用（本桥的 daemon 路径不用它）。"""
        return self._sendall_retry(bytes(packet))

    def send_text(self, text):
        """★ daemon 路径实际走这里：发一行文本，守护负责编码成 RPMsg 包。

        协议（与 apu1_heartbeat_daemon.py 的 _on_unix_readable 对应）：
            · 一行 = 一条指令，以 '\\n' 结尾
            · 守护 strip() 后按 DEVICE_CORE_CHECK 包发出去
            · 回帧不由这里读 —— 守护收下后从 UDP 状态口发出
        """
        line = (text.strip() + "\n").encode("utf-8")
        return self._sendall_retry(line)

    def receive(self, timeout):
        """回帧不走这里 —— 由守护进程收下后从 UDP 状态口发出。

        返回 None 让桥的接收循环空转即可。
        """
        time.sleep(min(timeout, 0.05))
        return None

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None


class Bridge:
    def __init__(self, args):
        self.args = args
        self.stop_event = threading.Event()
        self.write_lock = threading.Lock()
        self.last_heartbeat_reply = time.monotonic()
        self.transport = self._create_transport()
        self.command_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.command_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.command_socket.bind((args.command_bind, args.command_port))
        self.status_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.status_target = (args.status_host, args.status_port)

    def _create_transport(self):
        if self.args.backend == "mock":
            return MockUdpTransport(
                self.args.mock_endpoint_host,
                self.args.mock_endpoint_port,
                self.args.mock_bridge_host,
                self.args.mock_bridge_port,
            )
        if self.args.backend == "daemon":
            # ★ 推荐：经心跳守护转发，由它作为 /dev/rpmsgN 的唯一所有者
            return DaemonRelayTransport(self.args.daemon_cmd_unix)
        return PosixCharDeviceTransport(self.args.rpmsg_device)

    def send_packet(self, packet):
        with self.write_lock:
            self.transport.send(packet)

    def send_command(self, text):
        """下层是 daemon 时交纯文本（由守护编码），否则自己编码。

        ⚠️ 这两条路**不能混**：
          · daemon 后端：Unix 通道传的是**文本行**，守护收到后自己编码成
            DEVICE_CORE_CHECK 包再发 RPMsg。如果这里先编码成二进制，
            守护会把那串二进制**当成文本**再包一层 → APU1 收到乱码。
          · rpmsg/mock 后端：这里是设备唯一写入方，必须自己编码。
        """
        if self.args.backend == "daemon":
            with self.write_lock:
                self.transport.send_text(text)
        else:
            packet = LegacyPacketCodec.encode(DEVICE_CORE_CHECK, text)
            self.send_packet(packet)
        print("[BRIDGE] command -> IPC: {}".format(text), flush=True)

    def send_heartbeat(self, sequence):
        packet = LegacyPacketCodec.encode(
            DEVICE_CORE_HEARTBEAT, "PING:{}".format(sequence)
        )
        self.send_packet(packet)

    def publish_status(self, payload):
        encoded = (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8")
        self.status_socket.sendto(encoded, self.status_target)

    def command_loop(self):
        self.command_socket.settimeout(0.5)
        while not self.stop_event.is_set():
            try:
                data, _ = self.command_socket.recvfrom(1024)
            except socket.timeout:
                continue
            except OSError:
                break
            command = data.decode("utf-8", "replace").strip()
            if not command:
                continue
            try:
                self.send_command(command)
            except (OSError, RuntimeError) as exc:
                print("[BRIDGE] command transport error: {}".format(exc), flush=True)

    def receive_loop(self):
        while not self.stop_event.is_set():
            try:
                packet = self.transport.receive(0.25)
            except (OSError, RuntimeError) as exc:
                if not self.stop_event.is_set():
                    print(
                        "[BRIDGE] receive transport error: {}".format(exc),
                        flush=True,
                    )
                self.stop_event.set()
                break
            if not packet:
                continue
            try:
                command, text = LegacyPacketCodec.decode(packet)
            except PacketFormatError as exc:
                print("[BRIDGE] discarded invalid IPC packet: {}".format(exc), flush=True)
                continue

            if command == DEVICE_CORE_HEARTBEAT and text.startswith("PONG:"):
                self.last_heartbeat_reply = time.monotonic()
                print("[BRIDGE] heartbeat <- IPC: {}".format(text), flush=True)
                continue

            payload = status_from_packet(command, text)
            if payload is not None:
                self.publish_status(payload)

    def heartbeat_loop(self):
        sequence = 1
        while not self.stop_event.wait(self.args.heartbeat_interval):
            try:
                self.send_heartbeat(sequence)
            except (OSError, RuntimeError) as exc:
                print("[BRIDGE] heartbeat transport error: {}".format(exc), flush=True)
            if (
                time.monotonic() - self.last_heartbeat_reply
                > self.args.heartbeat_timeout
            ):
                print("[BRIDGE] heartbeat reply timeout", flush=True)
            sequence += 1

    def run(self):
        print(
            "[BRIDGE] backend={} command=127.0.0.1:{} status={}:{}".format(
                self.args.backend,
                self.args.command_port,
                self.args.status_host,
                self.args.status_port,
            ),
            flush=True,
        )
        threads = [
            threading.Thread(target=self.command_loop, daemon=True),
            threading.Thread(target=self.receive_loop, daemon=True),
        ]
        # ★ daemon 后端的模式下，心跳由 apu1_heartbeat_daemon.py 负责
        #   （它才是设备所有者）。桥再发一份就是重复心跳，没必要。
        if self.args.backend != "daemon":
            threads.append(threading.Thread(target=self.heartbeat_loop, daemon=True))
        else:
            print(
                "[BRIDGE] backend=daemon：心跳与设备收帧由心跳守护负责，"
                "本桥只转发指令、并照常从 UDP 状态口收状态",
                flush=True,
            )
        for thread in threads:
            thread.start()
        try:
            while not self.stop_event.wait(0.5):
                pass
        finally:
            self.stop_event.set()
            self.command_socket.close()
            self.status_socket.close()
            self.transport.close()


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--backend",
        choices=("mock", "rpmsg", "daemon"),
        default=os.environ.get("EG400_BRIDGE_BACKEND", "mock"),
        help="mock=本机环回；rpmsg=自己打开设备（需独占）；"
             "daemon=经心跳守护转发（推荐）",
    )
    parser.add_argument(
        "--daemon-cmd-unix",
        default=os.environ.get("EG400_CMD_UNIX", "/tmp/eg400_apu1_cmd"),
        help="daemon 后端用的 Unix socket 路径（要与心跳守护的 --cmd-unix-base 一致）",
    )
    parser.add_argument(
        "--command-bind",
        default=os.environ.get("EG400_BRIDGE_COMMAND_BIND", "127.0.0.1"),
    )
    parser.add_argument(
        "--command-port",
        type=int,
        default=int(os.environ.get("EG400_CMD_PORT", "9001")),
    )
    parser.add_argument(
        "--status-host",
        default=os.environ.get("EG400_STATUS_HOST", "127.0.0.1"),
    )
    parser.add_argument(
        "--status-port",
        type=int,
        default=int(os.environ.get("EG400_UDP_PORT", "9998")),
    )
    parser.add_argument(
        "--mock-endpoint-host",
        default=os.environ.get("EG400_MOCK_ENDPOINT_HOST", "127.0.0.1"),
    )
    parser.add_argument(
        "--mock-endpoint-port",
        type=int,
        default=int(os.environ.get("EG400_MOCK_ENDPOINT_PORT", "19001")),
    )
    parser.add_argument(
        "--mock-bridge-host",
        default=os.environ.get("EG400_MOCK_BRIDGE_HOST", "127.0.0.1"),
    )
    parser.add_argument(
        "--mock-bridge-port",
        type=int,
        default=int(os.environ.get("EG400_MOCK_BRIDGE_PORT", "19002")),
    )
    parser.add_argument(
        "--rpmsg-device",
        default=os.environ.get("EG400_RPMSG_DEVICE", ""),
    )
    parser.add_argument("--heartbeat-interval", type=float, default=0.5)
    parser.add_argument("--heartbeat-timeout", type=float, default=1.0)
    return parser


def main():
    args = build_parser().parse_args()
    bridge = Bridge(args)

    def stop_handler(_signum, _frame):
        bridge.stop_event.set()

    signal.signal(signal.SIGTERM, stop_handler)
    signal.signal(signal.SIGINT, stop_handler)
    bridge.run()


if __name__ == "__main__":
    main()
