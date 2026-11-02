"""
3D 管道走廊数据采集模块
========================
巡检进行中按里程步长（默认 0.25 m，弯道自适应加密）采集断面
（位置 x/y/z + 朝向 yaw/pitch + 超声管径），并**按时间**（默认每 1 s）
从视觉流独立抓取关键帧 JPEG 存入巡检目录 corridor/，供前端 three.js
渲染"实时生长的管道走廊"。几何（断面）与影像（关键帧）解耦：
断面按里程走、关键帧按时间走，前端按里程就近配对贴壁。

关键帧相似度去重（2026-10-17）：抓到的帧先压成 16x16 灰度签名，与上一张
已保留帧比较，"管道轮廓差不多"的重复画面直接丢弃不落盘；保底间隔保证
管壁始终有真实纹理（详见 KF_SIM_THRESHOLD 一组常量）。

设计要点：
- 独立线程 + 自适应生命周期：监听 config.current_inspect_dir 的变化自动
  开始/重置/收尾（巡检结束把索引写盘 corridor_index.json），不侵入
  inspection / motor_control 现有代码。
- 位置积分与 mapping.py 解耦：里程源用 config.total_mileage（增量 d_m），
  平面方向取 yaw、高程取 pitch 正弦积分 —— 即简介承诺的"走向 + 高程"重建。
- 硬件模式数据源 = system_status 真实遥测；非硬件模式（web-only/loopback
  演示）合成平滑演示遥测（蛇形走廊 + 变管径 + 示例缺陷），保证 web 端
  无外设也能演示完整走廊效果。
- 关键帧：硬件模式从 8080 视觉流抓帧（main._vision_grab_frame）；
  抓不到时 image=None，前端用程序化"里程牌"纹理兜底。
"""

import json
import math
import os
import threading
import time

import config

SECTION_STEP_M = float(getattr(config, "CORRIDOR_STEP_M", 0.25))
TURN_TRIGGER_DEG = float(getattr(config, "CORRIDOR_TURN_TRIGGER_DEG", 8.0))
MIN_SECTION_GAP_M = 0.12       # 转弯加密时的最小断面间距（防无限密集）
KEYFRAME_PERIOD_S = float(getattr(config, "CORRIDOR_KEYFRAME_PERIOD_S", 1.0))
LOOP_PERIOD_S = 0.1            # 采集线程周期
FALLBACK_DIAMETER_M = float(getattr(
    config, "CORRIDOR_FALLBACK_DIAMETER_M", 0.55))   # 超声无效时的默认管径 (m)
SONAR_MAX_MM = 2000            # 超声有效上限（与 mapping.py 一致）
# 管径有效性钳制（2026-10-18）：陆上调试超声乱反射，读数可跳到 2m+，
# 越界直接回退默认管径，否则走廊环/管径忽大忽小在原地开花
DIAMETER_MIN_M = float(getattr(config, "CORRIDOR_DIAMETER_MIN_M", 0.50))
DIAMETER_MAX_M = float(getattr(config, "CORRIDOR_DIAMETER_MAX_M", 0.60))

# ---- 固定轨迹（2026-10-27 用户拍板：按实物 L 形演示管锁死走廊几何）----
# 实物管道：总长 ~1.8m = 0.9m 入口直道 + 左转 90° + 转后直道。
# 里程是时间×速度的推算值、遥测 yaw 在演示时不反映真实转弯，
# 自由积分会把 L 形管画成一条 1.8m 大直道（用户截图"直道 1.5m"）。
# 开启后：里程直接投影到锁死轨道，断面位置/朝向、机器人位姿全走轨道。
FIXED_TRACK = bool(getattr(config, "CORRIDOR_FIXED_TRACK", True))
TRACK_S1_M = float(getattr(config, "CORRIDOR_TRACK_S1_M", 0.90))
TRACK_TURN_DEG = float(getattr(config, "CORRIDOR_TRACK_TURN_DEG", 90.0))
TRACK_TURN_R_M = float(getattr(config, "CORRIDOR_TRACK_TURN_R_M", 0.30))


