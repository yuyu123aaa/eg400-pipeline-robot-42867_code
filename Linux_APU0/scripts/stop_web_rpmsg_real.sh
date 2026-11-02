#!/bin/sh
# EG400 Phase C: 停止 Web + 真实 RPMsg Bridge
set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LOG_DIR="$SCRIPT_DIR/../logs"

stop_pid_file() {
    pid_file=$1
    if [ -f "$pid_file" ]; then
        pid=$(cat "$pid_file")
        kill "$pid" 2>/dev/null || true
        rm -f "$pid_file"
        echo "stopped $pid_file (pid=$pid)"
    fi
}

stop_pid_file "$LOG_DIR/web_rpmsg_real.pid"
stop_pid_file "$LOG_DIR/rpmsg_bridge_real.pid"

# 2026-09-21: 按名字兜底清理——pidfile 可能失配，且不带 sudo 时无法杀 root
# 进程（kill 静默失败导致旧代码进程霸占 5000），必须以 root 运行本脚本。
pkill -f "eg400_linux_app/app/main.py" 2>/dev/null || true
pkill -f "eg400_rpmsg_bridge.py" 2>/dev/null || true

echo "Web + real RPMsg bridge stopped."
