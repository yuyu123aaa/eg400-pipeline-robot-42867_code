"""
管道巡检机器人 — Web 控制台入口
================================
启动 Flask 应用，聚合 core/ 下的各业务模块。
"""

import json
import math
import os
import threading
import time
import urllib.request
from flask import Flask, Response, abort, jsonify, render_template, request, send_from_directory

import config
from core.comm import send_str_to_linux, start_udp_listener
from core.vision import init_yolo, set_yolo_enabled, start_camera_thread, gen_frames
from core.mapping import start_mapping_thread
from core.gamepad import start_gamepad_listener
from core.temperature import start_temperature_thread
from core.defect_recorder import start_defect_recorder_thread
from core.corridor import start_corridor_thread, corridor_current, corridor_history
from core.evaluation import (
    evaluate_defects, evaluate_history, compliance_checklist,
)
from core.management import build_maintenance_plan, build_network_overview
from core.motor_control import start_motor, stop_motor
from core.calibration import recalibrate_heading
from core.inspection import (
    get_defect_logs, get_history_list, get_history_detail,
)

# ---- Flask 应用初始化 ----
app = Flask(__name__,
            static_folder=config.STATIC_FOLDER,
            static_url_path='/static',
            template_folder='.')

if not os.path.exists(config.SAVE_DIR):
    os.makedirs(config.SAVE_DIR)


# ============================================================
# API 路由
# ============================================================


def _simulation_response(message, **extra):
    """返回不会误导为真实硬件已执行的模拟模式响应。"""
    payload = {
        "status": "simulation",
        "message": message,
        "runtime_mode": config.runtime_mode(),
    }
    payload.update(extra)
    return jsonify(payload)


def _loopback_response(message, **extra):
    """返回明确标记为回环的响应，不伪装成真实外设执行结果。"""
    payload = {
        "status": "loopback",
        "message": message,
        "runtime_mode": config.runtime_mode(),
    }
    payload.update(extra)
    return jsonify(payload)


@app.route('/')
def index():
    return render_template(
        'dashboard/index.html',
        save_dir=config.SAVE_DIR,
        show_stream_stats=config.SHOW_STREAM_STATS,
    )


@app.route('/dashboard/<path:filename>')
def dashboard_asset(filename):
    if filename not in {'style.css', 'app.js', 'corridor.js'}:
        abort(404)
    dashboard_dir = os.path.join(app.root_path, 'dashboard')
    return send_from_directory(dashboard_dir, filename)


# ---- 视觉服务代理（EG400 NPU 视觉程序，8080 端口） ----
# 2026-09-21: /video_feed 由"本机 OpenCV 相机"改为代理 NPU 视觉程序的 MJPEG 流，
# 上游路径自动探测并缓存，兼容不同版本视觉程序的流路径差异。
_VISION_BASE = "http://127.0.0.1:8080"
# 2026-09-21 实测：视觉程序的真实流路径是 /stream.mjpg（带 .mjpg 后缀）
_VISION_STREAM_CANDIDATES = ("/live", "/stream.mjpg", "/stream", "/mjpeg", "/video",
                             "/video_feed", "/cam", "/")
_vision_stream_state = {"path": None}
_vision_stream_lock = threading.Lock()


def _vision_stream_path():
    """探测 NPU 视觉程序的 MJPEG 流路径，命中后缓存。"""
    with _vision_stream_lock:
        cached = _vision_stream_state["path"]
    if cached:
        return cached
    for candidate in _VISION_STREAM_CANDIDATES:
        url = _VISION_BASE + candidate
        try:
            req = urllib.request.Request(url, headers={"Cache-Control": "no-store"})
            with urllib.request.urlopen(req, timeout=1.5) as resp:
                ctype = resp.headers.get("Content-Type", "")
                if ctype.startswith(("multipart/x-mixed-replace", "image/jpeg")):
                    with _vision_stream_lock:
                        _vision_stream_state["path"] = candidate
                    return candidate
        except Exception:
            continue
    return None


# ---- 2026-10-05: 视觉帧中枢（Frame Hub） ----
# 视觉程序的 MJPEG 流【只允许一个客户端】（"one 720p/5fps stream"）。
# 此前 /video_feed 中继、拍照、3D 走廊抓帧、缺陷记录各自开连接互相抢，
# 拍照时浏览器正占着流 → 第二条连接被回 503 → "拍照失败"。
# 修复：后台常驻一个独占连接持续读流，最新 JPEG 存内存；
# /video_feed、拍照、走廊、缺陷记录全部从_hub 取帧，一条连接大家共用。
_vision_hub_state = {
    "frame": None,      # 最新完整 JPEG 字节
    "ts": 0.0,          # 该帧的更新时间戳（用 time.monotonic()）
    "started": False,
}
_vision_hub_lock = threading.Lock()


