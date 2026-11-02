#!/bin/sh

# Read-only preflight for the intended physical layout:
# APU Core0 = bare-metal, APU Core1 = Linux.
#
# Linux logical CPU numbers (cpu0, cpu1, ...) are not proof of the physical
# APU core.  A one-core Linux started on physical APU1 may still enumerate that
# core as Linux logical cpu0.  Physical identity must be checked from the boot
# image, FSBL log and the device-tree CPU reg values.

set -u

LINUX_PHYSICAL_APU=1
BAREMETAL_PHYSICAL_APU=0
FAILURES=0

fail() {
    echo "FAIL: $1"
    FAILURES=$((FAILURES + 1))
}

warn() {
    echo "WARN: $1"
}

echo "===== requested physical APU layout ====="
echo "Linux physical APU Core: $LINUX_PHYSICAL_APU"
echo "Bare-metal physical APU Core: $BAREMETAL_PHYSICAL_APU"
echo "Linux logical cpuN names are not physical-core proof."

echo "===== kernel ====="
uname -a
if [ -r /proc/cmdline ]; then
    echo "cmdline: $(cat /proc/cmdline)"
fi

echo "===== CPU topology ====="
if [ -r /sys/devices/system/cpu/possible ]; then
    echo "possible: $(cat /sys/devices/system/cpu/possible)"
fi
if [ -r /sys/devices/system/cpu/present ]; then
    echo "present: $(cat /sys/devices/system/cpu/present)"
fi
if [ -r /sys/devices/system/cpu/online ]; then
    ONLINE="$(cat /sys/devices/system/cpu/online)"
    echo "online: $ONLINE"
else
    fail "/sys/devices/system/cpu/online is unavailable"
    ONLINE=""
fi

if [ -r /sys/devices/system/cpu/cpu0/online ]; then
    echo "cpu0 online flag: $(cat /sys/devices/system/cpu/cpu0/online)"
else
    echo "cpu0 online flag: unavailable (boot CPU commonly has no flag)"
fi

if [ -r /sys/devices/system/cpu/cpu1/online ]; then
    echo "Linux logical cpu1 online flag: $(cat /sys/devices/system/cpu/cpu1/online)"
else
    echo "Linux logical cpu1 online flag: unavailable"
fi

echo "===== device-tree CPU identities (raw) ====="
DT_CPU_ROOT=/sys/firmware/devicetree/base/cpus
if [ ! -d "$DT_CPU_ROOT" ]; then
    DT_CPU_ROOT=/proc/device-tree/cpus
fi
if [ -d "$DT_CPU_ROOT" ]; then
    for node in "$DT_CPU_ROOT"/cpu@*; do
        [ -d "$node" ] || continue
        echo "node: ${node##*/}"
        if [ -r "$node/reg" ]; then
            if command -v od >/dev/null 2>&1; then
                printf "  reg bytes (big-endian): "
                od -An -tx1 "$node/reg" 2>/dev/null | tr -s ' ' | tr '\n' ' '
                echo
            else
                echo "  reg bytes: unavailable (od command is absent)"
            fi
        fi
        if [ -r "$node/status" ]; then
            printf "  status: "
            tr -d '\000' < "$node/status"
            echo
        fi
    done
else
    fail "device-tree CPU nodes are unavailable"
fi

echo "===== boot arguments relevant to CPU assignment ====="
if [ -r /proc/cmdline ]; then
    CMDLINE="$(cat /proc/cmdline)"
    case " $CMDLINE " in
        *" maxcpus="*|*" nosmp"*)
            fail "current boot arguments already restrict Linux CPU bring-up"
            ;;
        *)
            echo "no maxcpus=1/nosmp restriction detected"
            ;;
    esac
    case " $CMDLINE " in
        *" isolcpus="*|*" nohz_full="*|*" rcu_nocbs="*)
            echo "isolation arguments: present"
            ;;
        *)
            echo "isolation arguments: not present"
            ;;
    esac
fi

echo "===== possible Linux-to-firmware interfaces ====="
REMOTE_PROC_FOUND=0
if [ -d /sys/class/remoteproc ]; then
    echo "remoteproc class: present"
    for node in /sys/class/remoteproc/remoteproc*; do
        [ -e "$node" ] || continue
        REMOTE_PROC_FOUND=1
        echo "remoteproc node: $node"
        [ -r "$node/state" ] && echo "  state: $(cat "$node/state")"
        [ -r "$node/firmware" ] && echo "  firmware: $(cat "$node/firmware")"
    done
    if [ "$REMOTE_PROC_FOUND" -eq 0 ]; then
        fail "remoteproc class exists, but no remoteproc instance is registered"
    fi
else
    fail "remoteproc class is absent"
fi

RPMSG_FOUND=0
for node in /dev/rpmsg*; do
    if [ -e "$node" ]; then
        echo "rpmsg device: $node"
        RPMSG_FOUND=1
    fi
done
if [ "$RPMSG_FOUND" -eq 0 ]; then
    fail "no /dev/rpmsg* device is present"
fi

echo "===== reserved memory ====="
FIRMWARE_MEMORY_FOUND=0
if [ -d /proc/device-tree/reserved-memory ]; then
    ls -la /proc/device-tree/reserved-memory 2>/dev/null || true
    for node in /proc/device-tree/reserved-memory/*; do
        [ -e "$node" ] || continue
        case "$node" in
            */linux,cma|*/#address-cells|*/#size-cells|*/name|*/ranges)
                continue
                ;;
        esac
        echo "dedicated reserved-memory node: $node"
        FIRMWARE_MEMORY_FOUND=1
    done
    if [ "$FIRMWARE_MEMORY_FOUND" -eq 0 ]; then
        fail "reserved-memory contains only Linux CMA; no dedicated firmware region is evidenced"
    fi
else
    fail "/proc/device-tree/reserved-memory is absent; a firmware shared-memory region is not evidenced"
fi

echo "===== result ====="
if [ "$FAILURES" -ne 0 ]; then
    echo "NOT READY: no CPU state was changed"
    exit 2
fi

echo "PRECHECK ONLY: no CPU state was changed"
echo "A passing precheck still does not start bare-metal on physical APU0 or move Linux to physical APU1."
echo "It also cannot prove physical-core ownership from Linux logical cpuN names alone."
exit 0
