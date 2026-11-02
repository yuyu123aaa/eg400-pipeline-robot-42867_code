"""
全局配置与共享状态
===================
所有模块通过 `from config import ...` 引用这些变量。
对于 dict/list 类型的共享状态，直接修改其内容即可跨模块生效，
不要对整体变量做重新赋值（那会创建本地副本）。
"""

import os
import platform

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(BASE_DIR)


def _env_bool(name, default):
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _env_int(name, default):
    value = os.environ.get(name)
    if value is None:
        return default
    try:
        parsed = int(value)
    except ValueError as exc:
        raise ValueError(f"环境变量 {name} 必须是整数") from exc
    if not 1 <= parsed <= 65535:
        raise ValueError(f"环境变量 {name} 必须在 1～65535 范围内")
    return parsed


# 默认只启动网页层。LOOPBACK_MODE 用于在没有真实外设时验证 Web 与
# UDP 通信契约；它不会打开真实设备，也不会允许执行器输出。
WEB_ONLY = _env_bool("EG400_WEB_ONLY", True)
LOOPBACK_MODE = _env_bool("EG400_LOOPBACK_MODE", False)
SIMULATION_MODE = _env_bool("EG400_SIMULATION_MODE", True)
HARDWARE_CONTROL_ENABLED = _env_bool(
    "EG400_HARDWARE_CONTROL_ENABLED",
    False,
)
WEB_HOST = os.environ.get("EG400_WEB_HOST", "0.0.0.0")
WEB_PORT = _env_int("EG400_WEB_PORT", 5000)


def runtime_mode():
    """返回当前运行模式，供 API 和日志明确展示。"""
    if WEB_ONLY:
        return "web-only"
    if LOOPBACK_MODE:
        return "loopback"
    if SIMULATION_MODE:
        return "simulation"
    return "hardware"


def hardware_control_enabled():
    """只有显式允许且不处于网页/模拟模式时才允许执行器输出。"""
    return bool(
        HARDWARE_CONTROL_ENABLED
        and not SIMULATION_MODE
        and not WEB_ONLY
        and not LOOPBACK_MODE
    )


def command_transport_enabled():
    """返回是否允许向通信桥发送命令。

    回环模式允许 UDP 命令进入模拟裸机，但仍禁止真实硬件输出。
    """
    return bool((LOOPBACK_MODE and not WEB_ONLY) or hardware_control_enabled())

# ============================================================
# 平台相关路径
# ============================================================
if platform.system() == 'Windows':
    STATIC_FOLDER = 'static'
    SAVE_DIR = os.environ.get(
        "EG400_SAVE_DIR",
        os.path.join(PROJECT_DIR, 'data'),
    )
    MODEL_PATH = os.environ.get(
        "EG400_MODEL_PATH",
        os.path.join(PROJECT_DIR, 'models', 'best.onnx'),
    )
else:
    STATIC_FOLDER = os.path.join(BASE_DIR, 'static')
    SAVE_DIR = os.environ.get(
        "EG400_SAVE_DIR",
        os.path.join(PROJECT_DIR, 'data'),
    )
    MODEL_PATH = os.environ.get(
        "EG400_MODEL_PATH",
        os.path.join(PROJECT_DIR, 'models', 'best.onnx'),
    )

# ============================================================
# 网络端口
# ============================================================
CMD_HOST = os.environ.get("EG400_CMD_HOST", "127.0.0.1")
CMD_PORT = _env_int("EG400_CMD_PORT", 9001)  # 向通信桥发送控制指令
UDP_PORT = _env_int("EG400_UDP_PORT", 9998)  # 接收通信桥发回的状态
GAMEPAD_PORT = 9002          # 接收 Windows 端手柄桥接 UDP 数据
GAMEPAD_TIMEOUT_S = 0.5      # 手柄数据超时（秒），手动模式下超时自动急停

# ============================================================
# USB 温度传感器（LES11-MC-Dx / Modbus RTU）
# ============================================================
TEMP_SENSOR_PORT = os.environ.get("TEMP_SENSOR_PORT", "/dev/ttyUSB0")
TEMP_SENSOR_BAUDRATE = 9600
TEMP_SENSOR_INTERVAL_S = 0.5

