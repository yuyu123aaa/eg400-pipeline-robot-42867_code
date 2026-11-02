# EG400 水下管道巡检机器人

> 参赛队 **42867** · 基于安路（Anlogic）DR1M90 FPSoC 的双核异构水下管道巡检系统

面向市政排水管道内部检测场景的水下机器人。整机为水平透明圆柱耐压舱，配 4 路水平推进器 + 2 路垂直推进器，通过前置摄像头与 NPU 实时识别管壁缺陷，Web 端同步呈现 3D 管廊、巡航轨迹、环境参数与巡检报告。

核心特点是**一颗芯片跑两个核**：APU0 运行 Linux（应用、Web、网络、视觉调度），APU1 运行裸机固件（电机 PWM、传感器总线、实时控制），两核经 RPMsg 通信，各干各擅长的事。

---

## 一、系统架构

```text
   浏览器 / Xbox 手柄
          │  HTTP 5000        UDP 9002
          ▼                     ▼
┌─────────────────────── APU0 · Linux (6.1.111-rt42, aarch64) ───────────────────────┐
│  Flask Web (main.py)          gamepad 接收              vision 取流                  │
│      │  ▲                        │                        │                         │
│      │  └── /api/data 轮询 ── 共享状态 config            ▼                         │
│      ▼                                                  帧中枢 /live（单连接 MJPEG） │
│  rpmsg_bridge  ◀── UDP 9001 指令 / UDP 9998 状态                                    │
│      │                                                                              │
│      └── Unix socket /tmp/eg400_apu1_cmd                                            │
│              │                                                                      │
│      heartbeat_daemon（独占 rpmsg 端点，500ms 一个心跳）                             │
└──────────────┼──────────────────────────────────────────────────────────────────────┘
               │  /dev/rpmsg_ctrl0  →  /dev/rpmsg0
┌──────────────┼───────────── APU1 · 裸机固件 ────────────────────────────────────────┐
│  电机控制（PCA9685 PWM）  超声测距（I2C）  姿态（UART）  气路阀泵  照明 LED          │
└─────────────────────────────────────────────────────────────────────────────────────┘

        摄像头 USB ──▶ yolo_web_camera（NPU 推理）──▶ 8080 MJPEG / 检测结果
```

数据流三条主线：

| 链路 | 走向 | 说明 |
|---|---|---|
| 控制下行 | Web/手柄 → 桥 → 心跳守护 → RPMsg → APU1 | 启动、前进、转向、下潜、灯光、气路采样 |
| 状态上行 | APU1 → RPMsg → 心跳守护 → 桥 → UDP 9998 → Web | 姿态、深度、里程、环境帧 |
| 视觉 | 摄像头 → NPU 推理 → 8080 流 + 检测结果 → Web | 缺陷识别、录像、拍照、3D 走廊抓帧 |

---

## 二、目录结构

```text
42867_代码/
├── Baremetal_APU1/             APU1 裸机工程（Vivado/Anlogic SDK 工程 + 驱动）
│   ├── BOOT.bin                启动镜像
│   ├── driver/                 Linux 侧 PL 驱动源码（dynclk / vdmafb / vtc）
│   ├── doc/                    硬件手册与参考资料
│   ├── sdk/ soc_sdk/ soc_hw/   芯片 SDK 与硬件描述
│   └── soc_prj/                工程文件
│
├── Linux_APU0/                 APU0 Linux 应用
│   ├── app/
│   │   ├── main.py             Flask 入口，页面 + 全部 API
│   │   ├── config.py           配置中心（环境变量驱动）
│   │   ├── core/               业务模块（见下表）
│   │   ├── dashboard/          前端：index.html / app.js / corridor.js / style.css
│   │   └── static/             echarts、three.js、机器人模型
│   ├── scripts/                一键启动、双核拉起、桥接、心跳守护
│   ├── models/best.onnx        ONNX 模型（PC 端推理/参考）
│   ├── config/                 环境变量模板
│   ├── data/                   巡检数据（每次巡检一个 Inspect_ 目录）
│   └── tests/                  单元测试
│
└── 视觉_NPU/                    NPU 视觉链路
    └── 11111/
        ├── yolo/               训练、导出、量化（QDQ UINT8）、模型与配置
        ├── npu_flow/           C++ 推理与推流程序（yolo_web_camera）
        ├── build.py            构建脚本
        └── 导出安路六输出YOLOv8_ONNX.py
```

