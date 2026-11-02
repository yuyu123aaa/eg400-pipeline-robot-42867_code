#!/bin/sh
set -u

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
APP_DIR="${PROJECT_ROOT}/app"
LOG_DIR="${PROJECT_ROOT}/logs"
PID_FILE="${LOG_DIR}/web_smoke.pid"
LOG_FILE="${LOG_DIR}/web_smoke.log"
BOARD_IP="${EG400_BOARD_IP:-192.168.137.101}"
WEB_PORT="${EG400_WEB_PORT:-5000}"

if [ -x "${PROJECT_ROOT}/.venv/bin/python" ]; then
    PYTHON_BIN="${PROJECT_ROOT}/.venv/bin/python"
else
    PYTHON_BIN="python3"
fi

mkdir -p "$LOG_DIR"

if [ -f "$PID_FILE" ]; then
    old_pid="$(cat "$PID_FILE")"
    if kill -0 "$old_pid" 2>/dev/null; then
        echo "Web smoke service is already running: PID $old_pid"
        exit 0
    fi
    rm -f "$PID_FILE"
fi

cd "$APP_DIR"
nohup "$PYTHON_BIN" web_smoke.py > "$LOG_FILE" 2>&1 &
echo $! > "$PID_FILE"
echo "Web smoke service started: PID $(cat "$PID_FILE")"
echo "Python: $PYTHON_BIN"
echo "Log: $LOG_FILE"
echo "Board URL: http://${BOARD_IP}:${WEB_PORT}/"
