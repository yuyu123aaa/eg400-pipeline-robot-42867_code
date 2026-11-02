"""
手柄桥接脚本（Windows 电脑端）
================================
读取标准微软 Xbox 手柄状态，通过 UDP 发送到板卡。

用法:
    python controller_bridge.py --host 192.168.1.100        # 正常运行
    python controller_bridge.py --host 192.168.1.100 --debug # 调试：打印所有轴/按钮索引

操控映射:
    RT 扳机(右)  → 前进油门
    LT 扳机(左)  → 后退油门   （同时按下取净差值）
    左摇杆左右   → 转向
    A 键         → 启动/关闭系统（同网页启动/关闭）
    B 键         → 急停（最高优先级）
    X 键         → 进入/退出手动模式
    Y 键         → 缺陷自动记录 开/关
    LB / RB      → 左灯 / 右灯开关
    左摇杆按下   → 重新标定 IMU 航向（同网页"重新标定"按钮）
    右摇杆按下   → 拍照
    菜单键(Menu) → 录像 开/关
    方向键上     → 前进 / 开始巡检（同网页"向前"）
    方向键下     → 停止电机（同网页"停止"）
    方向键左右   → 左转 / 右转（巡航航向 ±90°，自动模式有效）

依赖:
    pip install "pygame>=2.0"
"""

import argparse
import json
import socket
import sys
import time
import uuid

try:
    import pygame
except ImportError:
    print("缺少 pygame，请先安装:  pip install pygame")
    sys.exit(1)


# ============================================================
# 标准微软 Xbox 手柄映射（pygame / SDL2，Windows XInput）
# ============================================================
AXIS_LX = 0      # 左摇杆 X：-1 左 ~ +1 右  → 转向
AXIS_LY = 1      # 左摇杆 Y：-1 上 ~ +1 下  → 未使用
AXIS_RX = 2      # 右摇杆 X（预留）
AXIS_RY = 3      # 右摇杆 Y（预留）
AXIS_LT = 4      # 左扳机：静止 -1 ~ 满 +1  → 后退油门
AXIS_RT = 5      # 右扳机：静止 -1 ~ 满 +1  → 前进油门

BTN_A = 0
BTN_B = 1
BTN_X = 2
BTN_Y = 3
BTN_LB = 4
BTN_RB = 5
BTN_MENU = 7       # 菜单键 → 录像开/关
BTN_LEFT_STICK = 8  # 左摇杆按下 → 重新标定 IMU 航向（2026-10-29 按用户要求恢复）
BTN_RIGHT_STICK = 9  # R3 → 拍照

# 名称 → 按钮索引（发往板卡时用语义名，接收端更清晰）
BUTTON_MAP = {
    "a": BTN_A, "b": BTN_B, "x": BTN_X, "y": BTN_Y,
    "lb": BTN_LB, "rb": BTN_RB,
    "menu": BTN_MENU, "r3": BTN_RIGHT_STICK,
    "left_stick": BTN_LEFT_STICK,
}
CONTROL_NAMES = list(BUTTON_MAP) + [
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
]

# ------------------------------------------------------------
# 控制器映射：默认 Xbox 布局；可用 --map <json> 覆盖（Switch Pro 等）
# 映射文件格式：
#   {"axes": {"lx":0,"ly":1,"rx":2,"ry":3,"lt":4,"rt":5},
#    "buttons": {"a":0,"b":1,...},
#    "digital_triggers": {"lt":6,"rt":7}}     # 无模拟扳机的手柄用它
# ------------------------------------------------------------
AXIS_MAP = {"lx": AXIS_LX, "ly": AXIS_LY, "rx": AXIS_RX,
            "ry": AXIS_RY, "lt": AXIS_LT, "rt": AXIS_RT}
DIGITAL_TRIGGERS = {}

DEADZONE = 0.08      # 摇杆/扳机死区
SEND_HZ = 50         # 发送频率（Hz）


def normalize_trigger(value):
    """扳机原始值 [-1, +1] → [0, 1]（静止 0，满油门 1）"""
    return (value + 1.0) / 2.0


def map_steering(value):
    """左摇杆 X → 转向百分比 [-100, +100]（带死区线性映射）"""
    if abs(value) < DEADZONE:
        return 0
    sign = 1 if value > 0 else -1
    mapped = (abs(value) - DEADZONE) / (1.0 - DEADZONE) * 100
    return int(sign * mapped)


def map_throttle(rt_raw, lt_raw):
    """
    扳机 → 油门百分比 [-100, +100]
    RT 前进(+)，LT 后退(-)，同时按下取净差值。
    """
    rt = normalize_trigger(rt_raw)
    lt = normalize_trigger(lt_raw)

    if rt < DEADZONE:
        rt = 0.0
    if lt < DEADZONE:
        lt = 0.0

    throttle = rt - lt          # rt 前进，lt 后退，自动取差值
    return int(throttle * 100)


