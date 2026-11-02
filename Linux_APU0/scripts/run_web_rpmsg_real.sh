#!/bin/sh
# EG400 Phase C: Web + 真实 RPMsg 硬件后端启动脚本
# =============================================================================
#  ★ 2026-09-25 改：前置条件变了
# =============================================================================
#  旧版要求"先手工跑 rpmsg_demo 把 /dev/rpmsg0 建出来" —— 那是错的做法，
#  因为「内核规定一个 rpmsg 端点设备只能被一个进程 open」
#  （rpmsg_char.c 的 rpmsg_eptdev_open 里 return -EBUSY），
#  两个进程同时用会 open 失败、或者互相抢收消息。
#
#  现在改成"单所有者"模式（等价于飞腾单进程多线程共用一个 fd）：
#     ① 先跑 apu1_amp_up.sh —— 它拉起心跳守护，那个进程独占 /dev/rpmsgN，
#        负责敲门(WakeUp)、发心跳、收所有回帧
#     ② 再跑本脚本 —— 桥**不自己开设备**，而是把指令经 Unix socket
#        （默认 /tmp/eg400_apu1_cmd）交给心跳守护代发；
#        APU1 的回帧由守护收下后从 UDP 状态口发出，所以 Web 侧照常工作
#
#  用法：
#    sh scripts/apu1_amp_up.sh          # ① 先拉起心跳守护
#    sh scripts/run_web_rpmsg_real.sh   # ② 再起 Web + 桥
# =============================================================================
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
APP_DIR="$PROJECT_ROOT/app"
LOG_DIR="$PROJECT_ROOT/logs"

WEB_PID_FILE="$LOG_DIR/web_rpmsg_real.pid"
BRIDGE_PID_FILE="$LOG_DIR/rpmsg_bridge_real.pid"
WEB_LOG="$LOG_DIR/web_rpmsg_real.log"
BRIDGE_LOG="$LOG_DIR/rpmsg_bridge_real.log"

# 心跳守护建的 Unix 指令通道（两边必须一致）
CMD_UNIX=${EG400_CMD_UNIX:-/tmp/eg400_apu1_cmd}
WEB_PORT=${EG400_WEB_PORT:-5000}
CMD_PORT=${EG400_CMD_PORT:-9001}
UDP_PORT=${EG400_UDP_PORT:-9998}

if [ -x "$PROJECT_ROOT/.venv/bin/python" ]; then
    PYTHON_BIN="$PROJECT_ROOT/.venv/bin/python"
else
    PYTHON_BIN=${PYTHON_BIN:-python3}
fi

# ---- 前置检查：心跳守护必须在跑（它才是设备所有者）----
if [ ! -S "$CMD_UNIX" ]; then
    echo "[ERROR] 心跳守护的 Unix 指令通道不存在: $CMD_UNIX"
    echo "  说明心跳守护还没起来（或者路径不一致）。请先执行："
    echo "    1) sh $SCRIPT_DIR/apu1_amp_up.sh        # 拉起心跳守护（含 WakeUp 敲门）"
    echo "    2) ls -l $CMD_UNIX                       # 确认 socket 出现"
    echo "  然后重跑本脚本。"
    echo
    echo "  为什么要这样：内核规定一个 rpmsg 端点设备只能被一个进程打开，"
    echo "  所以心跳守护独占 /dev/rpmsgN，桥只能经它转发指令。"
    exit 1
fi

mkdir -p "$LOG_DIR"
rm -f "$WEB_PID_FILE" "$BRIDGE_PID_FILE"

# ---- 进程1：Bridge（经心跳守护转发；心跳由守护负责，桥不再自己发）----
nohup "$PYTHON_BIN" "$SCRIPT_DIR/eg400_rpmsg_bridge.py" \
    --backend daemon \
    --daemon-cmd-unix "$CMD_UNIX" \
    --command-port "$CMD_PORT" \
    --status-port "$UDP_PORT" \
    >"$BRIDGE_LOG" 2>&1 &
echo $! > "$BRIDGE_PID_FILE"

# ---- 进程2：Web（安全组合：命令放行、外设禁用） ----
nohup env \
    EG400_WEB_ONLY=0 \
    EG400_LOOPBACK_MODE=0 \
    EG400_SIMULATION_MODE=0 \
    EG400_HARDWARE_CONTROL_ENABLED=1 \
    EG400_WEB_HOST=0.0.0.0 \
    EG400_WEB_PORT="$WEB_PORT" \
    EG400_CMD_HOST=127.0.0.1 \
    EG400_CMD_PORT="$CMD_PORT" \
    EG400_UDP_PORT="$UDP_PORT" \
    "$PYTHON_BIN" "$APP_DIR/main.py" \
    >"$WEB_LOG" 2>&1 &
echo $! > "$WEB_PID_FILE"

sleep 1
echo "Web + 桥（经心跳守护）已启动。"
echo "URL: http://0.0.0.0:${WEB_PORT}/"
echo "Bridge log: $BRIDGE_LOG   (tail -f 看指令转发)"
echo "心跳守护日志: $LOG_DIR/heartbeat_daemon.log  (看 PONG)"
echo "Web log:    $WEB_LOG"
echo "停止: sh $SCRIPT_DIR/stop_web_rpmsg_real.sh"
