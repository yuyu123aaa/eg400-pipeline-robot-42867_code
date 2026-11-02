"""
缺陷自动记录线程
================
轮询 8080 NPU 视觉服务的 /api/detections，把真实检测结果
自动写入 config.defect_logs + 巡检目录 CSV（拍照留档）。

快照策略（对齐参考实现"检测即存图"）：
- 某一轮询周期内出现"新"缺陷（同类 0.2m 内未记过、且每类 <3 张）时，
  抓当前帧 → 后端用 PIL 把**全部**检测框 + 中文标签画上去 → 落盘。
- 画框在后端完成，与浏览器是否打开、停在哪个页面无关；
  中文字体位于 app/assets/defect_cn.ttf（部署时随包上传）。
- 演示管道：5 类缺陷仅放置在前 60cm，里程 >0.65m 后不记录（避免误检）。
"""

import json
import os
import threading
import time
import urllib.request
from datetime import datetime

import config
from core.evaluation import grade_defect, SEVERITY_TEXT, ADVICE_TEXT

VISION_API = "http://127.0.0.1:8080/api/detections"
POLL_INTERVAL_S = 0.6          # 轮询周期
# 空间去重：每类缺陷一次巡检只记录一张（演示管道 5 类样本各 1 张，
# 波纹管壁的重复误检不落盘）。多记的流水由评估层聚类合并兜底。
# 注：不再按里程设记录区——里程为时间×速度的推算值，停留讲解时
# 推进器仍在转、里程持续虚涨，里程闸会误拦真实检测（如停在孔洞前）。
DEDUP_MILEAGE_M = 0.2
MAX_PER_TYPE = 1

_state = {
    "last_seen": {},   # chinese -> (mileage, monotonic_ts)
    "count": {},       # chinese -> 本次巡检已存张数
    "started": False,
}

# 中文字体：优先用随包上传的 defect_cn.ttf（app/assets/）
_FONT_CACHE = {}


def _load_font(size):
    if size in _FONT_CACHE:
        return _FONT_CACHE[size]
    font = None
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # app/
    candidates = (
        os.path.join(base, "assets", "defect_cn.ttf"),
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    )
    for fp in candidates:
        try:
            from PIL import ImageFont
            font = ImageFont.truetype(fp, size)
            break
        except Exception:
            continue
    _FONT_CACHE[size] = font
    return font


def _fetch_detections():
    """拉取 8080 检测结果，失败返回 None。"""
    try:
        req = urllib.request.Request(
            VISION_API, headers={"Cache-Control": "no-store"})
        with urllib.request.urlopen(req, timeout=3) as resp:
            return json.loads(resp.read().decode("utf-8", "replace"))
    except Exception:
        return None


def _is_duplicate(chinese):
    """同类缺陷去重：0.2m 里程内不重复；每类一次巡检最多 3 张。"""
    if _state["count"].get(chinese, 0) >= MAX_PER_TYPE:
        return True
    last = _state["last_seen"].get(chinese)
    if last is None:
        return False
    last_mileage, _ = last
    return abs(float(config.total_mileage or 0) - last_mileage) < DEDUP_MILEAGE_M


