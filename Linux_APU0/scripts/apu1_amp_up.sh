#!/bin/sh
# =============================================================================
# apu1_amp_up.sh —— 一键把 AMP 链路拉起来（Linux 侧主动开门 + 持续心跳）
# =============================================================================
#  这个脚本解决的核心问题：
#    APU1 上电后进主循环第一件事就是"等 Linux 敲门"（等 SGI 14）。
#    而 `echo start` **只把 APU1 从复位里放出来，不会敲门**。
#    谁先写出第一帧 RPMsg，谁才产生第一脚 SGI。
#
#  所以顺序必须是（对齐飞腾 start.sh 的做法）：
#      ① 设固件名  ② start  ③ 等节点出现  ④ 【马上】发 WakeUp + 开始 500ms 心跳
#
#  历史教训：旧脚本在这中间插了 `sleep 3` + 手工跑 rpmsg_demo，
#  而 APU1 的看门狗超时只有 4.03 秒 → APU1 还没等到敲门就被复位了。
#  本脚本把这个空档压到最小（等节点最多 20s，但一旦节点出现立刻发）。
#
#  用法：
#    sh scripts/apu1_amp_up.sh                 # 默认固件 apu1_proto_v1_liveclock.elf
#    FW_NAME=apu1_proto_v1.elf sh scripts/apu1_amp_up.sh
#    sh scripts/apu1_amp_up.sh --no-start      # 只灌心跳，不碰 remoteproc
# =============================================================================

set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SELF_DIR/.." && pwd)
LOG_DIR="$PROJECT_ROOT/logs"

RPROC="${EG400_RPROC:-/sys/class/remoteproc/remoteproc0}"
FW_NAME="${FW_NAME:-apu1_proto_v1_liveclock.elf}"
FW_DIR="${FW_DIR:-/lib/firmware}"
HB_WAIT_DEVICE="${EG400_HB_WAIT_DEVICE:-20}"
NO_START=0

for a in "$@"; do
    case "$a" in
        --no-start) NO_START=1 ;;
        -h|--help)  sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "未知参数: $a"; exit 1 ;;
    esac
done

PYTHON_BIN="${PYTHON_BIN:-python3}"
if [ -x "$PROJECT_ROOT/.venv/bin/python" ]; then
    PYTHON_BIN="$PROJECT_ROOT/.venv/bin/python"
fi

mkdir -p "$LOG_DIR"
HB_LOG="$LOG_DIR/heartbeat_daemon.log"
HB_PID="$LOG_DIR/heartbeat_daemon.pid"

echo "=============================================================="
echo " APU1 AMP 一键拉起"
echo "   固件      : $FW_NAME"
echo "   remoteproc: $RPROC"
echo "   Python    : $PYTHON_BIN"
echo "=============================================================="

# ---------------------------------------------------------------------------
# 第 0 步：先把上一次残留的心跳进程收掉（否则它占着 /dev/rpmsg0）
# ---------------------------------------------------------------------------
if [ -f "$HB_PID" ] && kill -0 "$(cat "$HB_PID")" 2>/dev/null; then
    echo "[0] 停掉上一次的心跳守护 (pid $(cat "$HB_PID"))"
    kill "$(cat "$HB_PID")" 2>/dev/null || true
    sleep 0.5
fi