def _track_arc_m():
    return TRACK_TURN_R_M * math.radians(TRACK_TURN_DEG)


def _track_total_m():
    return float(getattr(config, "TRAJECTORY_MAX_M", 1.8))


def _track_pose(m):
    """里程 -> 锁死 L 形轨道的 (x, y, yaw_deg)。入口朝 +x，左转向 +y 弯。"""
    m = max(0.0, min(m, _track_total_m()))
    s1, arc = TRACK_S1_M, _track_arc_m()
    if m <= s1:
        return m, 0.0, 0.0
    if m <= s1 + arc:
        th = (m - s1) / TRACK_TURN_R_M
        return (s1 + TRACK_TURN_R_M * math.sin(th),
                TRACK_TURN_R_M * (1.0 - math.cos(th)),
                math.degrees(th))
    return (s1 + TRACK_TURN_R_M,
            TRACK_TURN_R_M + (m - s1 - arc),
            TRACK_TURN_DEG)


def _track_info():
    """随 API 下发给前端，corridor.js 用同一套参数精确还原轨道。"""
    return {
        "fixed": True,
        "straight1_m": TRACK_S1_M,
        "turn_deg": TRACK_TURN_DEG,
        "turn_radius_m": TRACK_TURN_R_M,
        "total_m": _track_total_m(),
    }


# 演示管 5 类缺陷贴墙方位（2026-10-27 用户拍板，按实拍画面定）：
#   左壁：裂缝、破洞；右壁：泥沙淤积、管壁脱落；顶部：树根侵入。
# 里程不在这里编 —— 实物缺陷全在入口前 60cm，日志里程本身就是实测值
# （2026-10-27 实测：脱落 0.32 / 树根 0.36 / 泥沙 0.39 / 裂缝 0.43 / 破洞 0.62）。
DEFECT_WALL_SIDE = {
    "裂缝": "left",
    "破洞": "left",
    "泥沙淤积": "right",
    "管壁脱落": "right",
    "树根侵入": "top",
}
DEFECT_ZONE_MAX_M = float(getattr(config, "CORRIDOR_DEFECT_ZONE_M", 0.68))
# 同侧照片最小轴向间距（m）：几类缺陷挤在 30cm 内，不排开就会叠成一张
DEFECT_MIN_GAP_M = float(getattr(config, "CORRIDOR_DEFECT_GAP_M", 0.18))


def _apply_defect_slots(items):
    """给缺陷挂上贴墙方位 + 展示里程。

    - 方位：由类型决定（左/右/顶），与里程无关；
    - 里程：用缺陷日志里的真实里程（实测都在前 60cm，超出 0.68m 钳回）；
    - 同侧冲突：同侧按里程排序后强制 > DEFECT_MIN_GAP_M，只往后微推。
    """
    by_side = {}
    for it in items:
        side = DEFECT_WALL_SIDE.get(it.get("type"))
        if not side:
            continue
        it["wall_side"] = side
        m = float(it.get("mileage") or 0.0)
        it["display_mileage"] = round(max(0.05, min(m, DEFECT_ZONE_MAX_M)), 2)
        by_side.setdefault(side, []).append(it)
    for arr in by_side.values():
        arr.sort(key=lambda x: x["display_mileage"])
        for i in range(1, len(arr)):
            prev = arr[i - 1]["display_mileage"]
            if arr[i]["display_mileage"] - prev < DEFECT_MIN_GAP_M:
                arr[i]["display_mileage"] = round(prev + DEFECT_MIN_GAP_M, 2)