def _vision_hub_loop():
    """独占视觉流连接，持续解析 MJPEG 帧并更新 _vision_hub_state。"""
    while True:
        upstream = None
        try:
            stream_path = _vision_stream_path()
            if not stream_path:
                time.sleep(2.0)
                continue
            upstream = urllib.request.urlopen(
                urllib.request.Request(
                    _VISION_BASE + stream_path,
                    headers={"Cache-Control": "no-store"},
                ),
                timeout=10,
            )
            buffer = b""
            while True:
                chunk = upstream.read(4096)
                if not chunk:
                    break
                buffer += chunk
                while True:
                    start = buffer.find(b"\xff\xd8")
                    if start < 0:
                        buffer = b""
                        break
                    end = buffer.find(b"\xff\xd9", start + 2)
                    if end < 0:
                        buffer = buffer[start:]
                        break
                    jpeg = buffer[start:end + 2]
                    buffer = buffer[end + 2:]
                    with _vision_hub_lock:
                        _vision_hub_state["frame"] = jpeg
                        _vision_hub_state["ts"] = time.monotonic()
        except Exception:
            pass
        finally:
            if upstream is not None:
                try:
                    upstream.close()
                except Exception:
                    pass
        # 断流后清空旧帧并稍等重连
        with _vision_hub_lock:
            _vision_hub_state["frame"] = None
        time.sleep(1.0)


def start_vision_hub():
    """启动帧中枢线程（幂等）。"""
    with _vision_hub_lock:
        if _vision_hub_state["started"]:
            return
        _vision_hub_state["started"] = True
    thread = threading.Thread(target=_vision_hub_loop, daemon=True)
    thread.start()
    print("📷 视觉帧中枢已启动（独占 /live 单客户端流，供视频/拍照/走廊共用）", flush=True)


def _vision_grab_frame(timeout_seconds=3.0):
    """取最新一帧 JPEG —— 从帧中枢读取（不再另开流连接）。"""
    deadline = time.monotonic() + timeout_seconds
    entry_ts = _vision_hub_state["ts"]
    while time.monotonic() < deadline:
        with _vision_hub_lock:
            frame = _vision_hub_state["frame"]
            ts = _vision_hub_state["ts"]
        if frame and ts > entry_ts:
            return frame
        if frame and ts == entry_ts and entry_ts > 0:
            # 已有帧且中枢没断流：直接用（允许稍旧）
            return frame
        time.sleep(0.1)
    with _vision_hub_lock:
        return _vision_hub_state["frame"]


@app.route('/video_feed')
def video_feed():
    # 2026-10-05: 改由帧中枢供流 —— 不再直接中继上游连接（抢单客户端流）。
    # 从 _vision_hub_state 取最新 JPEG，按 multipart/x-mixed-replace 推给浏览器。
    def _relay():
        last_ts = 0.0
        idle_since = None
        while True:
            with _vision_hub_lock:
                frame = _vision_hub_state["frame"]
                ts = _vision_hub_state["ts"]
            if frame and ts != last_ts:
                last_ts = ts
                idle_since = None
                yield (b"--frame\r\nContent-Type: image/jpeg\r\n\r\n"
                       + frame + b"\r\n")
            else:
                # 无新帧：短暂等待；断流超 8 秒则结束本次连接（浏览器会自动重连）
                now = time.monotonic()
                if idle_since is None:
                    idle_since = now
                elif now - idle_since > 8.0:
                    return
                time.sleep(0.08)

    response = Response(_relay(), mimetype="multipart/x-mixed-replace; boundary=frame")
    response.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
    response.headers["Pragma"] = "no-cache"
    response.headers["Expires"] = "0"
    response.headers["X-Accel-Buffering"] = "no"
    return response


# 2026-10-05: 原 _vision_grab_frame（每次拍照临时开流连接）已删除，
# 统一改用上方帧中枢版本 —— 避免与 /video_feed 中继抢视觉程序的
# 单客户端流（第二条连接会被 503，导致拍照失败）。


def _vision_json(path):
    """透传视觉程序的 JSON 接口，离线时返回 None。"""
    try:
        with urllib.request.urlopen(_VISION_BASE + path, timeout=3) as resp:
            return json.loads(resp.read().decode("utf-8", "replace"))
    except Exception:
        return None


@app.route('/api/vision/detections')
def vision_detections():
    payload = _vision_json("/api/detections")
    if payload is None:
        return jsonify({"ready": False, "objects": []})
    return jsonify(payload)


@app.route('/api/vision/status')
def vision_status():
    payload = _vision_json("/api/status")
    if payload is None:
        return jsonify({"ready": False})
    return jsonify(payload)


@app.route('/api/vision/snapshot')
def vision_snapshot():
    frame = _vision_grab_frame()
    if frame is None:
        return jsonify({"status": "error", "message": "视觉流不可用"}), 503
    directory = os.path.join(config.SAVE_DIR, "photos")
    os.makedirs(directory, exist_ok=True)
    filename = time.strftime("photo_%Y%m%d_%H%M%S") + ".jpg"
    with open(os.path.join(directory, filename), "wb") as file:
        file.write(frame)
    return jsonify({
        "status": "success",
        "url": "/images/photos/" + filename,
        "bytes": len(frame),
    })


_vision_recorder_state = {"thread": None, "stop": None, "path": None}


