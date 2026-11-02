"""
摄像头与 YOLO 推理模块
======================
- YOLO 模型加载
- 摄像头帧采集线程
- YOLO 缺陷检测与自动记录（拍照 + CSV）
- MJPEG 推流帧生成器

跨模块可变状态均通过 ``config.xxx`` 访问，避免 ``global`` 跨模块失效。
"""

import os
import csv as csv_module
import time
import threading
import cv2
import numpy as np
from datetime import datetime

import config
from core.severity import resolve_defect_profile

# YOLO 模型实例（本模块内部状态，非共享）
_model = None
_model_names = {}
_stream_fps_window_started_at = time.monotonic()
_stream_fps_frame_count = 0
_stream_clients_lock = threading.Lock()
_stream_client_count = 0


class _YoloBox:
    def __init__(self, xyxy, cls_id, conf):
        self.xyxy = np.array([xyxy], dtype=np.float32)
        self.cls = np.array([cls_id], dtype=np.float32)
        self.conf = np.array([conf], dtype=np.float32)


class _YoloResult:
    def __init__(self, frame, boxes, names):
        self._frame = frame
        self.boxes = boxes
        self.names = names

    def plot(self, line_width=1, font_size=1):
        annotated = self._frame.copy()
        for box in self.boxes:
            x1, y1, x2, y2 = box.xyxy[0].astype(int)
            cls_id = int(box.cls[0])
            conf = float(box.conf[0])
            label = f"{self.names.get(cls_id, cls_id)} {conf:.2f}"
            cv2.rectangle(annotated, (x1, y1), (x2, y2), (0, 255, 0), line_width)
            cv2.putText(annotated, label, (x1, max(12, y1 - 4)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.4, (0, 255, 0), 1, cv2.LINE_AA)
        return annotated


class _YoloFrameIntervalState:
    def __init__(self):
        self.frames_since_detection = 0
        self.last_result = None


class _LatestYoloWorker:
    """Run inference off the camera thread and keep only the newest queued frame."""

    def __init__(self):
        self._condition = threading.Condition()
        self._pending = None
        self._latest_result = None
        self._result_sequence = 0
        self._generation = 0
        self._stopping = False
        self._thread = None

    def start(self):
        with self._condition:
            if self._thread is not None and self._thread.is_alive():
                return self._thread
            self._stopping = False
            self._thread = threading.Thread(target=self._run, daemon=True)
            thread = self._thread
        thread.start()
        return thread

    def stop(self, timeout=1.0):
        with self._condition:
            self._stopping = True
            self._pending = None
            self._condition.notify_all()
            thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=timeout)

    def invalidate(self):
        """Discard queued/in-flight results when YOLO is toggled."""
        with self._condition:
            self._generation += 1
            self._pending = None
            self._latest_result = None
            self._condition.notify_all()

    def submit(self, model, frame, confidence):
        """Replace any waiting frame instead of building an inference backlog."""
        with self._condition:
            if self._stopping:
                return False
            self._pending = (
                self._generation,
                model,
                frame.copy(),
                confidence,
            )
            self._condition.notify()
        return True

    def latest(self):
        with self._condition:
            return self._result_sequence, self._latest_result

    def _run(self):
        while True:
            with self._condition:
                while self._pending is None and not self._stopping:
                    self._condition.wait()
                if self._stopping:
                    return
                generation, model, frame, confidence = self._pending
                self._pending = None

            try:
                result = model.predict(
                    source=frame,
                    conf=confidence,
                    verbose=False,
                )[0]
            except Exception as exc:
                print(f"[YOLO] Inference failed: {exc}", flush=True)
                continue

            with self._condition:
                if self._stopping or generation != self._generation:
                    continue
                self._result_sequence += 1
                self._latest_result = result


_yolo_worker = _LatestYoloWorker()