def get_newly_pressed_buttons(buttons, last_buttons):
    """只返回本帧新按下的按钮；未按、释放和持续按住均不输出。"""
    return [
        name
        for name in CONTROL_NAMES
        if buttons.get(name, False) and not last_buttons.get(name, False)
    ]


def supports_disconnect_events(pygame_module, joystick):
    """运行时断开检测依赖 pygame 2 的设备实例 ID 与移除事件。"""
    return (
        hasattr(pygame_module, "JOYDEVICEREMOVED")
        and callable(getattr(joystick, "get_instance_id", None))
    )


def build_disconnect_packet(seq, session_id):
    """构造手柄断开通知：零运动指令，但不伪装成 B 键全系统急停。"""
    return {
        "seq": seq,
        "session_id": session_id,
        "connected": False,
        "throttle": 0,
        "turn": 0,
        "buttons": {name: False for name in CONTROL_NAMES},
        "timestamp": time.time(),
    }


def send_disconnect_notice(sock, target, seq, session_id):
    """重复发送断开通知，降低单个 UDP 包丢失造成的风险。"""
    data = json.dumps(build_disconnect_packet(seq, session_id)).encode("utf-8")
    for _ in range(3):
        sock.sendto(data, target)


def run_debug(joystick):
    """调试模式：实时打印所有轴和按钮，帮你核对映射。"""
    print("=== 调试模式 === （Ctrl+C 退出）")
    print("拨动每个摇杆/扳机、按每个按钮/方向键，记下对应的索引号\n")
    clock = pygame.time.Clock()
    try:
        while True:
            pygame.event.pump()
            axes = [round(joystick.get_axis(i), 2) for i in range(joystick.get_numaxes())]
            btns = [i for i in range(joystick.get_numbuttons()) if joystick.get_button(i)]
            hats = [joystick.get_hat(i) for i in range(joystick.get_numhats())]
            print(f"\r轴: {axes}  | 按下的按钮: {btns}  | 方向键: {hats}        ", end="")
            clock.tick(10)
    except KeyboardInterrupt:
        print("\n退出调试。")