def _vision_record_worker(stop_event, output_path):
    """后台录像：从视觉帧中枢轮询取最新 JPEG，解码后写入 MP4。

    2026-10-18 修复：原来这里自己 urlopen 开第二条流连接 —— 但视觉程序的
    MJPEG 流只允许一个客户端，且该连接已被帧中枢常驻独占，第二条连接会被
    503 拒绝，worker 立即退出 → "没有生成录像文件"（帧中枢重构时漏改的）。
    现与拍照/走廊/缺陷记录同源，从 _vision_hub_state 取帧。"""
    import cv2
    import numpy as np

    writer = None
    last_ts = 0.0
    idle_since = None
    try:
        while not stop_event.is_set():
            with _vision_hub_lock:
                frame = _vision_hub_state["frame"]
                ts = _vision_hub_state["ts"]
            if frame and ts != last_ts:
                last_ts = ts
                idle_since = None
                decoded = cv2.imdecode(
                    np.frombuffer(frame, dtype=np.uint8), cv2.IMREAD_COLOR)
                if decoded is not None:
                    if writer is None:
                        height, width = decoded.shape[:2]
                        writer = cv2.VideoWriter(
                            output_path,
                            cv2.VideoWriter_fourcc(*"mp4v"),
                            15.0,
                            (width, height),
                        )
                    writer.write(decoded)
            else:
                # 视觉流断流：8 秒拿不到新帧则结束（开头就没帧 → stop 报
                # "没有生成录像文件"；录到一半断流 → 保留已录内容正常收尾）
                now = time.monotonic()
                if idle_since is None:
                    idle_since = now
                elif now - idle_since > 8.0:
                    return
                time.sleep(0.05)
    finally:
        if writer is not None:
            writer.release()
        upstream.close()


_annotated_frame = None   # 前端 canvas 合成帧（带框），缺陷快照用

@app.route('/api/vision/annotated_frame', methods=['POST'])
def save_annotated_frame():
    """前端把画好框的 canvas 帧发过来存着，缺陷记录线程取用。"""
    global _annotated_frame
    data = request.get_json(silent=True)
    if data and data.get('image'):
        import base64
        _annotated_frame = base64.b64decode(data['image'].split(',')[1])
    return jsonify({"status": "ok"})

@app.route('/api/vision/annotated_frame')
def get_annotated_frame():
    global _annotated_frame
    if _annotated_frame:
        return Response(_annotated_frame, mimetype='image/jpeg')
    return jsonify({"status": "error", "message": "no frame"}), 404

@app.route('/api/vision/record/<state>')
def vision_record(state):
    if state == "start":
        worker = _vision_recorder_state["thread"]
        if worker and worker.is_alive():
            return jsonify({"status": "error", "message": "录像已在进行中"})
        stream_path = _vision_stream_path()
        if not stream_path:
            return jsonify({"status": "error", "message": "视觉流不可用"}), 503
        directory = os.path.join(config.SAVE_DIR, "videos")
        os.makedirs(directory, exist_ok=True)
        output_path = os.path.join(
            directory, time.strftime("record_%Y%m%d_%H%M%S") + ".mp4")
        stop_event = threading.Event()
        worker = threading.Thread(
            target=_vision_record_worker,
            args=(stop_event, output_path),
            daemon=True,
        )
        worker.start()
        _vision_recorder_state.update(
            thread=worker, stop=stop_event, path=output_path)
        return jsonify({"status": "success", "recording": True})

    if state == "stop":
        stop_event = _vision_recorder_state["stop"]
        if stop_event:
            stop_event.set()
        worker = _vision_recorder_state["thread"]
        if worker:
            worker.join(timeout=15)
        output_path = _vision_recorder_state["path"]
        _vision_recorder_state.update(thread=None, stop=None, path=None)
        if output_path and os.path.exists(output_path):
            relative = os.path.relpath(output_path, config.SAVE_DIR)
            return jsonify({"status": "success", "url": "/images/" + relative})
        return jsonify({"status": "error", "message": "没有生成录像文件"})

    return jsonify({"status": "error", "message": "未知操作"}), 400


@app.route('/api/data')
def get_data():
    return jsonify(config.get_system_status())


@app.route('/api/defect_logs')
def api_defect_logs():
    return jsonify(get_defect_logs())


@app.route('/images/<path:subpath>')
def get_image(subpath):
    return send_from_directory(config.SAVE_DIR, subpath)