def _scale_xyxy(box, source_size, target_size):
    source_width, source_height = source_size
    target_width, target_height = target_size
    scale_x = target_width / source_width
    scale_y = target_height / source_height
    x1, y1, x2, y2 = box
    return (
        int(round(float(x1) * scale_x)),
        int(round(float(y1) * scale_y)),
        int(round(float(x2) * scale_x)),
        int(round(float(y2) * scale_y)),
    )


def _resolve_yolo_annotation(class_id, model_name=None):
    if model_name is None:
        normalized_name = config.YOLO_CLASS_NAMES.get(int(class_id), "")
    else:
        normalized_name = str(model_name).strip().lower()

    style = config.YOLO_ANNOTATION_STYLES.get(normalized_name)
    if style is not None:
        return {
            "name": normalized_name,
            "severity": style["severity"],
            "bgr": tuple(style["bgr"]),
        }

    fallback_name = (
        str(model_name).strip()
        if model_name is not None
        else str(class_id)
    )
    if not fallback_name.isascii():
        fallback_name = f"class-{int(class_id)}"

    return {
        "name": fallback_name,
        "severity": "Pending",
        "bgr": (128, 128, 128),
    }


def _draw_scaled_yolo_result(frame, result, source_size, line_width=2, font_size=0.6):
    annotated = frame.copy()
    target_size = (frame.shape[1], frame.shape[0])

    for box in result.boxes:
        x1, y1, x2, y2 = _scale_xyxy(
            box.xyxy[0],
            source_size=source_size,
            target_size=target_size,
        )
        cls_id = int(box.cls[0])
        conf = float(box.conf[0])
        names = getattr(result, "names", _model_names)
        model_name = names.get(cls_id)
        annotation = _resolve_yolo_annotation(cls_id, model_name)
        label = (
            f"{annotation['name']} | "
            f"{annotation['severity']} | "
            f"{conf:.2f}"
        )
        box_color = annotation["bgr"]

        cv2.rectangle(annotated, (x1, y1), (x2, y2), box_color, line_width)
        cv2.putText(
            annotated,
            label,
            (x1, max(16, y1 - 8)),
            cv2.FONT_HERSHEY_SIMPLEX,
            font_size,
            box_color,
            1,
            cv2.LINE_AA,
        )

    return annotated


def _build_defect_log(profile, confidence, mileage, timestamp, image_url):
    """构造 API、页面和 CSV 共用的缺陷记录。"""
    return {
        "type": profile["type"],
        "severity": profile["severity"],
        "severity_code": int(profile["severity_code"]),
        "confidence": round(float(confidence), 2),
        "advice": profile["advice"],
        "mileage": round(float(mileage), 2),
        "time": timestamp,
        "image": image_url,
    }


def _append_defect_csv(csv_path, defect_log, image_filename):
    """将一条统一缺陷记录追加到巡检 CSV。"""
    file_exists = os.path.isfile(csv_path)
    with open(csv_path, mode="a", newline="", encoding="utf-8-sig") as csv_file:
        writer = csv_module.writer(csv_file)
        if not file_exists:
            writer.writerow([
                "记录时间",
                "发生里程(m)",
                "病害类型",
                "严重等级",
                "识别置信度",
                "处置建议",
                "关联图片文件",
            ])
        writer.writerow([
            defect_log["time"],
            defect_log["mileage"],
            defect_log["type"],
            defect_log["severity"],
            defect_log["confidence"],
            defect_log["advice"],
            image_filename,
        ])


def _publish_stream_frame(frame_bytes, capture_timestamp_ms):
    global _stream_fps_window_started_at, _stream_fps_frame_count

    config.global_frame_timestamp_ms = capture_timestamp_ms
    config.global_frame = frame_bytes
    config.global_frame_id += 1

    _stream_fps_frame_count += 1
    now = time.monotonic()
    elapsed = now - _stream_fps_window_started_at
    if elapsed >= 1.0:
        config.system_status["stream_fps"] = round(
            _stream_fps_frame_count / elapsed,
            1,
        )
        _stream_fps_frame_count = 0
        _stream_fps_window_started_at = now