# ---- 关键帧相似度去重（2026-10-17：滤掉画面几乎没变的帧）----
# 抓到的帧先解码成 16x16 灰度签名，与上一张"已保留"帧做平均绝对差：
#   差 < KF_SIM_THRESHOLD 且未到强制保留间隔 -> 判定为重复画面，不落盘
#   （直管段 1s 一帧 × 0.15m/s = 0.15m 一帧，轮廓几乎一样，全存只会让
#    管壁贴牌密密麻麻、贴壁照片重复，还白占 SD 卡）
# 保底约束（按 1.6m 演示管校准 + 管壁贴图覆盖需求，2026-10-18）：
#   KF_MIN_GAP_M   距上一保留帧的最小里程（防爆发连拍）
#   KF_FORCE_GAP_M 里程上最多隔这么远必须保留一帧 —— 同时是管壁贴图的
#                  覆盖粒度：每 0.5m 必有真实帧，贴图无裸奔区
#   KF_FORCE_GAP_S 时间上最多隔这么久必须保留一帧（原地转镜头时也有变化）
KF_SIM_THRESHOLD = float(getattr(config, "CORRIDOR_KF_SIM_THRESHOLD", 8.0))
KF_MIN_GAP_M = float(getattr(config, "CORRIDOR_KF_MIN_GAP_M", 0.15))
KF_FORCE_GAP_M = float(getattr(config, "CORRIDOR_KF_FORCE_GAP_M", 0.5))
KF_FORCE_GAP_S = float(getattr(config, "CORRIDOR_KF_FORCE_GAP_S", 15.0))

# 演示模式的示例缺陷（非硬件模式下由 API 返回，标注 demo=True）
DEMO_DEFECTS = [
    {"mileage": 12.0, "type": "裂缝", "severity_code": 2, "confidence": 0.83},
    {"mileage": 30.0, "type": "泥沙淤积", "severity_code": 2, "confidence": 0.77},
    {"mileage": 48.0, "type": "破洞", "severity_code": 3, "confidence": 0.91},
    {"mileage": 66.0, "type": "树根侵入", "severity_code": 3, "confidence": 0.69},
]

_state = {
    "started": False,
    "thread": None,
    "kf_thread": None,        # 独立抓帧线程（抓帧耗时 0.2~3s，不能阻塞里程积分）
    "kf_wake": threading.Event(),
    "frame_provider": None,   # 抓帧回调，main 启动时注入
    "frame_retry_at": 0.0,    # 抓帧失败后的冷却时间点（monotonic 秒）
    "inspect_dir": "",        # 当前巡检目录（用于变化检测）
    "saved_for_dir": "",      # 已落盘索引对应的目录（防重复写）
    "sections": [],           # 断面列表（当前巡检）
    "keyframes": [],          # 按时间抓的视觉关键帧 [{seq,t,mileage,image}]
    "kf_seq": 0,              # 关键帧序号（文件名用）
    "next_kf_at": 0.0,        # 下一次抓帧时刻（monotonic 秒；0=立即抓第一帧）
    "kf_sig": None,           # 上一张已保留关键帧的灰度签名（相似度去重用）
    "kf_last_m": None,        # 上一张已保留关键帧的里程 (m)
    "kf_last_t": 0.0,         # 上一张已保留关键帧的时间戳（wall 秒）
    "kf_dup_skipped": 0,      # 被去重滤掉的帧数（日志用）
    "next_m": SECTION_STEP_M, # 下一个按步长采样的里程
    "x": 0.0, "y": 0.0, "z": 0.0,   # 航位推算位置（z=高程, m）
    "last_mileage": 0.0,
    "yaw0": None,             # 巡检起点航向（固定轨道模式下换算潜航器相对转向角）
    "lock": threading.Lock(),
}

FRAME_RETRY_COOLDOWN_S = 2.0    # 视觉流不可用时，抓帧失败的重试冷却

# SD 容量保护（双保险）
MAX_KF_PER_INSPECT = int(getattr(config, "CORRIDOR_MAX_KF_PER_INSPECT", 300))  # 单条巡检最多保留帧数
MAX_TOTAL_KF_BYTES = int(getattr(config, "CORRIDOR_MAX_TOTAL_KF_BYTES", 1024 * 1024 * 1024))  # 全局走廊图片总上限 ~1GB
_KF_GC_CHECK_EVERY = 20          # 每抓 N 帧做一次容量检查（轻量）


def _corridor_dir_of(inspect_dir):
    """返回该巡检的 corridor/ 目录路径。"""
    return os.path.join(inspect_dir, "corridor") if inspect_dir else ""