def main():
    parser = argparse.ArgumentParser(description="手柄桥接（Windows → 板卡）")
    parser.add_argument("--host", default="192.168.137.100", help="板卡 IP 地址")
    parser.add_argument("--port", type=int, default=9002, help="手柄 UDP 端口")
    parser.add_argument("--debug", action="store_true", help="调试模式：打印轴/按钮索引")
    parser.add_argument("--learn", action="store_true", help="学习模式：实时显示索引，供生成映射文件")
    parser.add_argument("--map", default="", help="控制器映射 JSON（默认 Xbox 布局）")
    args = parser.parse_args()

    # ---- 初始化手柄 ----
    pygame.init()
    pygame.joystick.init()

    if pygame.joystick.get_count() == 0:
        print("未检测到手柄！请确认手柄已连接并被 Windows 识别。")
        sys.exit(1)

    joystick = pygame.joystick.Joystick(0)
    joystick.init()
    if not supports_disconnect_events(pygame, joystick):
        print("当前 pygame 不支持手柄断开检测，请安装 pygame 2 或更高版本。")
        pygame.quit()
        return

    joystick_instance_id = joystick.get_instance_id()
    session_id = uuid.uuid4().hex
    print(f"手柄已连接: {joystick.get_name()}")
    print(f"  轴数: {joystick.get_numaxes()}, 按钮数: {joystick.get_numbuttons()}")

    # ---- 加载自定义映射 ----
    global AXIS_MAP, BUTTON_MAP, DIGITAL_TRIGGERS
    if args.map:
        try:
            with open(args.map, "r", encoding="utf-8") as fh:
                cfg = json.load(fh)
            AXIS_MAP.update(cfg.get("axes", {}))
            for name, idx in cfg.get("buttons", {}).items():
                BUTTON_MAP[name] = int(idx)
            DIGITAL_TRIGGERS = {k: int(v) for k, v in cfg.get("digital_triggers", {}).items()}
            print(f"  已加载映射: {args.map}")
            print(f"    轴: {AXIS_MAP}")
            print(f"    数字扳机: {DIGITAL_TRIGGERS or '无（用模拟轴）'}")
        except Exception as exc:
            print(f"  映射文件加载失败({exc})，改用默认 Xbox 布局")

    if args.debug or args.learn:
        run_debug(joystick)
        return

    print(f"  目标: {args.host}:{args.port}")
    print(
        "  X=进入/退出手动 | 方向键上=拍照 | 左=网页向前 | 右=网页停止 | "
        "B=急停 | L3=重新标定 | RT=前进 | LT=后退 | 左摇杆=转向"
    )
    print(f"  发送频率: {SEND_HZ} Hz\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.host, args.port)
    clock = pygame.time.Clock()

    seq = 0
    last_buttons = {name: False for name in CONTROL_NAMES}  # 上一帧按钮状态
    last_throttle, last_turn = None, None                 # 上一帧油门/转向，用于去重打印

    # 按钮中文描述（用于打印）
    BTN_LABEL = {
        "a": "A(启动/关闭)", "b": "B(急停)", "x": "X(手动开关)",
        "y": "Y(缺陷记录)", "lb": "LB(左灯)", "rb": "RB(右灯)",
        "menu": "Menu(录像)", "r3": "右摇杆按下(拍照)",
        "left_stick": "左摇杆按下(重新标定)",
        "dpad_up": "方向键上(向前)",
        "dpad_down": "方向键下(停止)",
    }

    disconnected = False
    try:
        while True:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    return
                if (
                    event.type == pygame.JOYDEVICEREMOVED
                    and getattr(event, "instance_id", None) == joystick_instance_id
                ):
                    disconnected = True
                    print("[GAMEPAD] 手柄已断开，退出手柄控制模式并请求停止推进器。")
                    send_disconnect_notice(sock, target, seq, session_id)
                    return
            pygame.event.pump()

            # ---- 读取摇杆与扳机 ----
            def axis(name, default=0.0):
                idx = AXIS_MAP.get(name)
                if idx is None or idx >= joystick.get_numaxes():
                    return default
                return joystick.get_axis(idx)

            lx = axis("lx"); ly = axis("ly"); rx = axis("rx"); ry = axis("ry")
            lt = axis("lt"); rt = axis("rt")

            # ---- 读取按钮（语义名 → 是否按住）----
            buttons = {}
            for name, idx in BUTTON_MAP.items():
                buttons[name] = bool(joystick.get_button(idx)) if idx < joystick.get_numbuttons() else False
            hat_x, hat_y = joystick.get_hat(0) if joystick.get_numhats() > 0 else (0, 0)
            buttons["dpad_up"] = hat_y > 0
            buttons["dpad_down"] = hat_y < 0
            # 十字键左右 = 左转 / 右转（航向 ±90°，板端沿检测，回中复位）
            buttons["yaw_left"] = hat_x < 0
            buttons["yaw_right"] = hat_x > 0

            # ---- 按钮按下沿检测 & 打印（用上一帧状态对比）----
            for name in get_newly_pressed_buttons(buttons, last_buttons):
                print(f"[BTN] {BTN_LABEL[name]} 按下")
            last_buttons = dict(buttons)

            if DIGITAL_TRIGGERS:
                pressed = {}
                for tag, idx in DIGITAL_TRIGGERS.items():
                    pressed[tag] = bool(joystick.get_button(idx)) if idx < joystick.get_numbuttons() else False
                throttle = (100 if pressed.get("rt") else 0) - (100 if pressed.get("lt") else 0)
            else:
                throttle = map_throttle(rt, lt)
            turn = map_steering(lx)

            # ---- 油门/转向变化时打印 ----
            if (throttle, turn) != (last_throttle, last_turn):
                if throttle != 0 or turn != 0:
                    print(f"[AXIS] 油门={throttle:+4d}  转向={turn:+4d}")
                elif last_throttle != 0 or last_turn != 0:
                    print(f"[AXIS] 停止(0, 0)")
                last_throttle, last_turn = throttle, turn

            # ---- 组装并发送数据包 ----
            packet = {
                "seq": seq,
                "session_id": session_id,
                "connected": True,
                "lx": round(lx, 3), "ly": round(ly, 3),
                "rx": round(rx, 3), "ry": round(ry, 3),
                "rt": round(rt, 3), "lt": round(lt, 3),
                "throttle": throttle,     # -100 ~ +100
                "turn": turn,             # -100 ~ +100
                "buttons": buttons,       # 各键当前是否按住（接收端做沿检测）
                "timestamp": time.time(),
            }
            seq += 1

            sock.sendto(json.dumps(packet).encode("utf-8"), target)
            clock.tick(SEND_HZ)

    except KeyboardInterrupt:
        print("\n停止中…")
    except pygame.error as exc:
        disconnected = True
        print(f"[GAMEPAD] 手柄读取失败（常见原因：误按模式键导致手柄重新配对）: {exc}")
        print("[GAMEPAD] 已请求停止推进器。重新插拔手柄后再运行本脚本。")
        send_disconnect_notice(sock, target, seq, session_id)
    finally:
        if not disconnected:
            # 正常退出时补发全系统急停；硬件断开已发送专用断开通知。
            stop_pkt = {
                "seq": seq, "session_id": session_id,
                "connected": True, "throttle": 0, "turn": 0,
                "buttons": {"b": True}, "timestamp": time.time(),
            }
            try:
                sock.sendto(json.dumps(stop_pkt).encode("utf-8"), target)
            except Exception:
                pass
        sock.close()
        pygame.quit()
        print("已退出手柄桥接。")


if __name__ == "__main__":
    main()
