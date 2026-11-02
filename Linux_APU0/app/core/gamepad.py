"""
手柄数据接收与模式仲裁（飞腾派 Linux 端）
==========================================
监听 Windows 端 controller_bridge.py 发来的手柄 UDP 包，
按模式状态机仲裁后，通过 send_str_to_linux 转发指令给 C 程序，
C 程序再经 RPMsg 透传给裸机（C 程序无需改动）。

模式状态机:
    IDLE ──Web自动巡检──▶ AUTO
      ▲                    │
      │ X / B急停           │ X
      └──── MANUAL ◀───────┘
            │ B急停 / 超时
            ▼
           IDLE

要点:
    - B 急停：最高优先级，无视模式立即停车
    - A 启动/关闭：与网页启动/关闭按钮一致
    - X：进入/退出手动遥控模式
    - 方向键上：拍照
    - 方向键左/右：与网页“向前”/“停止”按钮一致
    - 左摇杆按下（L3）：与网页“重新标定”按钮一致
    - 仅手动模式响应摇杆油门 → SPEED:<throttle>,<turn>
    - 拍照/识别/灯光按钮不依赖手动模式，但需要系统处于启动状态
    - 功能按钮（A/X/Y/LB/RB/L3/方向键）在本端做按下沿检测，避免连发
    - 超时保护：手动模式下超过 GAMEPAD_TIMEOUT_S 未收到包 → 自动急停
"""

import json
import socket
import threading
import time

import config
from core.comm import send_str_to_linux
from core.calibration import recalibrate_heading
from core.motor_control import start_motor, stop_motor
from core.vision import set_yolo_enabled


# 模块内部状态
_last_packet_time = 0.0          # 最近一次收到有效包的时间
_last_buttons = None             # 上一帧按钮状态（用于按下沿检测）
_last_speed = (None, None)       # 上一帧 (throttle, turn)，用于去重打印
_listener_running = False
_LEGACY_SESSION_ID = "__legacy_controller_bridge__"
_active_session_id = None        # 当前 Windows 手柄桥接进程的唯一会话 ID
_last_session_seq = -1           # 当前会话最后接受的数据包序号
_closed_session_ids = set()      # 已断开会话；拒绝其延迟或乱序 UDP 包
_modern_session_seen = False     # 新协议出现后，永久拒绝可绕过序号校验的旧格式包
_state_lock = threading.RLock()  # 串行化数据包处理与超时停机，避免 STOP/SPEED 竞态


def _rising_edge(buttons, name):
    """检测按钮 name 是否在本帧产生「按下沿」(上一帧未按、本帧按下)。"""
    now = bool(buttons.get(name, False))
    was = bool(_last_buttons.get(name, False))
    return now and not was


def _enter_manual():
    config.MANUAL_MODE = True
    config.system_status["manual_mode"] = True
    config.system_status["is_moving"] = False
    config.ENABLE_YOLO = False
    config.system_status["yolo_on"] = False
    send_str_to_linux("CMD:MANUAL_START")
    print("[GAMEPAD] 进入手动模式 >> CMD:MANUAL_START")


def _exit_manual(reason=""):
    config.MANUAL_MODE = False
    config.system_status["manual_mode"] = False
    config.system_status["is_moving"] = False
    send_str_to_linux("CMD:STOP")
    print(f"[GAMEPAD] 退出手动模式({reason}) >> CMD:STOP" if reason
          else "[GAMEPAD] 退出手动模式 >> CMD:STOP")


def _stop_system(reason=""):
    config.MANUAL_MODE = False
    config.system_status["manual_mode"] = False
    config.system_status["is_moving"] = False
    config.system_status["is_running"] = False
    config.system_status["temperature"] = None
    send_str_to_linux("CMD:STOP")
    print(f"[GAMEPAD] 关闭系统({reason}) >> CMD:STOP" if reason
          else "[GAMEPAD] 关闭系统 >> CMD:STOP")


def _toggle_system_running():
    if config.system_status["is_running"]:
        _stop_system("A")
    else:
        config.system_status["is_running"] = True
        send_str_to_linux("CMD:START")
        print("[GAMEPAD] 启动系统 >> CMD:START")


def _handle_packet(data):
    with _state_lock:
        _handle_packet_locked(data)