def _kf_filepath(inspect_dir, seq):
    return os.path.join(_corridor_dir_of(inspect_dir), f"kf_{seq:04d}.jpg")


def _prune_current_inspect():
    """保险 ①：当前巡检关键帧数 > MAX_KF_PER_INSPECT 时删最旧的。
    删文件同时从 _state['keyframes'] 移除对应条目（保持索引与盘上一致）。
    """
    inspect_dir = _state.get("inspect_dir", "")
    if not inspect_dir:
        return
    with _state["lock"]:
        kfs = _state["keyframes"]
        if len(kfs) <= MAX_KF_PER_INSPECT:
            return
        removed = 0
        while len(kfs) > MAX_KF_PER_INSPECT:
            kf = kfs.pop(0)          # 最旧在前
            url = kf.get("image") if kf else None
            if not url:
                continue
            # url 形如 /images/<folder>/corridor/kf_XXXX.jpg
            fname = os.path.basename(url)
            path = os.path.join(_corridor_dir_of(inspect_dir), fname)
            try:
                if os.path.isfile(path):
                    os.remove(path)
                removed += 1
            except OSError:
                pass
        if removed:
            print(f"[CORRIDOR] 保险①：当前巡检超过 {MAX_KF_PER_INSPECT} 帧，"
                  f"删除最旧 {removed} 帧", flush=True)


def _prune_global_storage():
    """保险 ②：所有巡检目录的 corridor/ 总占用超 MAX_TOTAL_KF_BYTES 时，
    整条删除最早的巡检目录（连断面索引一起）。
    """
    save_root = getattr(config, "SAVE_DIR", None)
    if not save_root or not os.path.isdir(save_root):
        return
    try:
        # 收集所有 Inspect_* 目录及其 corridor 子目录大小
        entries = []
        for name in os.listdir(save_root):
            full = os.path.join(save_root, name)
            if not os.path.isdir(full) or not name.startswith("Inspect_"):
                continue
            corr = os.path.join(full, "corridor")
            size = 0
            if os.path.isdir(corr):
                for f in os.listdir(corr):
                    try:
                        size += os.path.getsize(os.path.join(corr, f))
                    except OSError:
                        pass
            mtime = os.path.getmtime(full)
            entries.append((mtime, name, full, size))
        if not entries:
            return
        total = sum(e[3] for e in entries)
        if total <= MAX_TOTAL_KF_BYTES:
            return
        entries.sort(key=lambda x: x[0])      # 最旧在前
        removed_bytes = 0
        for mtime, name, full, size in entries:
            if total <= MAX_TOTAL_KF_BYTES:
                break
            # 正在进行的巡检不删（避免删当前正在写的数据）
            if full == _state.get("inspect_dir", ""):
                continue
            try:
                import shutil
                shutil.rmtree(full, ignore_errors=True)
                total -= size
                removed_bytes += size
                print(f"[CORRIDOR] 保险②：删除最旧巡检 {name} "
                      f"(释放 ~{size // 1024 // 1024}MB)", flush=True)
            except OSError as e:
                print(f"[CORRIDOR] 删除 {name} 失败: {e}", flush=True)
                break
        if removed_bytes:
            print(f"[CORRIDOR] 保险②：全局释放 ~{removed_bytes // 1024 // 1024}MB",
                  flush=True)
    except OSError:
        pass


def _kf_gc():
    """容量双保险：当前巡检帧数 + 全局占用。"""
    _prune_current_inspect()
    _prune_global_storage()