`app/core/` 模块职责：

| 模块 | 职责 |
|---|---|
| `comm.py` / `rpmsg_transport.py` | 与桥接进程通信，解析状态帧与 ENV 帧 |
| `motor_control.py` | 巡检开始/结束、里程推算、航向偏置 |
| `corridor.py` | 3D 管廊断面采集、关键帧、缺陷展位、存档 |
| `defect_recorder.py` | 缺陷记录、空间去重、照片落盘 |
| `severity.py` / `evaluation.py` | 缺陷等级判定、CJJ 181 / T/CAS 731 符合性评估 |
| `management.py` | 年度体检、管网健康总览 |
| `gamepad.py` | 手柄数据解析与动作映射 |
| `vision.py` | 视觉服务取流、录像、拍照 |
| `calibration.py` / `temperature.py` / `mapping.py` | 标定、温度、航位推算 |

---

## 三、硬件组成

| 部件 | 规格 |
|---|---|
| 主控 | 安路 DR1M90 FPSoC（PS 双核异构 + PL），Linux 内核 6.1.111-rt42 |
| 视觉加速 | 片内 NPU（`cma_mem` / `hard_npu` / `soft_npu` 三级驱动） |
| 摄像头 | USB UVC（`/dev/video0`），1280×720 |
| 推进器 | 4 路水平（前后左右）+ 2 路垂直（下潜），PCA9685 PWM 驱动 |
| 测距 | I2C 超声模块（左/前/右三向） |
| 姿态 | UART 姿态模块（yaw / pitch / roll） |
| 环境检测 | 四合一气体（CO / H₂S / O₂ / CH₄）+ 深度 + 浊度 |
| 气路 | 采样阀 + 气泵（GPIO 控制，吸气 6 秒） |
| 供电 | 12V 锂电池直供 |
| 演示管道 | 绿色波纹软管，总长 ≈1.8 m，内径 45~55 cm |

---

## 四、快速开始

### 1. 部署到板卡

板卡默认地址 `192.168.137.100`，应用目录 `/home/uisrc/eg400_linux_app`。

```bash
# 本机 → 板卡
scp -r Linux_APU0/app       uisrc@192.168.137.100:eg400_linux_app/
scp -r Linux_APU0/scripts   uisrc@192.168.137.100:eg400_linux_app/
scp    Linux_APU0/models    uisrc@192.168.137.100:eg400_linux_app/

# 板上强制落盘（重要，见"常见问题"）
ssh uisrc@192.168.137.100 "sync"
```

视觉程序与模型放在 `/opt/vision/`（不在本仓库内直接覆盖，只同步二进制与配置）。

### 2. 一键启动

```bash
sudo sh ~/eg400_start_all.sh
```

六步依次执行：

```text
[1/6] 清理旧进程
[2/6] 加载 NPU 驱动（cma_mem → hard_npu → soft_npu）
[3/6] 放开设备权限（/dev/cma_mem、/dev/video0、/dev/ttyUSB0）
[4/6] 启动双核通信（写固件名 → start → 心跳守护）
[5/6] 启动 Web + 桥接
[6/6] 启动视觉程序（8080）
```

停止全部：

```bash
sudo sh ~/eg400_start_all.sh stop
```

### 3. 打开界面

```text
http://192.168.137.100:5000/
```

### 4. 三种运行模式

通过环境变量切换，默认是安全的 `web-only`：

| 模式 | 开关 | 行为 |
|---|---|---|
| `web-only` | `EG400_WEB_ONLY=1` | 只起 Flask 与静态资源，不发任何控制指令 |
| `loopback` | `EG400_LOOPBACK_MODE=1` | 起模拟裸机，本地闭环联调，不接硬件 |
| `simulation` | `EG400_SIMULATION_MODE=1` | 接真实桥接，但不下发执行器输出 |
| `hardware` | 以上全 0 + `EG400_HARDWARE_CONTROL_ENABLED=1` | 全功能实机 |

