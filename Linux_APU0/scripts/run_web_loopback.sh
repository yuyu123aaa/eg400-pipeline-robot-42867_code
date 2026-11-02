#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
APP_DIR="$PROJECT_ROOT/app"
LOG_DIR="$PROJECT_ROOT/logs"
WEB_PID_FILE="$LOG_DIR/web_loopback.pid"
BRIDGE_PID_FILE="$LOG_DIR/simulated_baremetal.pid"
WEB_LOG="$LOG_DIR/web_loopback.log"
BRIDGE_LOG="$LOG_DIR/simulated_baremetal.log"

BOARD_IP=${EG400_BOARD_IP:-192.168.137.101}
WEB_PORT=${EG400_WEB_PORT:-5000}
CMD_PORT=${EG400_CMD_PORT:-9001}
UDP_PORT=${EG400_UDP_PORT:-9998}
SONAR_FAULT_MASK=${EG400_LOOPBACK_SONAR_FAULT_MASK:-0}

if [ -x "$PROJECT_ROOT/.venv/bin/python" ]; then
    PYTHON_BIN="$PROJECT_ROOT/.venv/bin/python"
else
    PYTHON_BIN=${PYTHON_BIN:-python3}
fi

mkdir -p "$LOG_DIR"

is_running() {
    pid_file=$1
    [ -f "$pid_file" ] || return 1
    pid=$(cat "$pid_file")
    kill -0 "$pid" 2>/dev/null
}

if is_running "$WEB_PID_FILE" || is_running "$BRIDGE_PID_FILE"; then
    echo "Loopback service is already running."
    echo "Web PID file: $WEB_PID_FILE"
    echo "Bridge PID file: $BRIDGE_PID_FILE"
    exit 0
fi

rm -f "$WEB_PID_FILE" "$BRIDGE_PID_FILE"

nohup "$PYTHON_BIN" "$SCRIPT_DIR/simulated_baremetal.py" \
    --command-host 127.0.0.1 \
    --command-port "$CMD_PORT" \
    --status-host 127.0.0.1 \
    --status-port "$UDP_PORT" \
    --sonar-fault-mask "$SONAR_FAULT_MASK" \
    >"$BRIDGE_LOG" 2>&1 &
echo $! > "$BRIDGE_PID_FILE"

nohup env \
    EG400_WEB_ONLY=0 \
    EG400_LOOPBACK_MODE=1 \
    EG400_SIMULATION_MODE=1 \
    EG400_HARDWARE_CONTROL_ENABLED=0 \
    EG400_WEB_HOST=0.0.0.0 \
    EG400_WEB_PORT="$WEB_PORT" \
    EG400_CMD_HOST=127.0.0.1 \
    EG400_CMD_PORT="$CMD_PORT" \
    EG400_UDP_PORT="$UDP_PORT" \
    "$PYTHON_BIN" "$APP_DIR/main.py" \
    >"$WEB_LOG" 2>&1 &
echo $! > "$WEB_PID_FILE"

sleep 1
if ! is_running "$WEB_PID_FILE"; then
    echo "Web loopback failed to start."
    tail -n 40 "$WEB_LOG" 2>/dev/null || true
    kill "$(cat "$BRIDGE_PID_FILE")" 2>/dev/null || true
    rm -f "$WEB_PID_FILE" "$BRIDGE_PID_FILE"
    exit 1
fi

echo "Web loopback started."
echo "URL: http://${BOARD_IP}:${WEB_PORT}/"
echo "Web log: $WEB_LOG"
echo "Simulated bare-metal log: $BRIDGE_LOG"
echo "Stop: $SCRIPT_DIR/stop_web_loopback.sh"