# ----------------------------------------------------------------------
# 数据源
# ----------------------------------------------------------------------
def _telemetry():
    """返回 (yaw_deg, pitch_deg, sonar_left_mm, sonar_right_mm)。

    硬件模式取真实遥测；其余模式（本地/演示）合成蛇形走廊 + 变管径，
    保证 web 端在无外设时也能完整演示走廊效果。
    """
    if config.runtime_mode() == "hardware":
        s = config.system_status
        return (s.get("yaw", 0.0), s.get("pitch", 0.0),
                s.get("sonar_left", 0), s.get("sonar_right", 0))
    m = config.total_mileage
    # 短管演示工况（2~3m 管道）：弯道/管径起伏波长按米级设计
    yaw = 30.0 * math.sin(1.05 * m) + 10.0 * math.sin(0.4 * m)   # 6m 波长弯道 + 缓偏
    pitch = 4.0 * math.sin(0.5 * m) - 0.5                         # 缓起伏（高程感）
    sl = 360.0 + 60.0 * math.sin(0.8 * m)
    sr = 440.0 + 60.0 * math.cos(0.6 * m)
    return (yaw, pitch, sl, sr)


def _diameter_m(sl, sr):
    """由左右超声推算管径 (m)；无效或越界时返回默认值。"""
    if 0 < sl < SONAR_MAX_MM and 0 < sr < SONAR_MAX_MM:
        d = (sl + sr) / 1000.0
        if DIAMETER_MIN_M <= d <= DIAMETER_MAX_M:
            return round(d, 3)
    return FALLBACK_DIAMETER_M


# ----------------------------------------------------------------------
# 生命周期
# ----------------------------------------------------------------------
def _reset_for_new_inspection(inspect_dir):
    with _state["lock"]:
        _state["inspect_dir"] = inspect_dir
        _state["sections"] = []
        _state["keyframes"] = []
        _state["kf_seq"] = 0
        _state["next_kf_at"] = 0.0
        _state["kf_sig"] = None
        _state["kf_last_m"] = None
        _state["kf_last_t"] = 0.0
        _state["kf_dup_skipped"] = 0
        _state["next_m"] = SECTION_STEP_M
        _state["x"] = _state["y"] = _state["z"] = 0.0
        _state["last_mileage"] = 0.0
        _state["yaw0"] = None
    _state["saved_for_dir"] = ""
    if inspect_dir:
        os.makedirs(os.path.join(inspect_dir, "corridor"), exist_ok=True)
        # 新巡检开始前先做一次全局清理（保旧巡检不占满 SD）
        _prune_global_storage()
        print("[CORRIDOR] 新巡检，走廊数据已重置", flush=True)


def _save_index():
    """巡检结束时把断面索引 + 缺陷落盘 corridor_index.json。"""
    inspect_dir = _state["inspect_dir"]
    with _state["lock"]:
        sections = list(_state["sections"])
        keyframes = list(_state["keyframes"])
    if not inspect_dir or not sections:
        return
    folder = os.path.basename(inspect_dir)
    index = {
        "folder": folder,
        "total_mileage": round(config.total_mileage, 2),
        "sections": sections,
        "keyframes": keyframes,
        "defects": _defects_payload(),
        "track": _track_info() if FIXED_TRACK else {"fixed": False},
    }
    path = os.path.join(inspect_dir, "corridor", "corridor_index.json")
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(index, f, ensure_ascii=False)
        print(f"[CORRIDOR] 走廊索引已保存: {path}（{len(sections)} 个断面）",
              flush=True)
    except OSError as e:
        print(f"[CORRIDOR] 索引保存失败: {e}", flush=True)


def _frame_signature(jpeg_bytes):
    """把 JPEG 帧压成 16x16 灰度签名（numpy int16 矩阵），用于相似度比较。

    cv2/numpy 不可用（如本地无 opencv 的测试环境）时返回 None ——
    调用方据此自动降级为"仅按最小里程间隔去重"。
    """
    try:
        import cv2
        import numpy as np
        arr = np.frombuffer(jpeg_bytes, dtype=np.uint8)
        img = cv2.imdecode(arr, cv2.IMREAD_GRAYSCALE)
        if img is None:
            return None
        return cv2.resize(img, (16, 16)).astype(np.int16)
    except Exception:
        return None


def _sig_distance(sig_a, sig_b):
    """两张签名的平均绝对差（0~255）。"""
    if sig_a is None or sig_b is None:
        return None
    try:
        import numpy as np
        return float(np.mean(np.abs(sig_a.astype(np.int32) - sig_b.astype(np.int32))))
    except Exception:
        return None