def _reset_stream_fps_counter():
    global _stream_fps_window_started_at, _stream_fps_frame_count

    _stream_fps_window_started_at = time.monotonic()
    _stream_fps_frame_count = 0
    config.system_status["stream_fps"] = 0.0


def _get_stream_client_count():
    with _stream_clients_lock:
        return _stream_client_count


def _register_stream_client():
    global _stream_client_count

    with _stream_clients_lock:
        was_empty = _stream_client_count == 0
        _stream_client_count += 1
    if was_empty:
        _reset_stream_fps_counter()


def _unregister_stream_client():
    global _stream_client_count

    with _stream_clients_lock:
        _stream_client_count = max(0, _stream_client_count - 1)
        is_empty = _stream_client_count == 0
    if is_empty:
        _reset_stream_fps_counter()


def _should_encode_stream_frame():
    return config.take_photo_flag or _get_stream_client_count() > 0


def _configure_camera_capture(cap):
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, config.CAMERA_WIDTH)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, config.CAMERA_HEIGHT)


def _get_yolo_detect_interval():
    try:
        return max(1, int(config.YOLO_DETECT_EVERY_N_FRAMES))
    except (TypeError, ValueError):
        return 1


def _predict_yolo_for_frame(model, frame, state):
    interval = _get_yolo_detect_interval()
    should_detect = state.last_result is None or state.frames_since_detection <= 0

    if should_detect:
        result = model.predict(source=frame, conf=config.YOLO_CONFIDENCE, verbose=False)[0]
        state.last_result = result
        state.frames_since_detection = interval - 1
        return result, True

    state.frames_since_detection -= 1
    return _YoloResult(frame, state.last_result.boxes, _model_names), False


class _OnnxYoloModel:
    def __init__(self, model_path):
        self.input_w = int(config.TARGET_WIDTH)
        self.input_h = int(config.TARGET_HEIGHT)
        self.names = dict(config.YOLO_CLASS_NAMES)

        print(f"[YOLO] Loading ONNX with OpenCV DNN: {model_path}", flush=True)
        self.net = cv2.dnn.readNetFromONNX(model_path)
        self.net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
        self.net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)

        print("[YOLO] Warming up OpenCV DNN...", flush=True)
        warmup = np.zeros(
            (1, 3, self.input_h, self.input_w),
            dtype=np.float32,
        )
        self.net.setInput(warmup)
        self.net.forward()
        print("[YOLO] OpenCV DNN model ready.", flush=True)

    def predict(self, source, conf=0.3, verbose=False):
        frame = source
        if frame.shape[:2] != (self.input_h, self.input_w):
            frame = cv2.resize(frame, (self.input_w, self.input_h))

        blob = cv2.dnn.blobFromImage(
            frame,
            scalefactor=1.0 / 255.0,
            size=(self.input_w, self.input_h),
            mean=(0, 0, 0),
            swapRB=True,
            crop=False,
            ddepth=cv2.CV_32F,
        )
        self.net.setInput(blob)
        output = self.net.forward()
        boxes = self._postprocess(output, frame.shape[1], frame.shape[0], conf)
        return [_YoloResult(frame, boxes, self.names)]

    def _postprocess(self, output, width, height, conf):
        predictions = np.squeeze(output, axis=0).T
        candidates = []
        scores = []
        class_ids = []

        for pred in predictions:
            class_scores = pred[4:]
            cls_id = int(np.argmax(class_scores))
            score = float(class_scores[cls_id])
            if score < conf:
                continue

            cx, cy, w, h = [float(v) for v in pred[:4]]
            x = max(0, min(width - 1, cx - w / 2))
            y = max(0, min(height - 1, cy - h / 2))
            w = max(1, min(width - x, w))
            h = max(1, min(height - y, h))
            candidates.append([int(x), int(y), int(w), int(h)])
            scores.append(score)
            class_ids.append(cls_id)

        if not candidates:
            return []

        indices = cv2.dnn.NMSBoxes(candidates, scores, conf, config.YOLO_IOU)
        if len(indices) == 0:
            return []

        boxes = []
        for index in np.array(indices).reshape(-1):
            x, y, w, h = candidates[int(index)]
            boxes.append(_YoloBox([x, y, x + w, y + h], class_ids[int(index)], scores[int(index)]))
        return boxes