@app.route('/api/control/<action>')
def control(action):
    simulation = not config.command_transport_enabled()

    if action == "start":
        config.system_status["is_running"] = True
        config.system_status["heading_offset_deg"] = 0.0   # 新巡航从直道基准开始
        config.system_status["dive_state"] = "stopped"     # 新巡航下潜从"停"开始
        if simulation:
            config.system_status["motor_command_state"] = "simulation"
            config.system_status["motor_control_message"] = (
                "模拟模式：未发送 CMD:START"
            )
        else:
            _send_saved_motion_params()
            send_str_to_linux("CMD:START")

    elif action == "stop":
        config.system_status["is_running"] = False
        config.system_status["temperature"] = None
        config.system_status["is_moving"] = False
        config.system_status["heading_offset_deg"] = 0.0
        config.system_status["dive_state"] = "stopped"     # CMD:STOP 六路联动停车，下潜清零
        if simulation:
            config.system_status["motor_command_state"] = "stopped"
            config.system_status["motor_control_message"] = (
                "模拟模式：未发送 CMD:STOP"
            )
        else:
            send_str_to_linux("CMD:STOP")

    elif action == 'led1on':
        config.system_status["light1_on"] = True
        if simulation:
            config.system_status["motor_control_message"] = (
                "模拟模式：未发送 CMD:LED1ON"
            )
        else:
            send_str_to_linux("CMD:LED1ON")
    elif action == 'led1off':
        config.system_status["light1_on"] = False
        if simulation:
            config.system_status["motor_control_message"] = (
                "模拟模式：未发送 CMD:LED1OFF"
            )
        else:
            send_str_to_linux("CMD:LED1OFF")
    elif action == 'led2on':
        config.system_status["light2_on"] = True
        if simulation:
            config.system_status["motor_control_message"] = (
                "模拟模式：未发送 CMD:LED2ON"
            )
        else:
            send_str_to_linux("CMD:LED2ON")
    elif action == 'led2off':
        config.system_status["light2_on"] = False
        if simulation:
            config.system_status["motor_control_message"] = (
                "模拟模式：未发送 CMD:LED2OFF"
            )
        else:
            send_str_to_linux("CMD:LED2OFF")

    # ---- 气路：吸样开始 / 急停（2026-10-16 新增）------------------------
    #  时序（阀→2s→泵→6s→泵关→0.5s→阀关）由 APU1 固件状态机强制保证，
    #  Web 侧只发命令，不做任何时序控制。
    #  回执走现有 control_ack：ACK: GAS SAMPLE STARTED / GAS STOPPED /
    #                        ERR: GAS BUSY / ERR: GAS GPIO NOT READY
    elif action == 'gas_sample_start':
        if simulation:
            return _simulation_response("模拟模式：未发送 CMD:GAS_SAMPLE_START")
        send_str_to_linux("CMD:GAS_SAMPLE_START")

    elif action == 'gas_stop':
        if simulation:
            return _simulation_response("模拟模式：未发送 CMD:GAS_STOP")
        send_str_to_linux("CMD:GAS_STOP")

    elif action == 'record_toggle':
        # 2026-10-01: 原 yoloon/yolooff（指向已废弃本机YOLO）改为缺陷自动记录开关
        from core.defect_recorder import set_recording
        enabled = set_recording(not config.DEFECT_RECORDING)
        return jsonify({
            "status": "success",
            "defect_recording": enabled,
        })

    elif action == 'calibrate':
        if simulation:
            calibration_seq = recalibrate_heading()
            return _simulation_response(
                "模拟模式：未读取真实姿态传感器",
                heading_calibration_seq=calibration_seq,
            )
        calibration_seq = recalibrate_heading()
        if config.LOOPBACK_MODE:
            return _loopback_response(
                "回环模式：已发送标定命令，等待模拟裸机回执",
                heading_calibration_seq=calibration_seq,
            )
        return jsonify({
            "status": "success",
            "heading_calibration_seq": calibration_seq,
        })

    elif action == 'motor_start':
        config.system_status["heading_offset_deg"] = 0.0   # 新巡航从直道基准开始
        config.system_status["dive_state"] = "stopped"     # 新巡航下潜从"停"开始
        if simulation:
            config.system_status["motor_command_state"] = "simulation"
            config.system_status["motor_control_message"] = (
                "模拟模式：未启动真实电机"
            )
            config.system_status["is_moving"] = False
            return _simulation_response("模拟模式：未发送 CMD:MOTOR_START")
        _send_saved_motion_params()
        start_motor()

    elif action == 'motor_stop':
        config.system_status["dive_state"] = "stopped"     # MOTOR_STOP 六路联动停车，下潜清零
        if simulation:
            config.system_status["motor_command_state"] = "stopped"
            config.system_status["motor_control_message"] = (
                "模拟模式：未发送 CMD:MOTOR_STOP"
            )
            config.system_status["is_moving"] = False
            return _simulation_response("模拟模式：未发送 CMD:MOTOR_STOP")
        stop_motor()

    # ---- 2026-10-04: 航向基准角切换（弯道航向纠正，队友方案） ----
    # Web 按钮"左转/右转" → CMD:YAW:LEFT / CMD:YAW:RIGHT（协议详见
    # EG400_航向基准角协议对齐规范_20261004.md）。固件端把巡航目标航向
    # target_yaw ±90°（wrap 到 ±180）后交由现有航向 PID 执行。
    # 仅自动巡航运行中有效；手动模式/未启动时拒绝。
    elif action in ('yaw_left', 'yaw_right'):
        if not config.system_status["is_running"]:
            return jsonify({"status": "error", "message": "system not running"})
        if config.MANUAL_MODE:
            return jsonify({"status": "error", "message": "manual mode on"})
        going_left = (action == 'yaw_left')
        prev = config.system_status.get("heading_offset_deg", 0.0)
        new_off = ((prev + (90.0 if going_left else -90.0)) + 180.0) % 360.0 - 180.0
        config.system_status["heading_offset_deg"] = new_off
        if simulation:
            return _simulation_response(
                "模拟模式：未发送 CMD:YAW:{}".format("LEFT" if going_left else "RIGHT"),
                heading_offset_deg=new_off,
            )
        send_str_to_linux("CMD:YAW:{}".format("LEFT" if going_left else "RIGHT"))
        if config.LOOPBACK_MODE:
            return _loopback_response(
                "回环模式：航向基准切换命令已发送",
                heading_offset_deg=new_off,
            )
        return jsonify({
            "status": "pending",
            "message": "航向基准切换命令已下发，等待裸机确认",
            "heading_offset_deg": new_off,
        })

    # ---- 2026-10-05: 垂向推进（下潜），队友固件契约 ----
    # 两条命令都是整串精确匹配、不带任何数据：CMD:DIVE / CMD:DIVE:STOP。
    # 下潜速度不走命令，永远随 CTRL: 帧第 16/17 字段下发（见 _motion_params_command）。
    # 手动模式下【允许】下潜（与 yaw_left/right 不同 —— 手飞时也要能控深度）。
    elif action == 'dive':
        if not config.system_status["is_running"]:
            return jsonify({"status": "error", "message": "system not running"})
        if simulation:
            return _simulation_response("模拟模式：未发送 CMD:DIVE")
        send_str_to_linux("CMD:DIVE")
        if config.LOOPBACK_MODE:
            return _loopback_response(
                "回环模式：下潜命令已发送，等待模拟裸机回执"
            )
        return jsonify({
            "status": "pending",
            "message": "下潜命令已下发，等待裸机确认",
            "dive_state": config.system_status["dive_state"],
        })

    # CMD:DIVE:STOP 与 CMD:STOP / CMD:MOTOR_STOP 同一条规矩：永远放行，
    # 任何时候发都有效（固件侧也如此），Web 不做运行态拦截。
    elif action == 'dive_stop':
        if simulation:
            return _simulation_response("模拟模式：未发送 CMD:DIVE:STOP")
        send_str_to_linux("CMD:DIVE:STOP")
        if config.LOOPBACK_MODE:
            return _loopback_response(
                "回环模式：下潜停止命令已发送，等待模拟裸机回执"
            )
        return jsonify({
            "status": "pending",
            "message": "下潜停止命令已下发，等待裸机确认",
            "dive_state": config.system_status["dive_state"],
        })

    # ---- 2026-09-27: Web 手动运动控制（与手柄共用 MANUAL_MODE 仲裁） ----
    elif action == 'manual_start':
        config.MANUAL_MODE = True
        config.system_status["manual_mode"] = True
        config.system_status["dive_state"] = "stopped"     # 进入手动模式触发六路联动停车，下潜清零
        if simulation:
            return _simulation_response("模拟模式：未发送 CMD:MANUAL_START")
        send_str_to_linux("CMD:MANUAL_START")

    elif action == 'manual_stop':
        config.MANUAL_MODE = False
        config.system_status["manual_mode"] = False
        config.system_status["is_moving"] = False
        config.system_status["dive_state"] = "stopped"     # CMD:STOP 六路联动停车，下潜清零
        if simulation:
            return _simulation_response("模拟模式：未发送 CMD:STOP")
        send_str_to_linux("CMD:STOP")

    elif action == 'speed':
        # SPEED:<throttle>,<turn> —— 与手柄手动模式完全同一条裸机混控链路
        # throttle: -100(后退)~100(前进)；turn: -100(左)~100(右)
        try:
            throttle = max(-100, min(100, int(request.args.get('throttle', 0))))
            turn = max(-100, min(100, int(request.args.get('turn', 0))))
        except (TypeError, ValueError):
            return jsonify({"status": "error", "message": "invalid speed params"})
        if not config.MANUAL_MODE:
            return jsonify({"status": "error", "message": "manual mode off"})
        if not config.system_status["is_running"]:
            return jsonify({"status": "error", "message": "system not running"})
        config.system_status["is_moving"] = (throttle != 0 or turn != 0)
        if simulation:
            return _simulation_response(f"模拟模式：未发送 SPEED:{throttle},{turn}")
        send_str_to_linux(f"SPEED:{throttle},{turn}")

    if simulation:
        return _simulation_response("模拟模式：未发送真实硬件指令")
    if config.LOOPBACK_MODE:
        return _loopback_response("回环模式：命令已发送，等待模拟裸机回执")
    return jsonify({"status": "success"})


