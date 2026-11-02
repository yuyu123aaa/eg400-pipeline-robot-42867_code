# ncnn 迁移指南 — ONNX Runtime → ncnn

## 前提

- 模型：`best.onnx`，输入 `images [1,3,192,256]`，输出 `output0 [1,9,1008]`，opset 11
- 飞腾派架构：ARM aarch64
- 目标：用 ncnn 替换 onnxruntime，消除 GCC 12 `std::vector` 断言崩溃，提升推理速度

---

## 一、PC 端：编译 ncnn 工具 + 转换模型

```bash
# 1. 克隆 ncnn
git clone https://github.com/Tencent/ncnn.git
cd ncnn && mkdir build && cd build

# 2. 编译 onnx2ncnn 和 ncnnoptimize 两个工具
cmake .. -DNCNN_BUILD_TOOLS=ON -DNCNN_BUILD_EXAMPLES=OFF
make -j$(nproc) onnx2ncnn ncnnoptimize

# 3. 把 best.onnx 拷贝到当前目录，然后转换
./tools/onnx/onnx2ncnn best.onnx best.param best.bin

# 4. 优化（fp16 存储，体积减半，推理更快）
./tools/ncnnoptimize best.param best.bin best_opt.param best_opt.bin 65536

# 5. 上传到飞腾派
scp best_opt.param best_opt.bin user@phytiumpi:/home/user/PhytiumLinux/
```

> **如果 onnx2ncnn 报 unsupported op**：先 `pip install onnxsim && python -m onnxsim best.onnx best_sim.onnx`，再对 `best_sim.onnx` 执行步骤 3。

---

## 二、飞腾派：安装 ncnn Python 绑定

### 方式 A：pip（5秒）

```bash
pip3 install ncnn
python3 -c "import ncnn; print('OK')"
```

### 方式 B：源码编译（20分钟）

如果 pip 没有 aarch64 预编译包：

```bash
# 安装编译依赖
sudo dnf install -y cmake gcc-c++ git

# 克隆 ncnn
git clone https://github.com/Tencent/ncnn.git
cd ncnn && mkdir build && cd build

# 编译（只编译 Python 绑定，不开工具和测试）
cmake .. \
    -DNCNN_BUILD_TOOLS=OFF \
    -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_BUILD_BENCHMARK=OFF \
    -DNCNN_BUILD_TESTS=OFF \
    -DNCNN_PYTHON=ON
make -j$(nproc)

# 安装
cd python && pip3 install .
```

验证：

```bash
python3 -c "import ncnn; print('ncnn OK')"
```

---

## 三、修改 vision.py

文件：`core/vision.py`

### 3.1 顶部 import 区

```python
# 新增
import ncnn

# 删除下面这行（如果有的话）
# import onnxruntime
```

如果 `onnxruntime` 还在其他地方被 import（比如 `from core.vision import _create_onnx_session_options`），也要一并清理。

### 3.2 删除 `_create_onnx_session_options` 函数

删除第 231–241 行整个函数：

```python
# 删除 ↓
def _create_onnx_session_options():
    import onnxruntime as ort
    ...
    return options
```

### 3.3 替换 `_OnnxYoloModel` 类

找到第 244–313 行的 `_OnnxYoloModel` 类，整体替换为：

```python
class _OnnxYoloModel:
    """ncnn 后端——ARM NEON 汇编级优化，零 onnxruntime 依赖。"""

    def __init__(self, model_path):
        param_path = model_path.replace(".onnx", ".param")
        bin_path = model_path.replace(".onnx", ".bin")

        print(f"[YOLO] Loading ncnn model: {param_path}", flush=True)
        self.net = ncnn.Net()
        self.net.load_param(param_path)
        self.net.load_model(bin_path)

        self.input_w = 256
        self.input_h = 192
        self.input_name = "images"
        self.output_name = "output0"
        self.names = self._read_names()
        print("[YOLO] ncnn model loaded.", flush=True)

    def _read_names(self):
        # ncnn 无法读取 ONNX metadata，fallback 到 config
        try:
            return {int(k): str(v) for k, v in config.DEFECT_NAME_MAP.items()}
        except Exception:
            return {}

    def predict(self, source, conf=0.3, verbose=False):
        # from_pixels_resize: 一步完成 resize + BGR→RGB
        mat_in = ncnn.Mat.from_pixels_resize(
            source,
            ncnn.Mat.PixelType.PIXEL_BGR2RGB,
            source.shape[1],
            source.shape[0],
            self.input_w,
            self.input_h,
        )
        # 归一化到 [0, 1]: (pixel - 0) * (1/255) = pixel / 255
        mat_in.substract_mean_normalize(
            [0.0, 0.0, 0.0],
            [1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0],
        )

        # 推理
        ex = self.net.create_extractor()
        ex.input(self.input_name, mat_in)
        ret, mat_out = ex.extract(self.output_name)
        if ret != 0:
            raise RuntimeError(f"ncnn extract failed, ret={ret}")

        # ncnn.Mat → numpy，shape 仍是 [9, 1008]
        out_np = np.array(mat_out)
        boxes = self._postprocess(out_np, source.shape[1], source.shape[0], conf)
        return [_YoloResult(source, boxes, self.names)]

    def _postprocess(self, output, width, height, conf):
        # ============================================================
        # 以下内容从原 _OnnxYoloModel._postprocess 原封不动搬过来
        # 不要改任何逻辑，输入输出形状完全一致
        # ============================================================
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
            boxes.append(_YoloBox(
                [x, y, x + w, y + h],
                class_ids[int(index)],
                scores[int(index)],
            ))
        return boxes
```

