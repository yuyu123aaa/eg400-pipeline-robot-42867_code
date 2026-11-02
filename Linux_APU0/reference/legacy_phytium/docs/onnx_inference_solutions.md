# ONNX 推理替代方案 — 飞腾派 YOLO 加速

## 背景

飞腾派运行 `main.py` 时，ONNX Runtime 1.19.2 在 ARM/Linux 上偶发 `std::vector` 越界崩溃：

```text
/opt/rh/gcc-toolset-12/root/usr/include/c++/12/bits/stl_vector.h:1123:
std::vector<unsigned int>::operator[]: Assertion '__n < this->size()' failed.
已放弃
```

当前已有稳定 `SessionOptions`（单线程、关闭图优化、关闭 spinning），但崩溃仍偶发。根本原因是 ONNX Runtime C++ 原生层在 `InferenceSession` 构造阶段（图解析 / 内存规划）存在 ARM 平台 bug，GCC 12 编译时 `_GLIBCXX_ASSERTIONS` 将静默越界变为 `abort()`。

直接改用 `.pt` 模型（Ultralytics 原生推理）可绕过崩溃，但帧率仅 **~10 fps**，不满足实时巡检需求。因此需要寻找替代的 ONNX 推理后端。

---

## 方案总览

| # | 方案 | 改动量 | 预期 FPS | 稳定性 | 耗时 |
|---|------|--------|----------|--------|------|
| ① | 换 onnxruntime pip 版本 | 零代码 | 15–25 | 取决于版本 | 5 分钟 |
| ② | OpenCV DNN 加载 ONNX | ~30 行 | 12–20 | ✅ 确定稳定 | 30 分钟 |
| ③ | ncnn 推理引擎 | ~40 行 + 模型转换 | **20–30** | ✅ 确定稳定 | 2 小时 |
| ④ | 源码编译 onnxruntime | 零代码 | 15–25 | ✅ 确定稳定 | 1–2 小时 |
| ⑤ | shell 兜底自动重试 | 零代码 | 15–25 | ⚠️ 启动靠运气 | 5 分钟 |

---

## 方案 ①：换 ONNX Runtime pip 版本（推荐先试）

### 原理

系统包管理器的 onnxruntime 用 GCC 12 (gcc-toolset-12) 编译，STL 带断言检查。pip 的 manylinux aarch64 wheel 用 older 工具链编译，不触发 `_GLIBCXX_ASSERTIONS`。

### 步骤

```bash
# 1. 卸载系统包版本
sudo dnf remove python3-onnxruntime

# 2. 逐个尝试 pip 版本
pip3 install onnxruntime==1.17.3

# 3. 验证：能否成功创建 session
python3 -c "
import onnxruntime as ort
print('ORT', ort.__version__)
ort.InferenceSession('/home/user/PhytiumLinux/best.onnx', providers=['CPUExecutionProvider'])
print('OK')
"

# 如果还崩，继续降级
pip3 install onnxruntime==1.16.3
pip3 install onnxruntime==1.15.1
pip3 install onnxruntime==1.14.1
```

### 注意事项

- 每个版本试一次 `InferenceSession` 创建，通过即表示该版本可用。
- 低版本 ONNX Runtime 对某些 opset 算子可能不支持，如果 `session.run()` 报错才需要换方案。
- 如果板子无法联网，在 PC 上下载 `.whl` 后 SCP 过去：

```powershell
# 在 PC 上下载 aarch64 wheel
pip download onnxruntime==1.17.3 --platform manylinux2014_aarch64 --only-binary :all: -d .

# 上传到飞腾派
scp onnxruntime-1.17.3-*.whl user@phytiumpi:/tmp/

# 在飞腾派上安装
pip3 install /tmp/onnxruntime-1.17.3-*.whl
```

---

## 方案 ②：OpenCV DNN 加载 ONNX（推荐首选）

### 原理

项目已依赖 OpenCV（`cv2`），其 DNN 模块原生支持加载和推理 ONNX 模型。OpenCV DNN 在后端用纯 C++ 实现（`dnn` 模块），不依赖 onnxruntime 库，不受 GCC 12 断言影响。ARM 上有 NEON 指令集加速。

### 代码修改

修改 `core/vision.py` 中 `_OnnxYoloModel` 类，将 `onnxruntime.InferenceSession` 替换为 `cv2.dnn.readNetFromONNX`：