@app.route('/api/media/<action_type>')
def media_action(action_type):
    if not config.hardware_control_enabled():
        return _simulation_response(
            "模拟模式：未连接摄像头，未执行媒体操作"
        )
    if action_type == 'photo':
        config.take_photo_flag = True
    elif action_type == 'record_start':
        config.is_recording = True
    elif action_type == 'record_stop':
        config.is_recording = False
    return jsonify({"status": "success"})


MOTION_PARAM_RANGES = {
    "kp": (0.0, 100.0),
    "ki": (0.0, 20.0),
    "kd": (0.0, 20.0),
    "heading_kp": (0.0, 2.0),
    "yaw_rate_damping": (0.0, 2.0),
    "integral_limit": (0.0, 100.0),
    "turn_limit": (0.001, 100.0),
    "base_speed": (0.0, 100.0),
    "min_output": (-100.0, 100.0),
    "max_output": (0.0, 100.0),
    "left_trim": (0.5, 1.0),
    "right_trim": (0.5, 1.0),
    "front_yaw_share": (0.0, 100.0),
    "rear_yaw_share": (0.0, 100.0),
    "front_max_output": (0.0, 100.0),
}

# 2026-10-02：回退到旧版固件（9-26）对应的参数表 —— 新版固件（3384bc64）
# 的 15 字段 CTRL（含 base_front_share）随固件一并回退，待固件重新定型后再对接。
DEFAULT_MOTION_PARAMS = {
    "kp": 20.0,
    "ki": 0.0,
    "kd": 1.0,
    "heading_kp": 0.4,
    "yaw_rate_damping": 0.15,
    "integral_limit": 0.5,
    "turn_limit": 15.0,
    "base_speed": 20.0,
    "min_output": 5.0,
    "max_output": 35.0,
    "left_trim": 1.0,
    "right_trim": 1.0,
    "front_yaw_share": 80.0,
    "rear_yaw_share": 20.0,
    "front_max_output": 15.0,
}

