# EG400 Linux 应用移植工作副本

## 1. 目录用途

本目录是从原飞腾派 Linux 项目复制出来的 EG400 适配工作副本。原始项目仍保留在：

```text
E:\水下机器人\集创赛飞腾\code\PhytiumLinux
```

本目录的修改不会替代原项目。原项目用于对照业务行为、网页接口、通信格式和已有测试；本目录用于后续 EG400 Linux/PS 端适配。

## 2. 副本范围

```text
eg400_linux_app/
|-- app/
|   |-- main.py
|   |-- config.py
|   |-- core/                 Python 业务模块副本
|   |-- dashboard/            网页模板、JS 和 CSS 副本
|   `-- static/               图表、3D 和模型资源副本
|-- models/
|   `-- best.onnx             ONNX 模型副本
|-- data/                     EG400 运行数据目录
|-- config/                   EG400 配置模板
|-- logs/                     运行日志目录
|-- scripts/                  Linux 操作脚本目录
|-- tests/                    原项目测试副本
|-- reference/
|   `-- legacy_phytium/       旧平台通信代码和文档，仅作参考
`-- README.md
```

已复制的旧平台参考内容位于 `reference/legacy_phytium/`，包括 `rpmsg-demo-single.c`、`controller_bridge.py`、`常用操作.md` 和原项目 `docs/`。`deploy_to_pi.ps1` 没有复制，因为它是飞腾派部署脚本，不能直接作为 EG400 部署脚本。

## 3. 当前限制

以下内容仍然是旧平台配置，不能直接用于 EG400：

```text
/dev/rpmsg_ctrl0
/dev/rpmsg0
/dev/ttyUSB0
192.168.10.11
CPU_SET(2)
原飞腾派 remoteproc/firmware 路径
```

EG400 上的实际 IP、摄像头节点、串口节点、RPMsg 节点、CPU 核号、设备树和裸机启动方式，必须以后续上板日志和官方资料为准。

## 4. 第一阶段：Web-only 冒烟验证

默认配置是安全的 Web-only 模式：只加载 Flask、网页模板和 API，不启动
YOLO、摄像头、RPMsg、手柄、温度或电机服务，也不会向裸机发送 UDP 控制指令。

进入 `app/` 后，网页验证使用以下方式：

```bash
cd /opt/eg400_linux_app/app
python3 web_smoke.py
```

Windows 本机使用对应虚拟环境的 Python：

```powershell
Set-Location -LiteralPath 'E:\水下机器人\eg400_linux_app\app'
& '..\.venv\Scripts\python.exe' web_smoke.py
```

Web-only 会跳过原 `main.py` 末尾的 `start_background_services()`，因此不会自动启动 YOLO、摄像头、RPMsg、手柄、温度和电机服务。

只有在确认 EG400 设备节点、通信协议、心跳和安全停车机制后，才允许评估以下组合：

```text
EG400_WEB_ONLY=0
EG400_SIMULATION_MODE=0
EG400_HARDWARE_CONTROL_ENABLED=1
```

三项缺一不可；未满足时通信层仍会拦截硬件指令。

模型和数据目录可通过 `EG400_MODEL_PATH`、`EG400_SAVE_DIR` 配置；默认使用
项目旁的 `models/best.onnx` 和 `data/`，不会依赖 `/home/user/Documents`。

浏览器访问：

```text
http://127.0.0.1:5000/
```

板端或虚拟机绑定 `0.0.0.0` 时，将地址替换成实际 IP。

当前项目统一的部署目标是：Windows 有线网卡 `192.168.137.1/24`，EG400 PS
网口使用 `192.168.137.101/24`，浏览器访问 `http://192.168.137.101:5000/`。
接口名不需要统一：当前 SD Ubuntu 中该网口是 `eth0`，当前 eMMC Buildroot 中是
`eth1`。SD 网络配置只改地址，不改接口名。

## 5. 后续顺序

```text
Web-only 页面
 -> Flask API 和模拟状态
 -> Linux Python 依赖
 -> 摄像头单帧
 -> MJPEG 视频
 -> best.onnx 单帧推理
 -> 视频和 YOLO 并行
 -> 图片/录像/缺陷记录
 -> 手柄和温度
 -> RPMsg 固定字符串回环
 -> Linux/裸机命令和状态协议
 -> 裸机控制联调
```

在 RPMsg 回环和裸机安全停车验证完成前，不能把网页控制动作连接到真实推进器。

## 6. 运行路径

建议运行时使用：

```text
/opt/eg400_linux_app/app
/opt/eg400_linux_app/models/best.onnx
/opt/eg400_linux_app/data
/opt/eg400_linux_app/logs
```

这些是部署建议，实际部署前需要根据 EG400 账号创建目录并设置权限和容量策略。

## 7. 来源记录

```text
来源：E:\水下机器人\集创赛飞腾\code\PhytiumLinux
目标：E:\水下机器人\eg400_linux_app
用途：EG400 Linux/PS 应用移植
复制日期：2026-09-04
```