# ============================================================
# 摄像头参数
# ============================================================
CAMERA_INDEX = 0
CAMERA_WIDTH = 640
CAMERA_HEIGHT = 480
TARGET_WIDTH = 256    # YOLO 推理分辨率
TARGET_HEIGHT = 192
JPEG_QUALITY = 70
SHOW_STREAM_STATS = True    # 是否在网页相机标题栏显示后端统计的视频 FPS

# ============================================================
# YOLO 推理参数
# ============================================================
YOLO_CONFIDENCE = 0.3
YOLO_IOU = 0.45
YOLO_DETECT_EVERY_N_FRAMES = 3  # 每 3 帧执行一次 YOLO，其余帧复用最近检测结果
DEFECT_CONFIDENCE = 0.5    # 超过此阈值才触发记录
LOG_COOLDOWN = 3.0         # 同类型缺陷记录冷却（秒）

# 类别映射（模型输出索引 → 中文名）
DEFECT_NAME_MAP = {
    0: "管壁脱落",
    1: "裂缝",
    2: "破洞",
    3: "树根侵入",
    4: "泥沙淤积",
}

# 模型标签（中英文）到统一中文缺陷名称的映射。
# 新的五分类 ONNX 模型只要使用这些标签之一，就不需要改动分级代码。
DEFECT_TYPE_ALIASES = {
    "spall": "管壁脱落",
    "spalling": "管壁脱落",
    "peeling": "管壁脱落",
    "wall_spalling": "管壁脱落",
    "wall_peeling": "管壁脱落",
    "管壁脱落": "管壁脱落",
    "crack": "裂缝",
    "cracks": "裂缝",
    "裂纹": "裂缝",
    "裂缝": "裂缝",
    "sediment": "泥沙淤积",
    "silt": "泥沙淤积",
    "deposit": "泥沙淤积",
    "泥沙淤积": "泥沙淤积",
    "hole": "破洞",
    "破洞": "破洞",
    "root": "树根侵入",
    "roots": "树根侵入",
    "tree_root": "树根侵入",
    "树根": "树根侵入",
    "树根侵入": "树根侵入",
}

# 比赛展示使用的固定三级分级：类别是唯一分级依据。
# bgr 用于 OpenCV 检测框；color 供 API / 前端作为语义颜色使用。
DEFECT_SEVERITY_PROFILES = {
    "管壁脱落": {
        "type": "管壁脱落",
        "severity": "轻度",
        "severity_code": 1,
        "color": "yellow",
        "bgr": (0, 255, 255),
        "advice": "建议持续观察",
    },
    "裂缝": {
        "type": "裂缝",
        "severity": "中度",
        "severity_code": 2,
        "color": "orange",
        "bgr": (0, 165, 255),
        "advice": "建议安排检修",
    },
    "泥沙淤积": {
        "type": "泥沙淤积",
        "severity": "中度",
        "severity_code": 2,
        "color": "orange",
        "bgr": (0, 165, 255),
        "advice": "建议安排清理",
    },
    "破洞": {
        "type": "破洞",
        "severity": "重度",
        "severity_code": 3,
        "color": "red",
        "bgr": (0, 0, 255),
        "advice": "建议立即检修",
    },
    "树根侵入": {
        "type": "树根侵入",
        "severity": "重度",
        "severity_code": 3,
        "color": "red",
        "bgr": (0, 0, 255),
        "advice": "建议立即处理",
    },
}

# YOLO 实时画框使用独立的英文标签和类别颜色。
# 业务日志、CSV 和前端仍使用上面的中文严重度配置。
YOLO_CLASS_NAMES = {
    0: "spall",
    1: "crack",
    2: "hole",
    3: "root",
    4: "sediment",
}

YOLO_ANNOTATION_STYLES = {
    "spall": {"severity": "Mild", "bgr": (0, 255, 0)},
    "crack": {"severity": "Moderate", "bgr": (0, 255, 255)},
    "hole": {"severity": "Severe", "bgr": (0, 0, 255)},
    "root": {"severity": "Severe", "bgr": (255, 0, 255)},
    "sediment": {"severity": "Moderate", "bgr": (255, 0, 0)},
}