def _annotate_frame(frame_bytes, objects, mw=256.0, mh=256.0):
    """把全部检测框 + 中文标签画到帧上（与前端实时画面同一套配色与文字）。"""
    if not objects:
        return frame_bytes
    try:
        import io as _io
        from PIL import Image, ImageDraw
    except Exception:
        return frame_bytes
    try:
        img = Image.open(_io.BytesIO(frame_bytes)).convert("RGB")
        draw = ImageDraw.Draw(img)
        W, H = img.size
        font = _load_font(max(18, round(H / 28)))
        lw = max(4, round(min(W, H) / 160))
        # 配色与前端 VISION_CLASSES 一致：0剥落蓝 1裂纹红 2孔洞紫 3树根绿 4沉积橙
        palette = ["#1e6fff", "#e84b5f", "#865be7", "#10a878", "#f09a39"]
        sx = W / float(mw or 256)
        sy = H / float(mh or 256)

        for obj in objects:
            try:
                ox = float(obj.get("x", 0)); oy = float(obj.get("y", 0))
                ow = float(obj.get("w", 0)); oh = float(obj.get("h", 0))
                label = int(obj.get("label", -1))
                prob = float(obj.get("prob", 0) or 0)
            except (TypeError, ValueError):
                continue
            en = config.YOLO_CLASS_NAMES.get(label, str(label))
            cn = config.DEFECT_TYPE_ALIASES.get(en, en)
            rgb = tuple(int(palette[label % 5][i:i+2], 16) for i in (1, 3, 5))
            x1 = (ox - ow / 2) * sx
            y1 = (oy - oh / 2) * sy
            x2 = (ox + ow / 2) * sx
            y2 = (oy + oh / 2) * sy
            draw.rectangle([x1, y1, x2, y2], outline=rgb, width=lw)
            if font:
                txt = f"{cn} {prob*100:.0f}%"
                tb = draw.textbbox((0, 0), txt, font=font)
                tw_, th_ = tb[2] - tb[0], tb[3] - tb[1]
                ty = y1 - th_ - 12 if y1 >= th_ + 18 else y2 + 6
                draw.rectangle([x1, ty, x1 + tw_ + 14, ty + th_ + 12], fill=rgb)
                draw.text((x1 + 7, ty + 6 - tb[1]), txt,
                          font=font, fill=(255, 255, 255))

        buf = _io.BytesIO()
        img.save(buf, format="JPEG", quality=90)
        return buf.getvalue()
    except Exception:
        return frame_bytes


def _write_annotated(frame_bytes, inspect_dir, obj, mw=256.0, mh=256.0, seq=1):
    """只画这一条缺陷的框（同帧其他目标不标）→ 落盘 defects/，返回相对路径。

    seq 用于同一轮询周期记多条时的文件名去重（_2、_3 后缀）。
    """
    try:
        frame = _annotate_frame(frame_bytes, [obj], mw, mh)
        d = os.path.join(inspect_dir, "defects")
        os.makedirs(d, exist_ok=True)
        suffix = "" if seq <= 1 else f"_{seq}"
        filename = time.strftime("defect_%Y%m%d_%H%M%S") + suffix + ".jpg"
        with open(os.path.join(d, filename), "wb") as f:
            f.write(frame)
        return f"defects/{filename}"
    except Exception:
        return None


def _record(obj, payload, image_rel=None):
    """把单个检测目标落盘：defect_logs + CSV（快照由调用方统一抓取）。"""
    label = config.YOLO_CLASS_NAMES.get(int(obj.get("label", -1)),
                                        str(obj.get("label")))
    chinese = config.DEFECT_TYPE_ALIASES.get(label, label)
    confidence = float(obj.get("prob", 0) or 0)

    mw = float(payload.get("model_width") or 0)
    mh = float(payload.get("model_height") or 0)
    w = float(obj.get("w") or 0)
    h = float(obj.get("h") or 0)
    size_ratio = (w * h / (mw * mh)) if (mw > 0 and mh > 0 and w > 0 and h > 0) else 0.0

    grade = grade_defect(chinese, confidence, size_ratio)
    mileage = round(float(config.total_mileage or 0), 2)
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    log = {
        "type": chinese,
        "severity": SEVERITY_TEXT[grade],
        "severity_code": grade,
        "confidence": round(confidence, 2),
        "advice": ADVICE_TEXT[grade],
        "mileage": mileage,
        "time": ts,
        "image": (f"/images/{os.path.basename(config.current_inspect_dir)}/{image_rel}"
                  if image_rel and config.current_inspect_dir else None),
        "grade": grade,
        "size_ratio": round(size_ratio, 4),
        # 检测框（模型 256x256 中心系坐标），3D 走廊按此投影缺陷贴壁位置
        "bbox": {
            "x": round(float(obj.get("x", 0) or 0), 1),
            "y": round(float(obj.get("y", 0) or 0), 1),
            "w": round(float(obj.get("w", 0) or 0), 1),
            "h": round(float(obj.get("h", 0) or 0), 1),
        },
    }
    config.defect_logs.append(log)
    _state["last_seen"][chinese] = (float(config.total_mileage or 0), time.monotonic())
    _state["count"][chinese] = _state["count"].get(chinese, 0) + 1

    # 追加 CSV（与历史格式兼容，多两列：缺陷等级 / 画面占比）
    try:
        import csv as csv_module
        csv_path = os.path.join(config.current_inspect_dir, "巡检缺陷报告.csv")
        file_exists = os.path.isfile(csv_path)
        with open(csv_path, mode="a", newline="", encoding="utf-8-sig") as f:
            writer = csv_module.writer(f)
            if not file_exists:
                writer.writerow(["记录时间", "发生里程(m)", "病害类型", "严重等级",
                                 "识别置信度", "处置建议", "关联图片文件",
                                 "缺陷等级(1-4)", "画面占比"])
            writer.writerow([ts, mileage, chinese, log["severity"],
                             log["confidence"], log["advice"],
                             image_rel or "", grade, log["size_ratio"]])
    except Exception as e:
        print(f"⚠️ 缺陷 CSV 写入失败: {e}")

    print(f"🚨 自动记录缺陷: {chinese} | {log['severity']}({grade}级) | "
          f"里程 {mileage}m | 置信 {log['confidence']} | 图片 {image_rel}", flush=True)


