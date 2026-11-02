#!/bin/sh
# =============================================================================
# apu1_amp_down.sh —— 停止心跳守护，并把 APU1 从 running 里停掉
# =============================================================================
#  用法：
#    sh scripts/apu1_amp_down.sh            # 停心跳 + stop APU1
#    sh scripts/apu1_amp_down.sh --keep-fw  # 只停心跳，让 APU1 继续跑
# =============================================================================

set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SELF_DIR/.." && pwd)
LOG_DIR="$PROJECT_ROOT/logs"
RPROC="${EG400_RPROC:-/sys/class/remoteproc/remoteproc0}"
HB_PID="$LOG_DIR/heartbeat_daemon.pid"
KEEP_FW=0

for a in "$@"; do
    case "$a" in
        --keep-fw) KEEP_FW=1 ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "未知参数: $a"; exit 1 ;;
    esac
done

# ---- 1) 停心跳守护（先停它，免得一边发心跳一边关核）----
if [ -f "$HB_PID" ]; then
    PID=$(cat "$HB_PID")
    if kill -0 "$PID" 2>/dev/null; then
        echo "[1] 停心跳守护 pid=$PID"
        kill "$PID" 2>/dev/null || true
        sleep 0.5
        kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null || true
    else
        echo "[1] 心跳守护已不在（stale pid file）"
    fi
    rm -f "$HB_PID"
else
    echo "[1] 没有心跳守护 pid 文件"
fi

# ---- 2) 兜底：把可能残留的同类进程也收掉 ----
if command -v pkill >/dev/null 2>&1; then
    pkill -f "apu1_heartbeat_daemon.py" 2>/dev/null || true
fi

# ---- 3) 可选：把 APU1 停掉 ----
if [ "$KEEP_FW" -eq 0 ]; then
    if [ -d "$RPROC" ]; then
        echo "[2] stop APU1"
        echo stop | sudo tee "$RPROC/state" >/dev/null 2>&1 || true
        sleep 0.5
        echo "    现在 state = $(cat "$RPROC/state" 2>/dev/null || echo unknown)"
    fi
else
    echo "[2] --keep-fw：APU1 继续跑（注意：没有心跳后 2.5s 它会安全停车）"
fi

echo "✓ 完成"