def _grab_frame_bytes():
    """抓一帧 JPEG 字节；失败返回 None 并进入冷却期。

    注意：只抓不写盘 —— 是否落盘由 _kf_loop 的相似度门控决定，
    重复画面直接丢弃，不占 SD 卡。
    """
    if time.monotonic() < _state.get("frame_retry_at", 0.0):
        return None
    provider = _state.get("frame_provider")
    if not provider:
        return None
    try:
        frame = provider()
        if not frame:
            _state["frame_retry_at"] = time.monotonic() + FRAME_RETRY_COOLDOWN_S
            return None
        return frame
    except Exception:
        _state["frame_retry_at"] = time.monotonic() + FRAME_RETRY_COOLDOWN_S
        return None


# ----------------------------------------------------------------------
# 采集线程
# ----------------------------------------------------------------------
def _corridor_loop():
    while True:
        try:
            inspect_dir = config.current_inspect_dir
            running = bool(config.system_status.get("is_running"))
            prev_dir = _state["inspect_dir"]

            # 收尾 1：巡检目录被清空（motor_stop -> end_inspection）
            if prev_dir and not inspect_dir:
                _save_index()
                with _state["lock"]:
                    _state["inspect_dir"] = ""
            # 收尾 2：系统已停止但目录还在（web "关闭"按钮 -> /api/control/stop）
            elif (prev_dir and not running
                  and _state.get("saved_for_dir") != prev_dir
                  and _state["sections"]):
                _save_index()
                _state["saved_for_dir"] = prev_dir
            # 新巡检：重置
            elif inspect_dir and inspect_dir != prev_dir:
                _reset_for_new_inspection(inspect_dir)

            # 巡检进行中：积分位置 + 按里程步长/转弯自适应采断面
            if (inspect_dir and config.system_status.get("is_running")
                    and config.system_status.get("is_moving")):
                yaw, pitch, sl, sr = _telemetry()
                with _state["lock"]:
                    d_m = config.total_mileage - _state["last_mileage"]
                    _state["last_mileage"] = config.total_mileage
                    if d_m > 0:
                        if FIXED_TRACK:
                            # 锁死轨道只决定"管道长在哪"（实物管形是已知常量）；
                            # 潜航器自己的转向角仍来自实时 yaw：
                            # 记起点航向，之后 "yaw-yaw0" 就是它相对入口转了多少。
                            if _state.get("yaw0") is None:
                                _state["yaw0"] = yaw
                            tx, ty, tyaw = _track_pose(config.total_mileage)
                            _state["x"], _state["y"], _state["z"] = tx, ty, 0.0
                            yaw, pitch = tyaw, 0.0
                        else:
                            yaw_rad = math.radians(yaw)
                            _state["x"] += d_m * math.cos(yaw_rad)
                            _state["y"] += d_m * math.sin(yaw_rad)
                            _state["z"] += d_m * math.sin(math.radians(pitch))

                    # 触发条件：到达下一个步长里程；或航向/管径相对上一断面
                    # 变化超阈值（弯道自适应加密）；均受最小间距约束
                    due = config.total_mileage >= _state["next_m"]
                    if not due and _state["sections"]:
                        last = _state["sections"][-1]
                        moved = config.total_mileage - last["mileage"]
                        if moved >= MIN_SECTION_GAP_M:
                            yaw_diff = abs((yaw - last["yaw"] + 180.0) % 360.0 - 180.0)
                            diam_mm = (sl + sr) / 2.0
                            last_diam_mm = (last["diameter"] or FALLBACK_DIAMETER_M) * 1000.0
                            if yaw_diff >= TURN_TRIGGER_DEG:
                                due = True
                            elif 0 < last_diam_mm < SONAR_MAX_MM and \
                                    abs(diam_mm - last_diam_mm) >= last_diam_mm * 0.15:
                                due = True

                    if due:
                        km = len(_state["sections"]) + 1
                        section = {
                            "km": km,
                            "mileage": round(config.total_mileage, 2),
                            "x": round(_state["x"], 3),
                            "y": round(_state["y"], 3),
                            "z": round(_state["z"], 3),
                            "yaw": round(yaw, 1),
                            "pitch": round(pitch, 1),
                            "diameter": _diameter_m(sl, sr),
                        }
                        _state["sections"].append(section)
                        _state["next_m"] = config.total_mileage + SECTION_STEP_M

                # 视觉关键帧：按时间触发独立抓帧线程（与断面里程解耦）。
                # 抓帧在线程里执行，这里只 set 事件，绝不阻塞里程积分。
                now = time.monotonic()
                if now >= _state["next_kf_at"]:
                    _state["next_kf_at"] = now + KEYFRAME_PERIOD_S
                    _state["kf_wake"].set()

            time.sleep(LOOP_PERIOD_S)
        except Exception as e:
            print(f"[CORRIDOR] 采集线程异常: {e}", flush=True)
            time.sleep(1.0)


