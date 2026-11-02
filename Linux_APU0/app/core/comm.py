"""
UDP 通信模块
============
- 向 C 底层程序发送控制指令
- 接收 C 程序发回的 IMU 姿态 + 声纳数据并更新 system_status
"""

import json
import socket
import threading

import config
from config import CMD_PORT, UDP_PORT, system_status


SONAR_FAULT_CHANNELS = (
    (0x01, "front"),
    (0x02, "left"),
    (0x04, "right"),
)


def apply_control_feedback(payload):
    """根据裸机异步回执更新网页使用的权威电机状态。"""
    ack = payload.get("control_ack")
    fault = payload.get("nav_fault")

    if ack:
        system_status["motor_control_message"] = ack
        # 2026-10-05: 下潜通道回执（独立于水平电机状态机，交接单 §4.3）。
        # "ACK: DIVE ON" → on；"ACK: DIVE STOP" → stopped；
        # "ERR: DIVE ..." 不落入任何分支 → 保持原状态、不污染 motor_command_state。
        # 注意未知指令回显（如 [CMD:DIVE:UP]）含冒号不含空格，不会误命中。
        if "DIVE ON" in ack:
            system_status["dive_state"] = "on"
        elif "DIVE STOP" in ack:
            system_status["dive_state"] = "stopped"
        if "CENTER CONTROL STARTED" in ack:
            system_status["motor_command_state"] = "running"
            system_status["is_moving"] = True
            system_status["nav_fault"] = ""
        elif ack.startswith("ACK: REJECTED"):
            system_status["motor_command_state"] = "rejected"
            system_status["is_moving"] = False
        elif (
            "电机已关闭" in ack
            or "系统已停止" in ack
            or "MOTOR STOPPED" in ack
        ):
            system_status["motor_command_state"] = "stopped"
            system_status["is_moving"] = False
            # 停车串 = 六路联动停车，固件侧下潜一并清零
            system_status["dive_state"] = "stopped"

    if fault:
        system_status["nav_fault"] = fault
        system_status["motor_control_message"] = fault
        system_status["motor_command_state"] = "rejected"
        system_status["is_moving"] = False
        # 导航安全故障触发六路联动停车，下潜一并清零
        system_status["dive_state"] = "stopped"


def apply_sonar_fault_feedback(payload):
    """同步裸机判定的超声波损坏状态，不在恢复时自动启动电机。"""
    if "sonar_fault" not in payload:
        return

    active = payload["sonar_fault"] is True
    was_active = system_status["sonar_fault"] is True
    mask = int(payload.get("sonar_fault_mask", 0)) if active else 0
    channels = [
        name
        for bit, name in SONAR_FAULT_CHANNELS
        if mask & bit
    ]

    system_status["sonar_fault"] = active
    system_status["sonar_fault_channels"] = channels
    system_status["sonar_fault_message"] = (
        "超声波传感器异常：推进器已停止、照明已关闭、遥控已锁定"
        if active
        else ""
    )

    if active:
        config.MANUAL_MODE = False
        system_status["is_moving"] = False
        system_status["manual_mode"] = False
        system_status["motor_command_state"] = "stopped"
        # 超声损坏触发六路联动停车，下潜一并清零
        system_status["dive_state"] = "stopped"
        system_status["light1_on"] = False
        system_status["light2_on"] = False
        system_status["motor_control_message"] = system_status[
            "sonar_fault_message"
        ]
        if not was_active:
            send_str_to_linux("CMD:STOP")
            send_str_to_linux("CMD:LED1OFF")
            send_str_to_linux("CMD:LED2OFF")
    else:
        system_status["motor_control_message"] = (
            "超声波已恢复，推进器和照明保持停止"
        )


def send_str_to_linux(*args):
    """通过 UDP 向通信桥发送空格分隔的控制指令。"""
    msg = " ".join(map(str, args))
    if not config.command_transport_enabled():
        print(f"[SIMULATION] 已拦截硬件指令: {msg}", flush=True)
        return False

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.sendto(msg.encode('utf-8'), (config.CMD_HOST, CMD_PORT))
            target = f"{config.CMD_HOST}:{CMD_PORT}"
            print(f"[Py->bridge] {target} 发送指令: {msg}", flush=True)
        return True
    except Exception:
        print(f"[通信失败] 无法发送指令: {msg}", flush=True)
        return False


def _udp_listener_loop():
    """UDP 监听线程的主循环"""
    udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp_socket.bind(('0.0.0.0', UDP_PORT))
    print(f"📡 监听服务已启动，等待通信桥数据 (端口 {UDP_PORT})...", flush=True)

    while True:
        try:
            data, _ = udp_socket.recvfrom(1024)
            imu_data = json.loads(data.decode('utf-8'))
            apply_control_feedback(imu_data)
            apply_sonar_fault_feedback(imu_data)

            if "map_event" in imu_data:
                continue

            system_status["roll"] = imu_data.get("roll", system_status["roll"])
            system_status["pitch"] = imu_data.get("pitch", system_status["pitch"])
            system_status["yaw"] = imu_data.get("yaw", system_status["yaw"])
            system_status["error"] = imu_data.get("error", system_status["error"])
            system_status["sonar_front"] = imu_data.get("sonar_front", system_status["sonar_front"])
            system_status["sonar_left"] = imu_data.get("sonar_left", system_status["sonar_left"])
            system_status["sonar_right"] = imu_data.get("sonar_right", system_status["sonar_right"])

            # ---- ENV: 三路环境传感器（APU1 固件 v2，2026-10-15 格式变更后）----
            # v2 去掉 *V 标志位，固件直接丢弃无效数据用 0 哨兵。
            # 2026-10-17 采样保持：传感器 1Hz 采样，两帧有效数据之间的单帧
            # 无效（哨兵 0）不再覆盖上次有效值，避免前端闪成「——/掉线」。
            #   DEPTH==0/65535 → 本帧无回波，保留上一次深度
            #   O2==0          → 气体组本帧无数据，保留上一次整组
            #   TURB           → 0 可能是清水真值，照常写入
            if "depth_mm" in imu_data and 0 < imu_data["depth_mm"] < 65535:
                system_status["depth_mm"] = imu_data["depth_mm"]
            if "turbidity" in imu_data:
                system_status["turbidity"] = imu_data["turbidity"]
            if "gas_co" in imu_data and imu_data.get("gas_o2", 0) > 0:
                system_status["gas_co"] = imu_data["gas_co"]
                system_status["gas_h2s"] = imu_data["gas_h2s"]
                system_status["gas_o2"] = imu_data["gas_o2"]
                system_status["gas_ch4"] = imu_data["gas_ch4"]
        except Exception:
            pass


def start_udp_listener():
    """启动 UDP 数据监听线程（守护线程）"""
    t = threading.Thread(target=_udp_listener_loop, daemon=True)
    t.start()
    return t
