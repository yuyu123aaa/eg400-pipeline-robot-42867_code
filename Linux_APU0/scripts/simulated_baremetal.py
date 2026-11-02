#!/usr/bin/env python3
"""模拟 Core0 裸机端，验证 Web 与通信桥之间的旧协议。

这个程序只占用本机 UDP 端口，不访问 GPIO、PWM、I2C、串口、RPMsg
或任何真实执行器。它接收 Web 发往 CMD_PORT 的文本命令，并把兼容
旧项目的 ACK/IMU/声纳 JSON 发回 UDP_PORT。
"""

import argparse
import json
import math
import socket
import time


class SimulatedBareMetal:
    def __init__(self, args):
        self.args = args
        self.system_running = False
        self.motor_moving = False
        self.light1_on = False
        self.light2_on = False
        self.manual_mode = False
        self.yaw = 35.0
        self.heading_zero = self.yaw
        self.sequence = 0

        self.command_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.command_socket.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_REUSEADDR,
            1,
        )
        self.command_socket.bind((args.command_host, args.command_port))
        self.status_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.status_target = (args.status_host, args.status_port)

    def close(self):
        self.command_socket.close()
        self.status_socket.close()

    def send_status(self, payload):
        self.status_socket.sendto(
            (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"),
            self.status_target,
        )

    def send_ack(self, message):
        print(f"[SIM-BAREMETAL] ACK: {message}", flush=True)
        self.send_status({"control_ack": message})

    def telemetry(self, now):
        if self.motor_moving:
            self.yaw = self.heading_zero + 8.0 * math.sin(now * 0.7)
        else:
            self.yaw = self.heading_zero + 2.0 * math.sin(now * 0.25)

        payload = {
            "roll": round(1.5 * math.sin(now * 0.9), 2),
            "pitch": round(1.0 * math.cos(now * 0.65), 2),
            "yaw": round(self.yaw, 2),
            "error": 0,
            "sonar_front": 800 + int(20 * math.sin(now)),
            "sonar_left": 720 + int(15 * math.cos(now * 0.8)),
            "sonar_right": 760 + int(18 * math.sin(now * 0.6)),
        }
        if self.args.sonar_fault_mask:
            payload["sonar_fault"] = True
            payload["sonar_fault_mask"] = self.args.sonar_fault_mask
        else:
            payload["sonar_fault"] = False
            payload["sonar_fault_mask"] = 0
        return payload

    def handle_command(self, raw):
        command = raw.decode("utf-8", errors="replace").strip()
        if not command:
            return
        print(f"[SIM-BAREMETAL] CMD: {command}", flush=True)

        if command == "PING" or command.startswith("PING:"):
            suffix = command[5:] if command.startswith("PING:") else ""
            self.send_status({"control_ack": f"PONG:{suffix}"})
        elif command == "CMD:START":
            self.system_running = True
            self.send_ack("ACK: 系统已启动")
        elif command == "CMD:STOP":
            self.system_running = False
            self.motor_moving = False
            self.manual_mode = False
            self.send_ack("ACK: 系统已停止")
        elif command == "CMD:MOTOR_START":
            self.motor_moving = True
            self.send_ack("ACK: 10HZ CENTER CONTROL STARTED, NAV SAFETY BYPASSED")
        elif command == "CMD:MOTOR_STOP":
            self.motor_moving = False
            self.send_ack("ACK: 电机已关闭")
        elif command == "CMD:MANUAL_START":
            self.manual_mode = True
            self.send_ack("ACK: MANUAL MODE STARTED")
        elif command == "CMD:CALIBRATE":
            self.heading_zero = self.yaw
            self.send_ack("ACK: HEADING TARGET RESET")
        elif command == "CMD:LED1ON":
            self.light1_on = True
            self.send_ack("ACK: LED1 已开启")
        elif command == "CMD:LED1OFF":
            self.light1_on = False
            self.send_ack("ACK: LED1 已关闭")
        elif command == "CMD:LED2ON":
            self.light2_on = True
            self.send_ack("ACK: LED2 已开启")
        elif command == "CMD:LED2OFF":
            self.light2_on = False
            self.send_ack("ACK: LED2 已关闭")
        elif command.startswith("CTRL:"):
            self.send_ack("ACK: CONTROL PARAMS UPDATED")
        elif command.startswith("SPEED:"):
            self.send_ack("ACK: SPEED UPDATED")
        else:
            self.send_ack(f"ACK: 收到未知指令 [{command}]")

    def run(self):
        print(
            "[SIM-BAREMETAL] listening "
            f"{self.args.command_host}:{self.args.command_port}; "
            f"status -> {self.args.status_host}:{self.args.status_port}",
            flush=True,
        )
        next_status = time.monotonic()
        deadline = (
            time.monotonic() + self.args.duration
            if self.args.duration > 0
            else None
        )
        try:
            while deadline is None or time.monotonic() < deadline:
                now = time.monotonic()
                wait = max(0.0, min(self.args.telemetry_interval, next_status - now))
                self.command_socket.settimeout(max(wait, 0.001))
                try:
                    data, _ = self.command_socket.recvfrom(1024)
                except (socket.timeout, BlockingIOError):
                    data = None
                if data is not None:
                    self.handle_command(data)

                now = time.monotonic()
                if now >= next_status:
                    self.send_status(self.telemetry(now))
                    self.sequence += 1
                    next_status = now + self.args.telemetry_interval
        finally:
            self.close()


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--command-host", default="127.0.0.1")
    parser.add_argument("--command-port", type=int, default=9001)
    parser.add_argument("--status-host", default="127.0.0.1")
    parser.add_argument("--status-port", type=int, default=9998)
    parser.add_argument("--telemetry-interval", type=float, default=0.25)
    parser.add_argument(
        "--sonar-fault-mask",
        type=lambda value: int(value, 0),
        default=0,
        help="持续模拟声纳故障位掩码，例如 0x01",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=0.0,
        help="运行秒数；0 表示持续运行",
    )
    return parser


def main():
    args = build_parser().parse_args()
    if args.telemetry_interval <= 0:
        raise SystemExit("--telemetry-interval 必须大于 0")
    if args.duration < 0:
        raise SystemExit("--duration 不能小于 0")
    SimulatedBareMetal(args).run()


if __name__ == "__main__":
    main()