### 3.4 不需要改的地方

以下函数/类**完全不动**：

- `_YoloBox` — 检测框包装
- `_YoloResult` — 推理结果包装
- `_predict_yolo_for_frame` — 跳帧逻辑
- `init_yolo` — 模型加载入口（`.onnx` 后缀仍走 `_OnnxYoloModel`）
- `set_yolo_enabled` — 开关控制
- `_camera_loop` — 摄像头主循环
- `_draw_scaled_yolo_result` — 画框标注
- `gen_frames` — MJPEG 推流

---

## 四、验证

### 4.1 加载测试（确保不崩）

```bash
python3 -c "
import sys; sys.path.insert(0, '/home/user/PhytiumLinux')
import config
from core.vision import _OnnxYoloModel
m = _OnnxYoloModel(config.MODEL_PATH)
print('ncnn model loaded OK')
print('names:', m.names)
"
```

### 4.2 单帧推理测试

```bash
python3 -c "
import cv2, numpy as np
import sys; sys.path.insert(0, '/home/user/PhytiumLinux')
from core.vision import _OnnxYoloModel, _draw_scaled_yolo_result
import config

m = _OnnxYoloModel(config.MODEL_PATH)
dummy = np.random.randint(0, 255, (480, 640, 3), dtype=np.uint8)
results = m.predict(dummy, conf=0.3)
print(f'boxes={len(results[0].boxes)}')
"
```

### 4.3 连续推理性能测试

```bash
python3 -c "
import cv2, time, numpy as np
import sys; sys.path.insert(0, '/home/user/PhytiumLinux')
from core.vision import _OnnxYoloModel
import config

m = _OnnxYoloModel(config.MODEL_PATH)
cap = cv2.VideoCapture(0)
times = []
for i in range(50):
    ret, frame = cap.read()
    t0 = time.time()
    results = m.predict(frame, conf=0.3)
    ms = (time.time() - t0) * 1000
    times.append(ms)
    print(f'frame {i}: {ms:.1f}ms  boxes={len(results[0].boxes)}')
cap.release()
print(f'---')
print(f'avg: {np.mean(times):.1f}ms  fps: {1000/np.mean(times):.1f}')
"
```

### 4.4 完整启动

```bash
python3 /home/user/PhytiumLinux/main.py
```

---

## 五、回退

```bash
cd /home/user/PhytiumLinux
git checkout core/vision.py
```

---

## 六、常见问题

### Q: onnx2ncnn 报 `Unsupported op XXX`

先用 onnxsim 简化模型图：

```bash
pip install onnxsim
python -m onnxsim best.onnx best_sim.onnx
```

再用 `best_sim.onnx` 做转换。

### Q: `from_pixels_resize` 参数报错

确认 ncnn 版本 ≥ 2023。如果版本太老，手动预处理：

```python
frame = cv2.resize(source, (self.input_w, self.input_h))
frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
mat_in = ncnn.Mat.from_pixels(
    frame.tobytes(),
    ncnn.Mat.PixelType.PIXEL_RGB,
    self.input_w, self.input_h,
)
mat_in.substract_mean_normalize([0,0,0], [1/255.0]*3)
```

### Q: `np.array(mat_out)` 报错

手动转换：

```python
out_np = np.zeros((mat_out.h, mat_out.w), dtype=np.float32)
for c in range(mat_out.c):
    channel = np.array(mat_out.channel(c))
    out_np = channel.reshape(mat_out.h, mat_out.w)
```

（如输出是单通道 `c=1` 时直接 `np.array(mat_out.channel(0))` 即可）

### Q: 类别名不对

ncnn 无法读取 ONNX metadata，类名从 `config.DEFECT_NAME_MAP` 取。确认 config.py 中 `DEFECT_NAME_MAP` 的 key 与模型类别索引一致：

```python
DEFECT_NAME_MAP = {
    0: "管壁脱落",
    1: "裂缝",
    2: "破洞",
    3: "树根侵入",
    4: "泥沙淤积",
}
```