```bash
cd app && EG400_WEB_ONLY=0 EG400_SIMULATION_MODE=0 \
  EG400_HARDWARE_CONTROL_ENABLED=1 python3 main.py
```

---

## 五、功能一览

Web 端共 11 个视图：总览、实时画面、GIS 轨迹、3D 走廊、管廊总览、手动控制、PID 参数、日志、报告、历史、环境监测。

### 缺陷检测

YOLOv8n 量化后在 NPU 上推理，识别 5 类管壁缺陷：

| 类别 | 典型位置 |
|---|---|
| 裂缝 | 左壁 |
| 破洞 | 左壁 |
| 泥沙淤积 | 右壁 |
| 管壁脱落 | 右壁 |
| 树根侵入 | 顶部 |

每类在 3D 走廊中只贴一张（同一缺陷重复检出去重），按实拍的墙面方位贴壁，位置与出现时机跟随真实检测里程。

### 3D 管廊

- 管道**随前进逐段生长**：只有 `is_moving` 时每 0.25 m 追加一个断面，没走到的地方不渲染
- 轨迹按实物管道几何约束（0.9 m 直道 → 左转 90° → 转后直道），不吃遥测 yaw 的噪声；yaw 只驱动潜航器车头朝向
- 缺陷照片是**沿管壁弯曲的曲面贴片**，与管壁同一套平滑半径，随管壁长到该里程才出现
- HUD 里程在本次巡检结束后才报数

### 环境监测

APU1 上报 `ENV` 帧，Web 侧做采样保持（单帧 0 不覆盖上次有效值）：

```text
ENV:DEPTH:<mm>,TURB:<unt>,GAS:<co>,<h2s>,<o2_raw>,<ch4>
```

- `DEPTH=0` → 无数据，显示 `——`
- `O2 raw` 单位 0.1 %VOL，需 ÷10（209 → 20.9）
- 浊度 5 级分档：0~20 / 20~100 / 100~300 / 300~800 / 800~1000 UNT
- 报警线：O₂ 19.5~23.5 %VOL；CH₄ 20 黄 / 50 红；CO 35 / 200；H₂S 10 / 20

### 评估与报告

- 缺陷等级（1~4 级）判定
- CJJ 181 与 T/CAS 731 符合性自查
- 年度体检方案与管网健康总览
- 单次巡检生成 CSV + JSON + 缺陷照片 + 走廊关键帧，历史可回放

### 手柄控制

Windows 端运行 `Linux_APU0/start_gamepad.bat`（Xbox 手柄，USB 或蓝牙均可），经 UDP 9002 发给板卡。

| 按键 | 功能 |
|---|---|
| A | 系统启动 / 关闭 |
| B | 急停（最高优先级） |
| X | 手动模式 进 / 出 |
| Y | 缺陷自动记录 开 / 关 |
| LB / RB | 左 / 右照明灯 |
| Menu | 录像 开 / 关 |
| R3（右摇杆按下） | 拍照 |
| L3（左摇杆按下） | 重新标定 IMU 航向 |
| 十字键 ↑ / ↓ | 向前（开始巡检）/ 停止（结束巡检） |
| 十字键 ← / → | 航向 ±90° |
| RT / LT | 手动模式 油门前进 / 后退 |
| 左摇杆 左右 | 手动模式 转向 |
| 左摇杆 下推 | 下潜（回中即停，正浮力自然上浮） |

手柄断开自动退手动并停机；手动模式下 0.5 s 收不到包即急停。

---

## 六、通信协议

### RPMsg（双核）

- 端点：`/dev/rpmsg_ctrl0` 建端点 → `/dev/rpmsg0` 收发
- **一个端点只允许一个进程打开**，故由 `apu1_heartbeat_daemon.py` 独占，其余进程经 Unix 域套接字 `/tmp/eg400_apu1_cmd` 转发
- 心跳：500 ms 一个 PING，80 ms 超时，连续 5 次无回应判失联
- 字符设备不报 `EPOLLOUT`，写侧必须阻塞写

### 应用层

| 方向 | 格式 | 示例 |
|---|---|---|
| 控制下行 | `CMD:<动作>[:<参数>]` | `CMD:START`、`CMD:DIVE`、`CMD:YAW:LEFT` |
| 状态上行 | `KEY:VALUE` 逗号分隔 | `ENV:DEPTH:1200,TURB:35,...` |