def _kf_loop():
    """独立抓帧线程：主循环每 KEYFRAME_PERIOD_S 唤醒一次，本线程负责
    实际抓帧 + 相似度去重 + 落盘 + 登记（抓帧耗时 0.2~3s，不能阻塞
    0.1s 周期的里程积分）。

    去重规则（2026-10-17，滤掉"管道轮廓差不多"的重复画面）：
      1. 距上一保留帧 < KF_MIN_GAP_M           -> 丢弃（防连拍）
      2. 与上一保留帧签名差 < KF_SIM_THRESHOLD
         且未到里程/时间强制保留间隔            -> 丢弃（画面几乎没变）
      3. 其余保留。cv2 不可用时规则 2 自动失效，仅按规则 1/3 走。
    """
    while True:
        if not _state["kf_wake"].wait(timeout=1.0):
            continue
        _state["kf_wake"].clear()
        inspect_dir = _state["inspect_dir"]
        if not inspect_dir or not config.system_status.get("is_moving"):
            continue
        frame = _grab_frame_bytes()
        if not frame:
            continue
        m = config.total_mileage
        now = time.time()

        # ---- 相似度门控（决定要不要这张帧）----
        sig = _frame_signature(frame)
        with _state["lock"]:
            keep = True
            last_m = _state.get("kf_last_m")
            if last_m is not None:
                if m - last_m < KF_MIN_GAP_M:
                    keep = False
                else:
                    dist = _sig_distance(sig, _state.get("kf_sig"))
                    forced = (m - last_m >= KF_FORCE_GAP_M
                              or now - _state.get("kf_last_t", 0.0) >= KF_FORCE_GAP_S)
                    if dist is not None and dist < KF_SIM_THRESHOLD and not forced:
                        keep = False
            if not keep:
                _state["kf_dup_skipped"] = _state.get("kf_dup_skipped", 0) + 1
                skipped = _state["kf_dup_skipped"]
                if skipped % 10 == 1:
                    print(f"[CORRIDOR] 相似度去重：已滤掉 {skipped} 张重复帧",
                          flush=True)
                continue
            seq = _state["kf_seq"] + 1

        # ---- 落盘 + 登记（写盘 IO 放锁外，单写线程无竞争）----
        filename = f"kf_{seq:04d}.jpg"
        try:
            with open(os.path.join(inspect_dir, "corridor", filename), "wb") as f:
                f.write(frame)
        except OSError as e:
            print(f"[CORRIDOR] 关键帧写盘失败: {e}", flush=True)
            continue
        url = f"/images/{os.path.basename(inspect_dir)}/corridor/{filename}"
        with _state["lock"]:
            _state["kf_seq"] = seq
            _state["kf_sig"] = sig
            _state["kf_last_m"] = m
            _state["kf_last_t"] = now
            _state["keyframes"].append({
                "seq": seq,
                "t": round(now, 1),
                "mileage": round(m, 2),
                "image": url,
            })
        # 容量双保险：每 N 帧检查一次（轻量，不阻塞抓帧节奏）
        if seq % _KF_GC_CHECK_EVERY == 0:
            _kf_gc()