# ============================================================
# 垂向推进（下潜）—— 2026-10-05 队友固件契约
# ============================================================
# 两路垂向推进器接 PCA9685 ch8/ch9，只做下潜；本艇正浮力，
# "上浮" = 把速度调小或停掉。速度经 CTRL: 帧第 16/17 字段下发
# （-100..+100，0 = 不转），开/停走 CMD:DIVE / CMD:DIVE:STOP。
# 默认 0 是故意的：若 CTRL 没带这两个字段，按"下潜"会回
# ERR: DIVE STRENGTH ZERO —— 一眼看出"字段没加上"。
DIVE_LEFT_PCT = 0.0    # CMD:DIVE 时 ch8 输出速度 %，-100..+100（0 = 不转）
DIVE_RIGHT_PCT = 0.0   # CMD:DIVE 时 ch9 输出速度 %，-100..+100

# ============================================================
# 横滚稳定（防侧翻）—— 2026-10-05 队友固件契约（CTRL: 18~22 字段）
# ============================================================
# 下潜时 ch8(左)+corr / ch9(右)-corr 纠平艇体；corr 由横滚 PID 算出。
# 增益全 0 = 功能关闭（上电默认）；水平基准靠 CMD:CALIBRATE 标定（同航向标定）。
# 默认值与交接单 §1.1 一致：kp/ki/kd=0（关）、limit=20、sign=+1
ROLL_KP = 0.0      # 横滚 P（倾角→差动 %），0~20
ROLL_KI = 0.0      # 横滚 I（消恒定倾斜），0~5
ROLL_KD = 0.0      # 横滚 D（角速度阻尼），0~5
ROLL_LIMIT = 20.0   # 单路修正上限 %，0~100
ROLL_SIGN = 1.0     # 修正方向：负值=反向，其它=正向，-1~1

# ============================================================
# 轨迹与里程
# ============================================================
TRAJECTORY_SPEED_M_S = 0.15     # 用于里程累计的真实速度 (m/s)
GIS_SPEED_MM_S = 50.0           # 用于 GIS 可视化绘图的假定速度 (mm/s)
# 里程上限钳制（2026-10-18）：无编码器，里程=时间×速度，机器人原地空转
# （如陆上桌面调试 is_moving=true 但车没动）里程也会一直涨 → 1.6m 演示管
# 算出 2.8m。钳到演示管实测范围内（用户要求 1.5~2.0m，取 1.6m 管长+余量）。
TRAJECTORY_MAX_M = float(os.environ.get("EG400_TRAJECTORY_MAX_M", "1.8"))

# 3D 走廊采样密度（2026-10-01：适配短管道/弯道）
CORRIDOR_STEP_M = float(os.environ.get("EG400_CORRIDOR_STEP_M", "0.25"))
CORRIDOR_TURN_TRIGGER_DEG = float(os.environ.get("EG400_CORRIDOR_TURN_DEG", "8.0"))
# 管径有效性钳制（2026-10-18）：陆上调试时超声打到桌子/墙壁乱反射，
# 管径读数乱跳（可达 2m+）→ 走廊环/管径忽大忽小全堆原点开花。
# 实测管道管径只可能落在该区间；越界一律回退默认管径。
CORRIDOR_DIAMETER_MIN_M = float(os.environ.get("EG400_CORRIDOR_DMIN", "0.50"))
CORRIDOR_DIAMETER_MAX_M = float(os.environ.get("EG400_CORRIDOR_DMAX", "0.60"))
CORRIDOR_FALLBACK_DIAMETER_M = float(os.environ.get("EG400_CORRIDOR_DFB", "0.55"))