MOTION_PARAMS_FILE = os.path.join(
    config.SAVE_DIR,
    "motion_control_params.json",
)


def _parse_motion_params(payload):
    if not isinstance(payload, dict):
        raise ValueError("请求数据格式错误")

    values = {}
    for name, (minimum, maximum) in MOTION_PARAM_RANGES.items():
        raw_value = payload.get(name)
        if isinstance(raw_value, bool):
            raise ValueError(f"{name} 格式错误")
        try:
            value = float(raw_value)
        except (TypeError, ValueError):
            raise ValueError(f"{name} 缺失或格式错误") from None
        if not math.isfinite(value) or not minimum <= value <= maximum:
            raise ValueError(
                f"{name} 必须在 {minimum:g}～{maximum:g} 范围内"
            )
        values[name] = value

    if abs(
        values["front_yaw_share"] +
        values["rear_yaw_share"] -
        100.0
    ) > 0.01:
        raise ValueError("前后 Yaw 占比合计必须为 100%")

    if not (
        values["min_output"] <=
        values["base_speed"] <=
        values["max_output"]
    ):
        raise ValueError("必须满足最小输出 ≤ 基础速度 ≤ 最大输出")

    if (
        values["turn_limit"] >
        values["max_output"] - values["min_output"]
    ):
        raise ValueError("转向限制不能大于最大输出与最小输出之差")

    return values


# 2026-10-05: 下潜速度字段（CTRL: 第 16/17 个）—— 与 15 个基础参数分开处理：
# 载荷里可以不带（兼容旧客户端 / 旧参数文件），不带时保持当前值不动。
DIVE_PARAM_RANGES = {
    "dive_left": (-100.0, 100.0),
    "dive_right": (-100.0, 100.0),
}

# 2026-10-05: 横滚稳定参数（CTRL: 第 18~22 字段）—— 与下潜速度同模式：
# 载荷可不带（兼容旧客户端/旧参数文件），不带时保持现值。默认值与交接单 §1.1 一致。
ROLL_PARAM_RANGES = {
    "roll_kp": (0.0, 20.0),
    "roll_ki": (0.0, 5.0),
    "roll_kd": (0.0, 5.0),
    "roll_limit": (0.0, 100.0),
    "roll_sign": (-1.0, 1.0),
}


def _parse_dive_params(payload):
    """解析可选的下潜速度字段；载荷完全没带时返回 None（保持现值）。

    只要带了其中一个，另一个就必须一起带（固件 17 字段一起原子生效）。
    """
    if not isinstance(payload, dict) or not any(
        name in payload for name in DIVE_PARAM_RANGES
    ):
        return None

    values = {}
    for name, (minimum, maximum) in DIVE_PARAM_RANGES.items():
        raw_value = payload.get(name)
        if isinstance(raw_value, bool):
            raise ValueError(f"{name} 格式错误")
        try:
            value = float(raw_value)
        except (TypeError, ValueError):
            raise ValueError(f"{name} 缺失或格式错误") from None
        if not math.isfinite(value) or not minimum <= value <= maximum:
            raise ValueError(
                f"{name} 必须在 {minimum:g}～{maximum:g} 范围内"
            )
        values[name] = value
    return values


def _parse_roll_params(payload):
    """解析可选的横滚稳定参数；载荷完全没带时返回 None（保持现值）。

    只要带了其中一个，5 个就必须一起带（固件 22 字段一起原子生效）。
    """
    if not isinstance(payload, dict) or not any(
        name in payload for name in ROLL_PARAM_RANGES
    ):
        return None

    values = {}
    for name, (minimum, maximum) in ROLL_PARAM_RANGES.items():
        raw_value = payload.get(name)
        if isinstance(raw_value, bool):
            raise ValueError(f"{name} 格式错误")
        try:
            value = float(raw_value)
        except (TypeError, ValueError):
            raise ValueError(f"{name} 缺失或格式错误") from None
        if not math.isfinite(value) or not minimum <= value <= maximum:
            raise ValueError(
                f"{name} 必须在 {minimum:g}～{maximum:g} 范围内"
            )
        values[name] = value
    return values