```python
# ========== 替换 _OnnxYoloModel 类 ==========

class _OnnxYoloModel:
    """OpenCV DNN 后端加载 ONNX 模型——零 onnxruntime 依赖。"""

    def __init__(self, model_path):
        print(f"[YOLO] Loading ONNX via OpenCV DNN: {model_path}", flush=True)
        self.net = cv2.dnn.readNetFromONNX(model_path)
        # OpenCV DNN 不需要 session options，不会触发 GCC 断言
        self.input_w = 256
        self.input_h = 192
        self.names = self._read_names()
        print("[YOLO] OpenCV DNN model loaded.", flush=True)

    def _read_names(self):
        # ONNX metadata 无法通过 OpenCV DNN 读取，直接从 config 获取
        try:
            return {int(k): str(v) for k, v in config.DEFECT_NAME_MAP.items()}
        except Exception:
            return {}

    def predict(self, source, conf=0.3, verbose=False):
        # blobFromImage 自动：resize → BGR→RGB → HWC→CHW → /255
        blob = cv2.dnn.blobFromImage(
            source,
            scalefactor=1 / 255.0,
            size=(self.input_w, self.input_h),
            swapRB=True,     # BGR → RGB
            crop=False,
        )
        self.net.setInput(blob)
        output = self.net.forward()   # shape: [1, 7, N] 同 onnxruntime 输出
        boxes = self._postprocess(output, source.shape[1], source.shape[0], conf)
        return [_YoloResult(source, boxes, self.names)]

    # _postprocess 方法完全不变，沿用原有实现
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
            boxes.append(_YoloBox(
                [x, y, x + w, y + h],
                class_ids[int(index)],
                scores[int(index)],
            ))
        return boxes
```

同时删除不再需要的 `_create_onnx_session_options()` 函数和 `import onnxruntime`。

### 优点

- **零新依赖**：OpenCV 已经在项目中
- **无 GCC 12 问题**：OpenCV 通常用 GCC 8–10 编译
- **ARM NEON 加速**：`cv2.dnn` 在 ARM 上有 NEON 优化
- **推理输出格式一致**：`_postprocess` 不用改

### 缺点

- 无法从 ONNX metadata 读取类别名，需在 config 中维护
- 不支持 GPU delegate（飞腾派上本来就不用）

---

## 方案 ③：ncnn 推理引擎（性能最优）

### 原理

腾讯 ncnn 是专为 ARM 移动端设计的高性能推理框架，汇编级 NEON 优化，无系统依赖，体积极小。在飞腾派上推理速度通常比 ONNX Runtime 快 30–50%。

### 步骤

#### 3.1 PC 端：ONNX → ncnn 模型转换

```bash
# 安装 ncnn 工具链
git clone https://github.com/Tencent/ncnn.git
cd ncnn
mkdir build && cd build
cmake .. -DNCNN_BUILD_TOOLS=ON -DNCNN_BUILD_EXAMPLES=OFF
make -j$(nproc) onnx2ncnn

# 转换模型
./tools/onnx/onnx2ncnn best.onnx best.param best.bin

# （可选）优化模型
./tools/ncnnoptimize best.param best.bin best_opt.param best_opt.bin 65536
```

#### 3.2 飞腾派端：安装 ncnn Python 绑定

```bash
pip3 install ncnn
```

如果不能 pip 安装，可编译安装：

```bash
git clone https://github.com/Tencent/ncnn.git
cd ncnn
mkdir build && cd build
cmake .. -DNCNN_BUILD_TOOLS=OFF -DNCNN_PYTHON=ON
make -j$(nproc)
cd python && pip3 install .
```

#### 3.3 上传模型文件

```powershell
scp best_opt.param best_opt.bin user@phytiumpi:/home/user/PhytiumLinux/
```

#### 3.4 代码修改

修改 `core/vision.py` 中 `_OnnxYoloModel` 类：

```python
import ncnn

class _OnnxYoloModel:
    """ncnn 后端加载 ONNX 转换模型——ARM 优化推理。"""

    def __init__(self, model_path):
        param_path = model_path.replace(".onnx", ".param")
        bin_path = model_path.replace(".onnx", ".bin")
        print(f"[YOLO] Loading ncnn model: {param_path}", flush=True)
        self.net = ncnn.Net()
        self.net.load_param(param_path)
        self.net.load_model(bin_path)
        self.input_w = 256
        self.input_h = 192
        self.names = self._read_names()
        print("[YOLO] ncnn model loaded.", flush=True)

    def _read_names(self):
        try:
            return {int(k): str(v) for k, v in config.DEFECT_NAME_MAP.items()}
        except Exception:
            return {}

    def predict(self, source, conf=0.3, verbose=False):
        # ncnn 使用 from_pixels_resize 可直接从 numpy 数组转换
        mat_in = ncnn.Mat.from_pixels_resize(
            source, ncnn.Mat.PixelType.PIXEL_BGR2RGB,
            source.shape[1], source.shape[0],
            self.input_w, self.input_h,
        )
        mat_in.substract_mean_normalize([0, 0, 0], [1/255.0, 1/255.0, 1/255.0])

        ex = self.net.create_extractor()
        ex.input("images", mat_in)
        ret, mat_out = ex.extract("output0")

        output = np.array(mat_out)      # shape: [1, 7, N]
        boxes = self._postprocess(output, source.shape[1], source.shape[0], conf)
        return [_YoloResult(source, boxes, self.names)]

    # _postprocess 方法完全不变
```

