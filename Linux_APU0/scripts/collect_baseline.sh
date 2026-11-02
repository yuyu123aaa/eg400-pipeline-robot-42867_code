#!/usr/bin/env bash
set -u

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
LOG_DIR="${PROJECT_ROOT}/logs"
OUTPUT_FILE="${LOG_DIR}/eg400_linux_baseline.txt"

mkdir -p "$LOG_DIR"

{
    date
    echo '---- uname ----'
    uname -a
    echo '---- model ----'
    cat /proc/device-tree/model 2>&1 || true
    echo '---- cpuinfo ----'
    cat /proc/cpuinfo 2>&1 || true
    echo '---- nproc ----'
    nproc 2>&1 || true
    echo '---- memory ----'
    free -h 2>&1 || true
    echo '---- disk ----'
    df -h 2>&1 || true
    echo '---- network ----'
    echo 'target PS network: MAC e0:78:a3:01:02:03 = 192.168.137.101/24'
    ip addr 2>&1 || true
    ip route 2>&1 || true
    echo '---- network interfaces ----'
    ls -l /sys/class/net 2>&1 || true
    echo '---- device-tree ethernet aliases ----'
    for alias in /sys/firmware/devicetree/base/aliases/ethernet*; do
        [ -f "$alias" ] || continue
        printf '%s = ' "${alias##*/}"
        tr -d '\000' < "$alias"
        echo
    done
    echo '---- netplan ----'
    ls -la /etc/netplan 2>&1 || true
    grep -Rni '192\.168\.137\|eth[01]' /etc/netplan 2>&1 || true
    echo '---- usb ----'
    lsusb 2>&1 || true
    echo '---- devices ----'
    ls -l /dev/video* /dev/ttyUSB* /dev/ttyACM* /dev/ttyS* /dev/i2c-* /dev/rpmsg* 2>&1 || true
    echo '---- python ----'
    python3 --version 2>&1 || true
} | tee "$OUTPUT_FILE"

echo "Baseline saved to: $OUTPUT_FILE"
