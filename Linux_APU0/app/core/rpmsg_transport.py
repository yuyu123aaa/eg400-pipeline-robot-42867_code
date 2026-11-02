"""Transport and legacy packet codec for the Linux-side control bridge.

The Web application keeps its existing UDP contract. This module owns only
the byte transport below the bridge, so a mock endpoint can be used before
the EG400 kernel exposes a real RPMsg device.

The packet codec matches the legacy project's data_packet layout. It is a
compatibility choice for testing, not proof that EG400's final APU0 protocol
must use this layout.
"""

import os
import select
import socket
import struct
from abc import ABC, abstractmethod


# The legacy C endpoint stores a trailing NUL in data[256] and rejects
# length >= 256 while parsing a received frame.  Keep the Python bridge on
# the same wire-level limit instead of accepting a frame the peer rejects.
MAX_DATA_LENGTH = 256
MAX_PAYLOAD_LENGTH = MAX_DATA_LENGTH - 1
_HEADER = struct.Struct("<IH")

DEVICE_CORE_CHECK = 0x0003
DEVICE_CORE_SENSOR = 0x0004
DEVICE_CORE_HEARTBEAT = 0x0005


class TransportError(RuntimeError):
    """Raised when the selected byte transport cannot be used."""


class PacketFormatError(ValueError):
    """Raised when a packet is not valid for the compatibility codec."""


class LegacyPacketCodec:
    """Encode and decode the old packed command/status packet."""

    @staticmethod
    def encode(command, text):
        payload = str(text).encode("utf-8")
        if len(payload) > MAX_PAYLOAD_LENGTH:
            raise PacketFormatError(
                "payload is larger than {} bytes".format(MAX_PAYLOAD_LENGTH)
            )
        return _HEADER.pack(int(command), len(payload)) + payload

    @staticmethod
    def decode(packet):
        if len(packet) < _HEADER.size:
            raise PacketFormatError("packet is shorter than its header")

        command, length = _HEADER.unpack(packet[:_HEADER.size])
        if length > MAX_PAYLOAD_LENGTH:
            raise PacketFormatError("packet length exceeds the legacy limit")
        end = _HEADER.size + length
        if len(packet) < end:
            raise PacketFormatError("packet payload is incomplete")
        return command, packet[_HEADER.size:end].decode("utf-8", "replace")


class ByteTransport(ABC):
    """Minimal interface used by the bridge."""

    @abstractmethod
    def send(self, packet):
        raise NotImplementedError

    @abstractmethod
    def receive(self, timeout):
        raise NotImplementedError

    @abstractmethod
    def close(self):
        raise NotImplementedError


class MockUdpTransport(ByteTransport):
    """UDP-backed test transport; it is not a hardware IPC implementation."""

    def __init__(self, endpoint_host, endpoint_port, listen_host, listen_port):
        self._target = (endpoint_host, int(endpoint_port))
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._socket.bind((listen_host, int(listen_port)))

    def send(self, packet):
        return self._socket.sendto(packet, self._target)

    def receive(self, timeout):
        ready, _, _ = select.select([self._socket], [], [], timeout)
        if not ready:
            return None
        packet, _ = self._socket.recvfrom(4096)
        return packet

    def close(self):
        self._socket.close()


class PosixCharDeviceTransport(ByteTransport):
    """Read/write an already-created Linux IPC character device.

    This class deliberately does not create RPMsg endpoints or issue an
    ioctl. The exact endpoint setup is platform-specific and is still
    unconfirmed on EG400. The configured device must already be provided by
    the kernel/firmware integration.
    """

    def __init__(self, device_path):
        if not device_path:
            raise TransportError(
                "EG400_RPMSG_DEVICE is required for the rpmsg backend"
            )
        try:
            # NOTE: the rpmsg char driver's poll() only reports readability;
            # it never signals EPOLLOUT. The fd must stay blocking so that
            # os.write() behaves like the C demo; reads still gate on select().
            self._fd = os.open(device_path, os.O_RDWR)
        except OSError as exc:
            raise TransportError(
                "cannot open configured IPC device {}: {}".format(
                    device_path, exc
                )
            ) from exc

    def send(self, packet):
        total = 0
        while total < len(packet):
            try:
                written = os.write(self._fd, packet[total:])
            except InterruptedError:
                continue
            if written <= 0:
                raise TransportError("IPC device returned an empty write")
            total += written
        return total

    def receive(self, timeout):
        readable, _, _ = select.select([self._fd], [], [], timeout)
        if not readable:
            return None
        try:
            return os.read(self._fd, 4096)
        except BlockingIOError:
            return None

    def close(self):
        if getattr(self, "_fd", None) is not None:
            os.close(self._fd)
            self._fd = None