def reset_state():
    """新巡检开始时清空去重状态（由 inspection.begin_inspection 调用）。"""
    _state["last_seen"] = {}
    _state["count"] = {}


def set_recording(enabled):
    """开关缺陷自动记录（web🤖按钮 / 手柄Y键 调用）。返回当前状态。"""
    config.DEFECT_RECORDING = bool(enabled)
    config.system_status["defect_recording"] = bool(enabled)
    print(f"[RECORDER] 缺陷自动记录: {'ON' if enabled else 'OFF'}", flush=True)
    return config.DEFECT_RECORDING


def _recorder_loop():
    while True:
        try:
            # 仅在记录开关开启 + 巡检进行中记录（里程闸已移除：
            # 时间推算里程在停留时会虚涨，误拦真实检测）
            if (config.DEFECT_RECORDING
                    and config.current_inspect_dir
                    and config.system_status.get("is_running")):
                payload = _fetch_detections()
                if payload and payload.get("ready"):
                    objs = payload.get("objects") or []
                    new_pairs = []
                    for obj in objs:
                        label = obj.get("label")
                        if label is None:
                            continue
                        en = config.YOLO_CLASS_NAMES.get(int(label), str(label))
                        chinese = config.DEFECT_TYPE_ALIASES.get(en, en)
                        if not _is_duplicate(chinese):
                            new_pairs.append((obj, chinese))
                    if new_pairs:
                        # 抓一帧原始画面，每个新缺陷各出一张图、只画自己的框
                        raw = None
                        provider = _state.get("snapshot_provider")
                        if provider:
                            try:
                                raw = provider()
                            except Exception:
                                raw = None
                        mw = float(payload.get("model_width") or 256)
                        mh = float(payload.get("model_height") or 256)
                        seq = 0
                        for obj, chinese in new_pairs:
                            image_rel = None
                            if raw:
                                seq += 1
                                image_rel = _write_annotated(
                                    raw, config.current_inspect_dir,
                                    obj, mw, mh, seq)
                            _record(obj, payload, image_rel)
            time.sleep(POLL_INTERVAL_S)
        except Exception as e:
            print(f"⚠️ 缺陷记录线程异常: {e}")
            time.sleep(2.0)


def start_defect_recorder_thread(snapshot_provider=None):
    """启动缺陷自动记录守护线程；snapshot_provider 为抓帧回调。"""
    if _state["started"]:
        return
    _state["started"] = True
    _state["snapshot_provider"] = snapshot_provider
    t = threading.Thread(target=_recorder_loop, daemon=True)
    t.start()
    print("🧾 缺陷自动记录线程已启动（数据源: 8080 NPU 检测）", flush=True)
    return t