def start_corridor_thread(frame_provider=None):
    """启动走廊采集守护线程；frame_provider 为视觉流抓帧回调。"""
    if _state["started"]:
        if frame_provider:
            _state["frame_provider"] = frame_provider
        return
    _state["started"] = True
    _state["frame_provider"] = frame_provider
    t = threading.Thread(target=_corridor_loop, daemon=True)
    t.start()
    _state["kf_thread"] = threading.Thread(target=_kf_loop, daemon=True)
    _state["kf_thread"].start()
    print("[CORRIDOR] 走廊采集线程已启动（断面 0.25m · 关键帧每 %.1fs 一帧·独立线程）"
          % KEYFRAME_PERIOD_S, flush=True)
    return t


# ----------------------------------------------------------------------
# API 数据组装
# ----------------------------------------------------------------------
def _defects_payload():
    """缺陷列表：硬件模式用真实 defect_logs；演示模式返回示例缺陷。"""
    if config.runtime_mode() == "hardware":
        payload = []
        for d in config.defect_logs:
            item = {
                "mileage": d.get("mileage", 0.0),
                "type": d.get("type", ""),
                "severity_code": d.get("severity_code", 1),
                "confidence": d.get("confidence", 0.0),
                "image": d.get("image"),
                "bbox": d.get("bbox"),   # 检测框（模型 256 系），前端投影贴壁用
            }
            # 2026-10-27：贴墙方位按实拍画面锁死（左壁裂缝/破洞、右壁泥沙/
            # 脱落、顶部树根），里程用缺陷日志真实里程（都在入口前 60cm），
            # 前端按 wall_side + display_mileage 精确贴壁，不走针孔投影
            # （贴片会歪/飘出管外）。同侧过近的照片由 _apply_defect_slots 排开。
            payload.append(item)
        _apply_defect_slots(payload)
        return payload
    payload = []
    m = config.total_mileage
    for d in DEMO_DEFECTS:
        item = dict(d)
        item["demo"] = True
        item["visible"] = m >= d["mileage"]
        payload.append(item)
    return payload


def corridor_current():
    """当前巡检的走廊实时数据（前端 1s 轮询）。"""
    with _state["lock"]:
        sections = [dict(s) for s in _state["sections"]]
        keyframes = list(_state["keyframes"])
        pose = {
            "x": _state["x"], "y": _state["y"], "z": _state["z"],
            "next_m": _state["next_m"],
        }
    yaw, pitch, _, _ = _telemetry()
    pose["yaw"] = yaw
    pose["pitch"] = pitch
    if FIXED_TRACK:
        # 锁死轨道：位置 = 轨道投影（管子里跑，不飘出管外）；
        # 车头朝向 = 实时 yaw 相对起点航向的转角 —— 转没转弯在车上直接看得见。
        px, py, pyaw = _track_pose(config.total_mileage)
        pose.update({"x": px, "y": py, "z": 0.0, "pitch": pitch})
        yaw0 = _state.get("yaw0")
        if yaw0 is not None:
            rel = (yaw - yaw0 + 180.0) % 360.0 - 180.0   # 相对入口转向角
            pose["yaw"] = round(rel, 1)
        else:
            pose["yaw"] = pyaw
    return {
        "folder": (os.path.basename(config.current_inspect_dir)
                   if config.current_inspect_dir else None),
        "running": bool(config.system_status.get("is_running")),
        "moving": bool(config.system_status.get("is_moving")),
        "total_mileage": round(config.total_mileage, 2),
        "demo_mode": config.runtime_mode() != "hardware",
        "sections": sections,
        "keyframes": keyframes,
        "pose": pose,
        "defects": _defects_payload(),
        "track": _track_info() if FIXED_TRACK else {"fixed": False},
    }


def corridor_history(folder_name):
    """历史巡检的走廊数据（读落盘索引）。"""
    folder_path = os.path.join(config.SAVE_DIR, folder_name)
    index_path = os.path.join(folder_path, "corridor", "corridor_index.json")
    if not os.path.isfile(index_path):
        return None
    try:
        with open(index_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None