def _motion_params_command(values):
    # 2026-10-05: 队友固件契约 —— CTRL: 由 15 个浮点扩为 22 个。
    #   前 15：基础运动控制参数（顺序/含义不变）
    #   16-17：下潜速度 dive_left / dive_right（-100..+100，0=不转）
    #   18-22：横滚稳定 roll_kp / roll_ki / roll_kd / roll_limit / roll_sign
    # 固件逐级兼容：15/17/22 字段都能接受，其它个数整帧拒绝。
    command_values = (
        values["kp"],
        values["ki"],
        values["kd"],
        values["heading_kp"],
        values["yaw_rate_damping"],
        values["integral_limit"],
        values["turn_limit"],
        values["base_speed"],
        values["min_output"],
        values["max_output"],
        values["left_trim"],
        values["right_trim"],
        values["front_yaw_share"] / 100.0,
        values["rear_yaw_share"] / 100.0,
        values["front_max_output"],
        config.DIVE_LEFT_PCT,     # 16: 下潜时 ch8 速度 %
        config.DIVE_RIGHT_PCT,    # 17: 下潜时 ch9 速度 %
        config.ROLL_KP,           # 18: 横滚 P
        config.ROLL_KI,            # 19: 横滚 I
        config.ROLL_KD,            # 20: 横滚 D
        config.ROLL_LIMIT,         # 21: 单路修正上限 %
        config.ROLL_SIGN,          # 22: 修正方向
    )
    return "CTRL:" + ",".join(
        f"{value:.3f}" for value in command_values
    )