### 优点

- ARM NEON 汇编级优化，**飞腾派上最快的 ONNX 替代方案**
- 无系统依赖，静态链接
- 内存占用极小

### 缺点

- 需要 PC 端模型转换步骤
- 输入/输出 tensor 名称需与模型一致（`images` / `output0`）
- 模型转换可能遇到不支持算子，需要简化模型

### 常见算子兼容问题

如果 `onnx2ncnn` 报 unsupported op：

```bash
# 先用 onnxsim 简化模型
pip install onnxsim
python3 -m onnxsim best.onnx best_simplified.onnx

# 再用简化后的模型转换
./onnx2ncnn best_simplified.onnx best.param best.bin
```

---

## 方案 ④：源码编译 ONNX Runtime（关断言）

### 原理

从源码编译 ONNX Runtime，编译时显式关闭 `_GLIBCXX_ASSERTIONS`，让内部的轻度越界问题变成静默行为而非 abort。

### 步骤

```bash
# 在飞腾派上编译（需要 1-2 小时）
sudo dnf install -y cmake gcc-c++ git python3-devel

git clone --depth 1 --branch v1.18.0 https://github.com/microsoft/onnxruntime.git
cd onnxruntime

./build.sh --config Release \
    --build_shared_lib \
    --parallel \
    --cmake_extra_defines \
        CMAKE_CXX_FLAGS="-D_GLIBCXX_ASSERTIONS=0" \
    --skip_tests \
    --use_cpu

# 安装编译好的 wheel
pip3 install build/Linux/Release/dist/onnxruntime-*-linux_aarch64.whl
```

### 优点

- 代码完全不变
- onnxruntime API 兼容，SessionOptions 等都可保留
- 自由选择 onnxruntime 版本

### 缺点

- 飞腾派上编译耗时 1–2 小时
- 需要安装编译工具链
- 升级不方便（需要重新编译）

---

## 方案 ⑤：Shell 兜底自动重试

### 原理

崩溃发生在 `InferenceSession` 构造阶段（启动时）。一旦成功创建 session，后续推理不会再崩。用 shell 死循环自动重启，直到启动成功。

### 步骤

创建 `/home/user/PhytiumLinux/start_robot.sh`：

```bash
#!/bin/bash
# 管道巡检机器人 — 崩溃自动重启启动器
# 解决 ONNX Runtime 启动时偶发崩溃问题

RESTART_COUNT=0
MAX_RESTART=50   # 防止无限重启

cleanup() {
    echo "[LAUNCHER] 释放系统资源..."
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1
}

while [ $RESTART_COUNT -lt $MAX_RESTART ]; do
    echo "[LAUNCHER] ==== 第 $((RESTART_COUNT + 1)) 次启动 $(date) ===="
    cleanup
    python3 /home/user/PhytiumLinux/main.py
    rc=$?
    echo "[LAUNCHER] main.py 退出，返回码: $rc"

    # 正常退出（Ctrl+C = 130，SIGTERM = 143）不重启
    if [ $rc -eq 130 ] || [ $rc -eq 143 ] || [ $rc -eq 0 ]; then
        echo "[LAUNCHER] 正常退出，不重启。"
        exit $rc
    fi

    RESTART_COUNT=$((RESTART_COUNT + 1))
    echo "[LAUNCHER] 崩溃了！3 秒后重试..."
    sleep 3
done

echo "[LAUNCHER] 已达到最大重试次数 ($MAX_RESTART)，放弃。"
exit 1
```

```bash
chmod +x /home/user/PhytiumLinux/start_robot.sh
./start_robot.sh
```

### 优点

- 零代码改动
- 5 分钟搞定
- 适用于任何崩在启动阶段的场景

### 缺点

- 纯靠运气，可能连崩数十次
- 如果后续推理阶段也崩（目前没观察到），无法解决
- 启动期间有几秒"盲等"窗口

---

## 方案选择建议

```
试试 ①？
 ├── 成功了 → 收工
 └── 不行 ↓
选 ② 还是 ③？
 ├── 时间紧、不想折腾 → ② OpenCV DNN（30 分钟，够用）
 └── 追求帧率、愿意折腾 → ③ ncnn（20-30 fps，比赛加分）
```

---

## 参考

- [ONNX Runtime 崩溃排查与修复记录](./onnx_runtime_crash_resolution.md) — 已有的崩溃分析和 SessionOptions 修复
- [ncnn GitHub](https://github.com/Tencent/ncnn)
- [OpenCV DNN ONNX 文档](https://docs.opencv.org/4.x/d3/d9f/tutorial_dnn_onnx.html)