def status_from_packet(command, text):
    """Convert legacy sensor/control text into the Web JSON contract."""

    if command == DEVICE_CORE_CHECK:
        return {"control_ack": text}

    if command == DEVICE_CORE_HEARTBEAT:
        return None

    if command != DEVICE_CORE_SENSOR:
        return None

    if text.startswith("R:"):
        try:
            roll, pitch, yaw = (
                text[2:]
                .replace(",P:", ",")
                .replace(",Y:", ",")
                .split(",")
            )
            return {
                "roll": float(roll),
                "pitch": float(pitch),
                "yaw": float(yaw),
                "error": 0,
            }
        except (TypeError, ValueError):
            return None

    if text.startswith("SONAR:"):
        try:
            # 2026-09-27 修复：固件实际格式为 SONAR:<front>,<left>,<right>,V:<battery>
            # （4 个字段，末尾带电压）。原实现按恰好 3 字段解包，遇到 V: 后缀
            # 抛 ValueError → 返回 None → 守护永不向 Web 转发 SONAR（IMU 正常、
            # 超声恒 0 的根因）。改为取前 3 个字段，电压段透传给 Web。
            parts = text[6:].split(",")
            payload = {
                "sonar_front": int(parts[0]),
                "sonar_left": int(parts[1]),
                "sonar_right": int(parts[2]),
            }
            if len(parts) >= 4 and parts[3].startswith("V:"):
                try:
                    payload["battery_voltage_raw"] = int(parts[3][2:])
                except ValueError:
                    pass
            return payload
        except (TypeError, ValueError, IndexError):
            return None

    # ---- ENV: 三路环境传感器合并帧（APU1 固件 v2，2026-10-15 格式变更后）----
    # v2 格式（去掉了 *V 标志位，固件直接丢弃无效数据，用 0 当哨兵）：
    #   ENV:DEPTH:<mm>,TURB:<v>,GAS:<co>,<h2s>,<o2_raw>,<ch4>
    # 语义：
    #   DEPTH=0 → 本次无有效深度（0mm 物理不可能）
    #   GAS 四值全 0 → 整组没读到（O2=0 %VOL 物理不可能）
    #   TURB=0 → 有歧义（清水真值=0 / 无数据），按队友建议就显示 0
    #   O2 固件直传 0.1%VOL，这里 ÷10 换算成 %VOL（209→20.9）
    if text.startswith("ENV:"):
        try:
            tokens = text[4:].split(",")
            kv = {}
            gas_raw = None

            i = 0
            while i < len(tokens):
                tok = tokens[i]
                key, sep, value = tok.partition(":")
                if key == "GAS":
                    gas_raw = [value] + tokens[i + 1:i + 4]
                    i += 4
                    continue
                if sep:
                    kv[key] = value
                i += 1

            if gas_raw is None or len(gas_raw) != 4:
                return None

            def _env_int(name, default=0):
                try:
                    return int(kv.get(name, default))
                except (TypeError, ValueError):
                    return default

            def _env_num(name, default=0):
                """兼容浮点：XM3318B 手册倍率 100，固件可能直发浮点
                （如 TURB:5.36），int() 会失败回退 0 —— 2026-10-18
                浊度恒 0 的根因。整值保持整型外观，小数的保留 1 位。"""
                try:
                    f = float(kv.get(name, default))
                except (TypeError, ValueError):
                    return default
                return int(f) if f == int(f) else round(f, 1)

            try:
                # 同样兼容浮点（GAS 通道固件若改发浮点不至于整组归零）
                co, h2s, o2_raw, ch4 = (int(float(v)) for v in gas_raw)
            except (TypeError, ValueError):
                co = h2s = o2_raw = ch4 = 0

            return {
                "depth_mm":   _env_int("DEPTH"),
                "turbidity":  _env_num("TURB"),
                "gas_co":     co,
                "gas_h2s":    h2s,
                "gas_o2":     round(o2_raw / 10.0, 1),
                "gas_ch4":    ch4,
            }
        except (TypeError, ValueError, IndexError):
            return None

    if text.startswith("FAULT:SONAR:"):
        try:
            return {
                "sonar_fault": True,
                "sonar_fault_mask": int(text[12:]),
            }
        except ValueError:
            return None

    if text == "RECOVER:SONAR":
        return {"sonar_fault": False, "sonar_fault_mask": 0}

    if text.startswith("FAULT:NAV:"):
        return {"nav_fault": text}

    return None