def _save_motion_params(values):
    directory = os.path.dirname(MOTION_PARAMS_FILE)
    if directory:
        os.makedirs(directory, exist_ok=True)

    # 2026-10-05: 下潜速度 + 横滚参数与 15 个基础参数一起落盘（下次开机仍用）
    stored = dict(values)
    stored["dive_left"] = config.DIVE_LEFT_PCT
    stored["dive_right"] = config.DIVE_RIGHT_PCT
    stored["roll_kp"] = config.ROLL_KP
    stored["roll_ki"] = config.ROLL_KI
    stored["roll_kd"] = config.ROLL_KD
    stored["roll_limit"] = config.ROLL_LIMIT
    stored["roll_sign"] = config.ROLL_SIGN

    temporary_file = f"{MOTION_PARAMS_FILE}.tmp"
    with open(temporary_file, "w", encoding="utf-8") as file:
        json.dump(
            stored,
            file,
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
        file.write("\n")
    os.replace(temporary_file, MOTION_PARAMS_FILE)


def _load_motion_params():
    try:
        with open(MOTION_PARAMS_FILE, "r", encoding="utf-8") as file:
            payload = json.load(file)
        values = _parse_motion_params(payload)
        # 2026-10-05: 旧参数文件没有下潜/横滚字段 → 保持现值
        dive_values = _parse_dive_params(payload)
        if dive_values is not None:
            config.DIVE_LEFT_PCT = dive_values["dive_left"]
            config.DIVE_RIGHT_PCT = dive_values["dive_right"]
        roll_values = _parse_roll_params(payload)
        if roll_values is not None:
            config.ROLL_KP = roll_values["roll_kp"]
            config.ROLL_KI = roll_values["roll_ki"]
            config.ROLL_KD = roll_values["roll_kd"]
            config.ROLL_LIMIT = roll_values["roll_limit"]
            config.ROLL_SIGN = roll_values["roll_sign"]
        return values
    except (OSError, ValueError, json.JSONDecodeError):
        return dict(DEFAULT_MOTION_PARAMS)


def _send_saved_motion_params():
    if not config.command_transport_enabled():
        return False
    return send_str_to_linux(_motion_params_command(_load_motion_params()))


@app.route('/api/control/motion_params', methods=['GET'])
def get_motion_params():
    params = _load_motion_params()
    # 2026-10-05: 一并返回下潜速度 + 横滚参数，供前端回填
    params["dive_left"] = config.DIVE_LEFT_PCT
    params["dive_right"] = config.DIVE_RIGHT_PCT
    params["roll_kp"] = config.ROLL_KP
    params["roll_ki"] = config.ROLL_KI
    params["roll_kd"] = config.ROLL_KD
    params["roll_limit"] = config.ROLL_LIMIT
    params["roll_sign"] = config.ROLL_SIGN
    return jsonify(params)


@app.route('/api/control/set_motion_params', methods=['POST'])
def set_motion_params():
    payload = request.get_json(silent=True)
    try:
        values = _parse_motion_params(payload)
        # 2026-10-05: 可选的下潜速度 + 横滚参数（不带 → 保持现值，兼容旧客户端）
        dive_values = _parse_dive_params(payload)
        roll_values = _parse_roll_params(payload)
    except ValueError as exc:
        return jsonify({
            "status": "error",
            "message": str(exc),
        }), 400

    if dive_values is not None:
        config.DIVE_LEFT_PCT = dive_values["dive_left"]
        config.DIVE_RIGHT_PCT = dive_values["dive_right"]
    if roll_values is not None:
        config.ROLL_KP = roll_values["roll_kp"]
        config.ROLL_KI = roll_values["roll_ki"]
        config.ROLL_KD = roll_values["roll_kd"]
        config.ROLL_LIMIT = roll_values["roll_limit"]
        config.ROLL_SIGN = roll_values["roll_sign"]

    try:
        _save_motion_params(values)
    except OSError as exc:
        return jsonify({
            "status": "error",
            "message": f"参数保存失败：{exc}",
        }), 500

    if not config.command_transport_enabled():
        return _simulation_response(
            "参数已保存；模拟模式未下发到裸机"
        )

    send_str_to_linux(_motion_params_command(values))
    if config.LOOPBACK_MODE:
        return _loopback_response(
            "参数已保存并下发到模拟裸机，等待回执"
        )
    return jsonify({
        "status": "pending",
        "message": "参数已保存并下发，等待裸机确认",
    })


@app.route('/api/history')
def api_history():
    return jsonify(get_history_list())


@app.route('/api/history/<folder_name>')
def api_history_detail(folder_name):
    return jsonify(get_history_detail(folder_name))


# ============================================================
# CJJ 181-2012 管段评估
# ============================================================
@app.route('/api/evaluation/current')
def api_evaluation_current():
    """当前巡检实时评估（基于 defect_logs 实时数据）。"""
    report = evaluate_defects(config.defect_logs, config.total_mileage)
    report["is_live"] = True
    report["inspect_dir"] = (os.path.basename(config.current_inspect_dir)
                             if config.current_inspect_dir else None)
    return jsonify(report)


@app.route('/api/evaluation/history/<folder_name>')
def api_evaluation_history(folder_name):
    """历史巡检评估：新版存档直读；旧版/无存档从 CSV 按聚类合并现算。"""
    folder_path = os.path.join(config.SAVE_DIR, folder_name)
    if not os.path.isdir(folder_path):
        return jsonify({"status": "error", "message": "目录不存在"}), 404

    report = evaluate_history(folder_path)
    if report:
        report["inspect_dir"] = folder_name
        return jsonify(report)
    return jsonify({"status": "error", "message": "该巡检无缺陷数据"}), 404


# ============================================================
# 巡检管理：年度体检计划 / 管网健康总览 / 标准符合性自查
# ============================================================
@app.route('/api/maintenance/plan')
def api_maintenance_plan():
    """年度体检计划：全部历史管段按等级排修复时限与下次检测日期。"""
    return jsonify(build_maintenance_plan())


@app.route('/api/network/overview')
def api_network_overview():
    """管网健康总览：多趟巡检汇总（每趟一段，按等级着色）。"""
    return jsonify(build_network_overview())


@app.route('/api/compliance/checklist')
def api_compliance_checklist():
    """标准符合性自查表（CJJ 181-2012 / T/CAS 731-2023）。"""
    return jsonify(compliance_checklist())


# ============================================================
# 3D 管道走廊
# ============================================================
@app.route('/api/corridor/current')
def api_corridor_current():
    """当前巡检的走廊实时数据（前端 three.js 1s 轮询）。"""
    return jsonify(corridor_current())


@app.route('/api/corridor/history/<folder_name>')
def api_corridor_history(folder_name):
    """历史巡检走廊回放数据（读落盘的 corridor_index.json）。"""
    data = corridor_history(folder_name)
    if data is None:
        return jsonify({"status": "error", "message": "该巡检没有走廊数据"}), 404
    return jsonify(data)


# ============================================================
# 启动入口
# ============================================================
def start_background_services():
    if config.WEB_ONLY:
        print("Web-only 模式：跳过 YOLO、通信、摄像头、手柄和温度服务。", flush=True)
        return True

    if config.LOOPBACK_MODE:
        print("Loopback 模式：启动 UDP 通信监听和轨迹线程；跳过真实外设服务。", flush=True)
        start_udp_listener()
        start_mapping_thread()
        # 走廊采集（演示遥测由 corridor 模块合成，无需外设）
        start_vision_hub()   # 帧中枢：视觉流/拍照/走廊共用一条流连接
        start_corridor_thread(frame_provider=_vision_grab_frame)
        return True

    if config.PRELOAD_YOLO:
        if not init_yolo():
            print("YOLO preload failed; startup stopped.", flush=True)
            return False

    start_udp_listener()
    start_mapping_thread()
    # start_camera_thread()
    start_gamepad_listener()
    start_temperature_thread()
    # 视觉帧中枢：独占视觉程序的单客户端流，供 /video_feed、拍照、走廊、缺陷记录共用
    start_vision_hub()
    # 缺陷自动记录（数据源: 8080 NPU 检测，快照复用视觉流抓帧）
    start_defect_recorder_thread(snapshot_provider=_vision_grab_frame)
    # 3D 走廊采集（关键帧同样复用视觉流抓帧）
    start_corridor_thread(frame_provider=_vision_grab_frame)
    return True


def run_app():
    print(
        f"🌐 工作站控制台启动！访问: http://{config.WEB_HOST}:{config.WEB_PORT}"
    )
    app.run(host=config.WEB_HOST, port=config.WEB_PORT, threaded=True)


if __name__ == '__main__':
    if start_background_services():
        run_app()
    else:
        raise SystemExit(1)