if [ "$NO_START" -eq 0 ]; then
    # -----------------------------------------------------------------------
    # 第 1 步：设固件名
    # -----------------------------------------------------------------------
    if [ ! -d "$RPROC" ]; then
        echo "[1] ✗ 找不到 $RPROC"
        echo "    内核可能没使能 remoteproc，或设备名不是 remoteproc0。"
        echo "    先跑：ls /sys/class/remoteproc/"
        exit 1
    fi

    if [ ! -f "$FW_DIR/$FW_NAME" ]; then
        echo "[1] ✗ $FW_DIR/$FW_NAME 不存在"
        echo "    先把这个固件拷进去："
        echo "      sudo cp <你的固件> $FW_DIR/$FW_NAME"
        echo "    当前 $FW_DIR 里的 apu1 固件："
        ls -l "$FW_DIR" 2>/dev/null | grep -i apu || echo "      (没有)"
        exit 1
    fi

    echo "[1] 写入固件名: $FW_NAME"
    echo "$FW_NAME" | sudo tee "$RPROC/firmware" >/dev/null

    # 若已经在跑，先停掉，保证是干净的一次启动
    STATE_NOW=$(cat "$RPROC/state" 2>/dev/null || echo unknown)
    if [ "$STATE_NOW" = "running" ]; then
        echo "[1] 固件当前已在 running，先 stop 再重来"
        echo stop | sudo tee "$RPROC/state" >/dev/null
        sleep 0.5
    fi

    # -----------------------------------------------------------------------
    # 第 2 步：start（★ 这一步之后要尽快灌心跳，中间不要插 sleep）
    # -----------------------------------------------------------------------
    echo "[2] start（APU1 会被放出来，它马上就开始等我们敲门）"
    echo start | sudo tee "$RPROC/state" >/dev/null

    # -----------------------------------------------------------------------
    # 第 3 步：等 remoteproc 进 running（通常几百毫秒）
    # -----------------------------------------------------------------------
    i=0
    while [ "$i" -lt 50 ]; do
        STATE=$(cat "$RPROC/state" 2>/dev/null || echo unknown)
        [ "$STATE" = "running" ] && break
        i=$((i + 1))
        sleep 0.1
    done
    echo "[3] remoteproc state = ${STATE:-unknown}"
    if [ "${STATE:-}" != "running" ]; then
        echo "    ✗ 没进 running，看内核日志："
        echo "      dmesg | tail -40"
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# 第 4 步：立刻起心跳守护（它内部会：等节点 → 发 WakeUp → 每 500ms PING）
# ---------------------------------------------------------------------------
#  ★ 关掉守护自己的 UDP 命令口（--no-udp-command）：
#     之后 run_web_rpmsg_real.sh 起的 Web 桥要绑 9001，
#     两个进程绑同一个 UDP 端口会有一个失败。
#     指令统一走 Unix socket（桥 → 守护），所以不必留 UDP 命令口。
#     想手工用 UDP 9001 打指令时，加 EG400_HB_UDP_CMD=1 再跑本脚本。
# ---------------------------------------------------------------------------
HB_UDP_CMD_FLAG="--no-udp-command"
if [ "${EG400_HB_UDP_CMD:-0}" = "1" ]; then
    HB_UDP_CMD_FLAG=""
fi

echo "[4] 启动心跳守护（等到节点就立刻发 WakeUp，之后 500ms 一个 PING）"
nohup "$PYTHON_BIN" "$SELF_DIR/apu1_heartbeat_daemon.py" \
    --wait-device "$HB_WAIT_DEVICE" \
    $HB_UDP_CMD_FLAG \
    -v \
    >"$HB_LOG" 2>&1 &
echo $! > "$HB_PID"
echo "    pid=$(cat "$HB_PID")  log=$HB_LOG"

# 观察 3 秒，把关键几行打出来给用户看
sleep 3
echo "--------------------------------------------------------------"
echo " 心跳守护输出（最近 15 行）："
tail -n 15 "$HB_LOG" 2>/dev/null || true
echo "--------------------------------------------------------------"

if kill -0 "$(cat "$HB_PID")" 2>/dev/null; then
    echo "✓ 心跳守护在跑"
else
    echo "✗ 心跳守护已退出 —— 完整日志："
    cat "$HB_LOG" 2>/dev/null || true
    exit 1
fi

echo
echo " 现在去看 APU1 串口，应该看到："
echo "   [APU1][poll] waiting for APU0 kick 1s (SGI rx=0)   ← 修复前会一直 0"
echo "   [APU1] SGI 14 received from APU0 (expect 14)        ← ★ 敲门成功"
echo "   [APU1][poll] ... (SGI rx=1)                          ← SGI 计数开始涨"
echo "   ← PONG 1                                             ← 心跳往返成立"
echo
echo " 停止：sh $SELF_DIR/apu1_amp_down.sh"