Web API 主要路由：

```text
GET  /api/data                     整机状态（姿态、里程、环境、缺陷）
GET  /api/defect_logs              缺陷日志
GET  /api/corridor/current         当前巡检走廊数据
GET  /api/corridor/history/<dir>   历史走廊回放
GET  /api/history[/<dir>]          巡检历史
GET  /api/evaluation/current       本次评估
GET  /api/compliance/checklist     符合性自查
POST /api/control/<action>         控制指令
GET  /api/vision/*                 视觉状态、检测框、快照、录像
GET  /video_feed, /live            视频流（单连接帧中枢）
```

控制动作：`start` `stop` `led1on/off` `led2on/off` `gas_sample_start` `gas_stop` `record_toggle` `calibrate` `motor_start` `motor_stop` `yaw_left/right` `dive` `dive_stop` `manual_start/stop` `speed`

---

## 七、主要配置项

全部走环境变量，方便不改代码切换模式：

| 变量 | 默认 | 说明 |
|---|---|---|
| `EG400_WEB_HOST` / `EG400_WEB_PORT` | `0.0.0.0` / `5000` | Web 监听 |
| `EG400_WEB_ONLY` | `1` | 只起 Web，不发控制 |
| `EG400_HARDWARE_CONTROL_ENABLED` | `0` | 是否允许真实执行器输出 |
| `EG400_CMD_HOST` / `EG400_CMD_PORT` | `127.0.0.1` / `9001` | 给桥接发指令 |
| `EG400_UDP_PORT` | `9998` | 收桥接状态 |
| `EG400_TRAJECTORY_MAX_M` | `1.8` | 里程上限 |
| `EG400_CORRIDOR_FIXED_TRACK` | `1` | 走廊按实物管道几何约束（`0` 回退自由积分） |
| `EG400_SAVE_DIR` | `data/` | 巡检数据根目录 |

---

## 八、常见问题

这几个坑都是实际调试踩出来的，留档备查。

**启动脚本在随机位置 `Connection reset`，`uptime` 归零**
先查供电。失败点乱跑（这次 `[5/6]`、下次 `[6/6]`、下次脚本开头）基本是供电或接触问题；固定在同一行才是代码。用万用表带载测一下输入电压。

**接全外设就整板重启**
APU1 固件开了硬件看门狗（RESET 模式，约 4 s）。一旦主循环被 I²C/UART 访问阻塞（外设未接或应答异常），喂狗停止，整芯片被复位。排查时逐路接外设定位，根治要让喂狗挂在独立定时器上、所有总线访问带超时。

**Web 黑屏**
按顺序查：① 板上文件是否完整（`md5sum` + `wc -c` + `tail -1` 三查）→ ② 服务端出口 `curl` 字节数是否一致 → ③ 浏览器 `Ctrl+Shift+R` / 无痕窗口。`index.html` 是 Jinja2 模板，覆盖途中被请求读到半截会缓存在内存里，此时 md5 再对也黑屏，必须重启 Web。

**传上去的文件长度对但 Python 报 `null bytes`**
掉电复位使未落盘的数据块读出全为 `0x00`。scp 后立刻在板上 `sync`，再校验。

**改了配置不生效**
ENV 帧是在桥接进程里解析的，只重启 Web 没用；`app/comm.py` 与 `core/comm.py` 是两份副本，改动要同步。

**`pkill -f main.py` 把自己 ssh 会话杀掉**
远程命令行文本里也含 `main.py`，会被自己匹配到。写成 `main[.]py`，并把"落位"和"重启"拆成两条 ssh。

**视频掉、拍照失败**
视觉程序的 MJPEG 只支持一个连接。浏览器开着画面再点拍照，第二条连接被回 503。现由后台常驻的帧中枢统一取流、最新帧放内存，视频/拍照/走廊抓帧都从这取。

---

## 九、说明

- `data/`、`logs/`、`.venv/`、编译中间产物不进版本库
- 视觉模型与固件二进制随仓库分发，仅供本作品复现使用

---

**42867 队** · 2026