def init_yolo():
    """加载 YOLO 模型（阻塞，建议在启动时调用一次）"""
    global _model, _model_names
    if _model is not None:
        return True

    print(f"[YOLO] Loading model: {config.MODEL_PATH}", flush=True)
    try:
        if not os.path.exists(config.MODEL_PATH):
            raise FileNotFoundError(f"模型文件不存在: {config.MODEL_PATH}")

        if config.MODEL_PATH.lower().endswith(".onnx"):
            _model = _OnnxYoloModel(config.MODEL_PATH)
        else:
            from ultralytics import YOLO
            _model = YOLO(config.MODEL_PATH, task='detect')

        _model_names = _model.names
        print("[YOLO] Model initialization completed.", flush=True)
        return True
    except Exception as e:
        print(f"[YOLO] Model initialization failed: {e}", flush=True)
        _model = None
        return False


def set_yolo_enabled(enabled):
    """打开/关闭 YOLO；首次打开时按需加载模型。"""
    if enabled:
        if not init_yolo():
            config.ENABLE_YOLO = False
            config.system_status["yolo_on"] = False
            _yolo_worker.invalidate()
            return False
        _yolo_worker.invalidate()
        config.ENABLE_YOLO = True
        config.system_status["yolo_on"] = True
        return True

    config.ENABLE_YOLO = False
    config.system_status["yolo_on"] = False
    _yolo_worker.invalidate()
    return True


