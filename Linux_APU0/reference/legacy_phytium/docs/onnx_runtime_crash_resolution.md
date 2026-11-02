# ONNX Runtime 崩溃排查与修复记录

## 背景

飞腾派运行 `main.py` 时，启动阶段加载 `best.onnx` 偶发崩溃。日志中能看到 ONNX Runtime 已开始加载模型：

```text
正在加载 YOLO 核心...
Loading /home/user/PhytiumLinux/best.onnx for ONNX Runtime inference...
Using ONNX Runtime 1.19.2 with CPUExecutionProvider
/opt/rh/gcc-toolset-12/root/usr/include/c++/12/bits/stl_vector.h:1123:
std::vector<unsigned int>::operator[]: Assertion '__n < this->size()' failed.
已放弃
```

这个错误不是 Python 异常，而是 ONNX Runtime 的 C++ 原生层触发 `abort`，所以普通 `try/except` 无法捕获。

## 初步判断

问题不是简单的模型文件损坏：

- `best.onnx` 能通过 ONNX checker。
- 模型输入固定为 `[1, 3, 192, 256]`。
- 模型输出为 `[1, 7, 1008]`，对应 4 个框坐标加 3 个类别。
- 模型导出参数中 `nms=False`。
- 在本机裸 ONNX Runtime 能创建 session 并完成一次随机输入推理。

因此，更可疑的是飞腾派上的 ONNX Runtime 1.19.2 在 ARM/Linux 环境下，使用默认 session 初始化策略加载该 YOLO ONNX 图时不稳定。

## 最小复现

为排除 Flask、摄像头、手柄、温度传感器和业务线程干扰，在开发板上直接测试：

```bash
cd /home/user/PhytiumLinux
python3 -c 'import onnxruntime as ort; print("ORT", ort.__version__); print("providers", ort.get_available_providers()); [print("default session", i, flush=True) or ort.InferenceSession("best.onnx", providers=["CPUExecutionProvider"]) for i in range(200)]; print("default done")'
```

结果：默认配置连续创建 session 时，运行到约 175 次后开发板卡死，无法打开新终端，只能重启。

重启后检查系统状态：

```text
内存 total 3.4GiB，available 2.3GiB
swap 使用 0B
dmesg 未见 OOM killer
```

说明问题不像是单纯内存不足，更像 ONNX Runtime 默认线程池、图优化或 session 初始化策略导致资源/调度异常。

## 稳定配置对照

随后使用稳定 SessionOptions 做对照测试：

```bash
cd /home/user/PhytiumLinux
python3 -c 'import onnxruntime as ort; so=ort.SessionOptions(); so.intra_op_num_threads=1; so.inter_op_num_threads=1; so.execution_mode=ort.ExecutionMode.ORT_SEQUENTIAL; so.graph_optimization_level=ort.GraphOptimizationLevel.ORT_DISABLE_ALL; so.add_session_config_entry("session.intra_op.allow_spinning","0"); so.add_session_config_entry("session.inter_op.allow_spinning","0"); print("ORT", ort.__version__); [print("stable session", i, flush=True) or ort.InferenceSession("best.onnx", sess_options=so, providers=["CPUExecutionProvider"]) for i in range(20)]; print("stable done")'
```

结果：稳定输出 `stable done`。

由此得到结论：

- `best.onnx` 本身大概率可用。
- 主程序、Flask、摄像头线程不是崩溃主因。
- ONNX Runtime 默认 session 配置在飞腾派上有风险。
- 限制线程、关闭图优化、关闭 spinning 后稳定性明显改善。

## 代码修复方案

修复思路不是改用 `.pt` 模型，因为 `.pt` 在开发板上延迟明显更高；也不是单纯“裸 ONNX Runtime 就能绕过问题”，因为底层库仍然是同一个。

真正有效的方案是：

1. Linux 默认模型路径继续使用项目内 `best.onnx`。
2. `.onnx` 模型不再走 Ultralytics 默认 ONNX 加载路径。
3. 新增直接 ONNX Runtime 后端 `_OnnxYoloModel`。
4. 创建 session 时固定使用稳定参数：

```text
intra_op_num_threads = 1
inter_op_num_threads = 1
execution_mode = ORT_SEQUENTIAL
graph_optimization_level = ORT_DISABLE_ALL
session.intra_op.allow_spinning = 0
session.inter_op.allow_spinning = 0
providers = ["CPUExecutionProvider"]
```

5. 程序中模型只加载一次，后续复用同一个 session。
6. 自己实现 ONNX 输出后处理、NMS 和画框包装，使业务代码仍可使用：

```python
results = _model.predict(...)
results[0].plot(...)
results[0].boxes
```

`.pt` 模型仍保留 Ultralytics 加载路径，作为备用。

## 真实摄像头验证

摄像头读取测试：

```text
opened: True
ret: True shape: (480, 640, 3)
```

ONNX 加载测试：

```text
正在加载 YOLO 核心...
YOLO 模型加载成功！
True
```

真实摄像头连续推理 50 帧：

```text
infer 0: 197.5 ms, boxes=0
infer 1~49: 基本稳定 150 ms 左右
done 50 frames avg total fps 6.1
```

注意：摄像头原始画面为 `640x480`，但送入模型前会 resize 到 `256x192`，因为 `best.onnx` 的固定输入就是 `[1, 3, 192, 256]`。

## 最终结论

原始问题是：

```text
ONNX Runtime 1.19.2 在飞腾派 ARM/Linux 上，用默认配置加载 best.onnx 时偶发 native 层崩溃或卡死。
```

当前方案是：

```text
使用 best.onnx，但通过直接 ONNX Runtime 后端创建稳定配置的 session，并且只创建一次后复用。
```

该方案已通过：

- 本机单元测试。
- 本机 ONNX 后端冒烟测试。
- 开发板 ONNX 加载测试。
- 开发板真实摄像头 50 帧连续推理测试。

后续还需要在完整 Web 控制台中验证：

- `/video_feed` 视频流正常。
- Web 按钮开启/关闭 YOLO 正常。
- 检测框位置与实际画面一致。
- 外设装配后完整巡检流程正常。
