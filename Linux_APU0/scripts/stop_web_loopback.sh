#!/bin/sh
set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
LOG_DIR="$PROJECT_ROOT/logs"

stopped=0
for pid_file in "$LOG_DIR/web_loopback.pid" "$LOG_DIR/simulated_baremetal.pid"; do
    if [ -f "$pid_file" ]; then
        pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            stopped=1
        fi
        rm -f "$pid_file"
    fi
done

if [ "$stopped" -eq 1 ]; then
    echo "Web loopback stopped."
else
    echo "Web loopback was not running."
fi