def _handle_packet_locked(data):
    global _last_packet_time, _last_buttons
    global _active_session_id, _last_session_seq, _modern_session_seen

    try:
        pkt = json.loads(data.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return

    session_id = pkt.get("session_id")
    if session_id is None:
        session_id = _LEGACY_SESSION_ID
        if _modern_session_seen or session_id in _closed_session_ids:
            return
        if _active_session_id is None:
            _active_session_id = session_id
            _last_session_seq = -1
            _last_buttons = None
        elif _active_session_id != session_id:
            return
    else:
        seq = pkt.get("seq")
        if not isinstance(session_id, str) or not session_id:
            return
        if not isinstance(seq, int) or isinstance(seq, bool) or seq < 0:
            return
        _modern_session_seen = True
        if session_id in _closed_session_ids:
            return

        if _active_session_id != session_id:
            if _active_session_id is not None:
                _closed_session_ids.add(_active_session_id)
                if config.MANUAL_MODE:
                    _exit_manual("手柄会话切换")
                else:
                    config.system_status["is_moving"] = False
                    send_str_to_linux("CMD:STOP")
            _active_session_id = session_id
            _last_session_seq = -1
            _last_buttons = None

        if seq <= _last_session_seq:
            return
        _last_session_seq = seq

    _last_packet_time = time.time()
    buttons = pkt.get("buttons", {})

    # ---- 手柄物理断开：立即退出手动模式并停车，但不关闭整个系统 ----
    if pkt.get("connected", True) is False:
        _closed_session_ids.add(session_id)
        _active_session_id = None
        _last_session_seq = -1
        config.system_status["gamepad_online"] = False
        if config.MANUAL_MODE:
            _exit_manual("手柄断开")
        else:
            config.system_status["is_moving"] = False
            send_str_to_linux("CMD:STOP")
        _last_buttons = None
        return

    config.system_status["gamepad_online"] = True

    # 超声波损坏报警由裸机锁定执行机构；遥控端只同步停机状态，
    # 不再发出启动、速度或灯光命令，直到裸机上报恢复。
    if config.system_status.get("sonar_fault", False):
        config.MANUAL_MODE = False
        config.system_status["manual_mode"] = False
        config.system_status["is_moving"] = False
        _last_buttons = dict(buttons)
        return

    # ---- B 急停：最高优先级，无视一切 ----
    if buttons.get("b", False):
        _stop_system("B急停")
        print("[GAMEPAD] 手柄急停! >> CMD:STOP")
        _last_buttons = dict(buttons)
        return

    if _last_buttons is None:
        _last_buttons = dict(buttons)
        return

    # ---- A：启动/关闭系统（同网页启动/关闭）----
    if _rising_edge(buttons, "a"):
        _toggle_system_running()
        _last_buttons = dict(buttons)
        return

    # ---- X：进入/退出手动模式（按下沿）----
    if _rising_edge(buttons, "x"):
        if config.MANUAL_MODE:
            _exit_manual("X")
            _last_buttons = dict(buttons)
            return
        _enter_manual()

    # ---- 功能按钮（按下沿，避免连发）：系统启动后即可使用 ----
    if config.system_status["is_running"]:
        if _rising_edge(buttons, "left_stick"):
            recalibrate_heading()
            print("[GAMEPAD] 左摇杆按下：重新标定 >> CMD:CALIBRATE")

        if _rising_edge(buttons, "dpad_up"):
            start_motor()
            print("[GAMEPAD] 方向键上：向前 >> CMD:MOTOR_START")
            _last_buttons = dict(buttons)
            return

        if _rising_edge(buttons, "dpad_down"):
            stop_motor()
            print("[GAMEPAD] 方向键下：停止 >> CMD:MOTOR_STOP")
            _last_buttons = dict(buttons)
            return

        # ---- 左转 / 右转：调整巡航目标航向 ±90°（与网页 yaw_left / yaw_right 等价）----
        if _rising_edge(buttons, "yaw_left") or _rising_edge(buttons, "yaw_right"):
            if not config.MANUAL_MODE:
                going_left = _rising_edge(buttons, "yaw_left")
                prev = config.system_status.get("heading_offset_deg", 0.0)
                new_off = ((prev + (90.0 if going_left else -90.0)) + 180.0) % 360.0 - 180.0
                config.system_status["heading_offset_deg"] = new_off
                cmd = "CMD:YAW:{}".format("LEFT" if going_left else "RIGHT")
                send_str_to_linux(cmd)
                print("[GAMEPAD] {}：航向偏置 {:+.0f}° >> {}".format(
                    "左转" if going_left else "右转", new_off, cmd))

        if _rising_edge(buttons, "y"):
            # 2026-10-01: Y 键从"本机 YOLO 开关"（已废弃）改为"缺陷自动记录开/关"
            from core.defect_recorder import set_recording
            enabled = set_recording(not config.DEFECT_RECORDING)
            print(f"[GAMEPAD] 缺陷记录: {'ON' if enabled else 'OFF'}")

        if _rising_edge(buttons, "r3"):
            config.take_photo_flag = True
            print("[GAMEPAD] 右摇杆按下：拍照")

        if _rising_edge(buttons, "menu"):
            config.is_recording = not config.is_recording
            print(f"[GAMEPAD] 录像: {'ON' if config.is_recording else 'OFF'}")

        if _rising_edge(buttons, "lb"):
            on = not config.system_status["light1_on"]
            config.system_status["light1_on"] = on
            cmd = "CMD:LED1ON" if on else "CMD:LED1OFF"
            send_str_to_linux(cmd)
            print(f"[GAMEPAD] 左灯{'开' if on else '关'} >> {cmd}")

        if _rising_edge(buttons, "rb"):
            on = not config.system_status["light2_on"]
            config.system_status["light2_on"] = on
            cmd = "CMD:LED2ON" if on else "CMD:LED2OFF"
            send_str_to_linux(cmd)
            print(f"[GAMEPAD] 右灯{'开' if on else '关'} >> {cmd}")

    # ---- 非手动模式：不响应摇杆油门 ----
    if not config.MANUAL_MODE:
        _last_buttons = dict(buttons)
        return

    # ---- 手动模式已开启但系统未启动：不响应油门/功能键 ----
    if not config.system_status["is_running"]:
        config.system_status["is_moving"] = False
        _last_buttons = dict(buttons)
        return

    # ---- 手动模式：转发速度指令 ----
    global _last_speed
    throttle = int(pkt.get("throttle", 0))
    turn = int(pkt.get("turn", 0))
    config.system_status["is_moving"] = (throttle != 0 or turn != 0)
    cmd = f"SPEED:{throttle},{turn}"
    send_str_to_linux(cmd)
    if (throttle, turn) != _last_speed:
        print(f"[GAMEPAD] >> {cmd}")
        _last_speed = (throttle, turn)

    _last_buttons = dict(buttons)


def _check_timeout(now=None):
    """检查一次通讯超时；与数据包处理共用锁，保证 STOP 不会被 SPEED 越过。"""
    global _active_session_id, _last_session_seq, _last_buttons

    with _state_lock:
        if _last_packet_time <= 0:
            return

        current_time = time.time() if now is None else now
        if (current_time - _last_packet_time) <= config.GAMEPAD_TIMEOUT_S:
            return

        config.system_status["gamepad_online"] = False
        if _active_session_id is not None:
            _closed_session_ids.add(_active_session_id)
            _active_session_id = None
            _last_session_seq = -1
        _last_buttons = None

        if config.MANUAL_MODE:
            _exit_manual("通讯超时")


def _timeout_checker():
    """手动模式下，超过超时阈值未收到手柄包 → 自动急停。"""
    while _listener_running:
        time.sleep(0.1)
        _check_timeout()


def _udp_loop():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.5)
    try:
        sock.bind(("0.0.0.0", config.GAMEPAD_PORT))
    except OSError as e:
        print(f"[GAMEPAD] 手柄监听端口 {config.GAMEPAD_PORT} 绑定失败: {e}")
        return

    print(f"[GAMEPAD] 手柄监听已启动: 0.0.0.0:{config.GAMEPAD_PORT}")
    while _listener_running:
        try:
            data, _ = sock.recvfrom(2048)
            _handle_packet(data)
        except socket.timeout:
            continue
        except Exception as e:
            print(f"手柄数据处理异常: {e}")
    sock.close()


def start_gamepad_listener():
    """启动手柄监听 + 超时检测两个守护线程。"""
    global _listener_running
    _listener_running = True
    threading.Thread(target=_udp_loop, daemon=True).start()
    threading.Thread(target=_timeout_checker, daemon=True).start()