def _camera_loop():
    """摄像头线程主循环：采集帧 → YOLO 推理 → 编码 JPEG"""

    print("📸 正在启动摄像头...")
    cap = cv2.VideoCapture(config.CAMERA_INDEX)
    _configure_camera_capture(cap)
    time.sleep(2)
    print("📸 摄像头就绪！")

    last_log_time = {}   # 同类缺陷冷却计时
    yolo_frames_until_submit = 0
    last_yolo_result_sequence = 0

    while True:
        if not config.system_status["is_running"]:
            _reset_stream_fps_counter()
            if not cap.grab():
                time.sleep(0.01)
            continue

        ret, frame = cap.read()
        if not ret or frame is None:
            continue
        capture_timestamp_ms = int(time.time() * 1000)

        # ---- YOLO 推理 ----
        if config.ENABLE_YOLO and _model is not None:
            yolo_frame = cv2.resize(
                frame,
                (config.TARGET_WIDTH, config.TARGET_HEIGHT),
            )
            if yolo_frames_until_submit <= 0:
                _yolo_worker.submit(
                    _model,
                    yolo_frame,
                    config.YOLO_CONFIDENCE,
                )
                yolo_frames_until_submit = _get_yolo_detect_interval() - 1
            else:
                yolo_frames_until_submit -= 1

            result_sequence, result = _yolo_worker.latest()
            is_fresh_detection = (
                result is not None
                and result_sequence != last_yolo_result_sequence
            )

            if result is not None:
                annotated_frame = _draw_scaled_yolo_result(
                    frame,
                    result,
                    source_size=(config.TARGET_WIDTH, config.TARGET_HEIGHT),
                )
            else:
                annotated_frame = frame

            if is_fresh_detection and config.current_inspect_dir:
                last_yolo_result_sequence = result_sequence
                for box in result.boxes:
                    cls_id = int(box.cls[0])
                    conf = float(box.conf[0])
                    model_name = _model_names.get(cls_id, str(cls_id))
                    profile = resolve_defect_profile(cls_id, model_name)
                    name = profile["type"]

                    if conf > config.DEFECT_CONFIDENCE:
                        current_t = time.time()
                        if current_t - last_log_time.get(name, 0) > config.LOG_COOLDOWN:
                            last_log_time[name] = current_t

                            img_filename = f"DEFECT_{datetime.now().strftime('%Y%m%d_%H%M%S')}.jpg"
                            img_path = os.path.join(config.current_inspect_dir, img_filename)
                            cv2.imwrite(img_path, annotated_frame)

                            folder_name = os.path.basename(config.current_inspect_dir)
                            new_log = _build_defect_log(
                                profile=profile,
                                confidence=conf,
                                mileage=config.total_mileage,
                                timestamp=datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                                image_url=f"/images/{folder_name}/{img_filename}",
                            )
                            config.defect_logs.append(new_log)
                            print(
                                f"🚨 记录缺陷: {new_log['type']} | "
                                f"{new_log['severity']} | 里程: {new_log['mileage']}m"
                            )

                            # 写入 CSV
                            csv_path = os.path.join(config.current_inspect_dir, "巡检缺陷报告.csv")
                            _append_defect_csv(csv_path, new_log, img_filename)
            elif is_fresh_detection:
                last_yolo_result_sequence = result_sequence
        else:
            yolo_frames_until_submit = 0
            annotated_frame = frame

        # ---- JPEG 编码供推流 ----
        if _should_encode_stream_frame():
            encoded, buffer = cv2.imencode(
                '.jpg',
                annotated_frame,
                [int(cv2.IMWRITE_JPEG_QUALITY), config.JPEG_QUALITY],
            )
            if encoded:
                frame_bytes = buffer.tobytes()
                _publish_stream_frame(frame_bytes, capture_timestamp_ms)

                # ---- 拍照 ----
                if config.take_photo_flag:
                    filename = os.path.join(
                        config.SAVE_DIR,
                        f"IMG_{datetime.now().strftime('%Y%m%d_%H%M%S')}.jpg",
                    )
                    with open(filename, 'wb') as f:
                        f.write(frame_bytes)
                    config.take_photo_flag = False

        # ---- 录像 ----
        if config.is_recording:
            if config.video_writer is None:
                vid_filename = os.path.join(config.SAVE_DIR,
                                            f"VID_{datetime.now().strftime('%Y%m%d_%H%M%S')}.mp4")
                fourcc = cv2.VideoWriter_fourcc(*'mp4v')
                frame_h, frame_w = annotated_frame.shape[:2]
                config.video_writer = cv2.VideoWriter(vid_filename, fourcc, 20.0,
                                                      (frame_w, frame_h))
            config.video_writer.write(annotated_frame)
        else:
            if config.video_writer is not None:
                config.video_writer.release()
                config.video_writer = None


def start_camera_thread():
    """启动摄像头采集线程（守护线程）"""
    _yolo_worker.start()
    t = threading.Thread(target=_camera_loop, daemon=True)
    t.start()
    return t


def gen_frames():
    """MJPEG 推流帧生成器（供 Flask Response 使用）"""
    last_sent_frame_id = None
    _register_stream_client()
    try:
        while True:
            if not config.system_status["is_running"]:
                time.sleep(0.1)
                continue

            frame = config.global_frame
            frame_id = config.global_frame_id
            frame_timestamp_ms = config.global_frame_timestamp_ms
            if frame is not None and frame_id != last_sent_frame_id:
                last_sent_frame_id = frame_id
                yield (b'--frame\r\n'
                       b'Content-Type: image/jpeg\r\n' +
                       f'Content-Length: {len(frame)}\r\n'.encode('ascii') +
                       f'X-Frame-Id: {frame_id}\r\n'.encode('ascii') +
                       f'X-Frame-Time-Ms: {frame_timestamp_ms}\r\n\r\n'.encode('ascii') +
                       frame + b'\r\n')

            time.sleep(0.005)
    finally:
        _unregister_stream_client()