# 3D 走廊固定轨迹（2026-10-27 用户拍板：按实际搭建的 L 形演示管锁死几何）
# 实物：绿色波纹软管，总长 ~1.8m = 0.9m 入口直道 + 左转 90°（中线 R0.30m，
# 弧长 0.47m）+ 转后直道 0.43m。里程=时间×速度推算、遥测 yaw 在演示中不代表
# 真实转弯，自由积分会把 L 管画成 1.8m 大直道（"直道 1.5m"问题）——
# 开启后走廊几何把里程直接投影到该锁死轨道，不再信遥测 yaw。
# 设 EG400_CORRIDOR_FIXED_TRACK=0 可回退自由积分。
CORRIDOR_FIXED_TRACK = os.environ.get("EG400_CORRIDOR_FIXED_TRACK", "1") == "1"
CORRIDOR_TRACK_S1_M = float(os.environ.get("EG400_TRACK_S1", "0.90"))
CORRIDOR_TRACK_TURN_DEG = float(os.environ.get("EG400_TRACK_TURN_DEG", "90"))
CORRIDOR_TRACK_TURN_R_M = float(os.environ.get("EG400_TRACK_TURN_R", "0.30"))

# ============================================================
# 共享可变状态（跨模块、跨线程读写）
# 注意：只能修改 dict/list 内部内容，不要整体赋值！
# ============================================================

global_frame = None         # 最新一帧 JPEG 字节，供 MJPEG 推流使用
global_frame_id = 0         # 最新推流帧编号，供浏览器端统计实际新帧
global_frame_timestamp_ms = 0  # 最新推流帧生成时间（毫秒时间戳）
ENABLE_YOLO = False         # 是否启用 YOLO 缺陷检测
PRELOAD_YOLO = False         # 启动时同步预加载模型，避免首次开启识别时长时间等待
is_recording = False        # 是否正在录像
take_photo_flag = False     # 拍照触发标志（一次性）
video_writer = None         # cv2.VideoWriter 实例

current_inspect_dir = ""    # 当前巡检的专属目录路径
defect_logs = []            # 缺陷记录列表
DEFECT_RECORDING = True     # 缺陷自动记录开关（web🤖按钮 / 手柄Y键 控制；8080 识别恒开，此开关只管"记录"）
total_mileage = 0.0         # 累计行驶里程 (m)
trajectory_data = []        # 轨迹采样点

MANUAL_MODE = False         # 是否处于手柄手动遥控模式

system_status = {
    "is_running": False,
    "is_moving": False,
    "light1_on": False,
    "light2_on": False,
    "yolo_on": False,
    "defect_recording": True,
    "stream_fps": 0.0,
    "error": 0,
    "roll": 0.0,
    "pitch": 0.0,
    "yaw": 0.0,
    "heading_calibration_seq": 0,
    "sonar_front": 0,
    "sonar_left": 0,
    "sonar_right": 0,
    "sonar_fault": False,
    "sonar_fault_message": "",
    "sonar_fault_channels": [],
    "temperature": None,
    "temperature_online": False,
    "temperature_error": "",
    "manual_mode": False,       # 手柄手动模式（供前端显示 AUTO/MANUAL）
    "gamepad_online": False,    # 手柄是否在线
    "motor_command_state": "stopped",
    "motor_control_message": "",
    "heading_offset_deg": 0.0,  # 2026-10-04 航向基准偏移（±90° 累计，wrap ±180；UI 不显示数字）
    "dive_state": "stopped",    # 2026-10-05 下潜通道状态（由固件 ACK 文本更新）："on" / "stopped"
    "nav_fault": "",
    # ---- ENV: 三路环境传感器（APU1 固件 v2 合并上报，2026-10-15 格式变更后）----
    # v2 去掉了 *V 标志位，固件直接丢弃无效数据用 0 哨兵
    "depth_mm": 0,
    "turbidity": 0,
    "gas_co": 0,
    "gas_h2s": 0,
    "gas_o2": 0.0,
    "gas_ch4": 0,
    "web_only": WEB_ONLY,
    "simulation_mode": SIMULATION_MODE,
    "hardware_control_enabled": hardware_control_enabled(),
    "loopback_mode": LOOPBACK_MODE,
    "runtime_mode": runtime_mode(),
}


def get_system_status():
    """返回包含动态运行模式的状态快照，避免 API 暴露过期配置。"""
    status = dict(system_status)
    status.update({
        "web_only": WEB_ONLY,
        "simulation_mode": SIMULATION_MODE,
        "hardware_control_enabled": hardware_control_enabled(),
        "loopback_mode": LOOPBACK_MODE,
        "runtime_mode": runtime_mode(),
    })
    return status
