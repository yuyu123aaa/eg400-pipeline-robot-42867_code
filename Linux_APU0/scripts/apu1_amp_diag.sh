#!/bin/sh
# =============================================================================
# apu1_amp_diag.sh —— AMP 链路一键体检（出问题时先跑这个，把输出发给队友）
# =============================================================================
#  只读脚本：不改任何东西，只把关键状态打出来。
#  用法：sh scripts/apu1_amp_diag.sh
# =============================================================================

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SELF_DIR/.." && pwd)
RPROC="${EG400_RPROC:-/sys/class/remoteproc/remoteproc0}"

hr() { echo "--------------------------------------------------------------"; }
sec() { echo; echo "### $1"; }

echo "=============================================================="
echo " APU1 AMP 链路体检   $(date '+%Y-%m-%d %H:%M:%S')"
echo "=============================================================="

sec "1. 主机/内核"
echo "uname : $(uname -a)"
echo "cpus  : $(grep -c ^processor /proc/cpuinfo 2>/dev/null || echo '?') 个核（AMP 下 Linux 应只用 core0）"

sec "2. remoteproc 状态"
if [ -d "$RPROC" ]; then
    echo "firmware : $(cat "$RPROC/firmware" 2>/dev/null)"
    echo "state    : $(cat "$RPROC/state" 2>/dev/null)"
    echo "name     : $(cat "$RPROC/name" 2>/dev/null)"
    echo "可用 remoteproc: $(ls /sys/class/remoteproc/ 2>/dev/null | tr '\n' ' ')"
else
    echo "✗ 找不到 $RPROC"
    echo "  可用：$(ls /sys/class/remoteproc/ 2>/dev/null || echo 无)"
fi

sec "3. RPMsg 设备节点"
echo "/dev/rpmsg*        : $(ls -l /dev/rpmsg* 2>/dev/null | tr '\n' ' ' || echo '无 ★ 这是关键')"
echo "/sys/bus/rpmsg/devices/ :"
ls /sys/bus/rpmsg/devices/ 2>/dev/null | sed 's/^/    /' || echo "    (空)"

sec "4. 固件文件"
FW="${EG400_FW_NAME:-apu1_proto_v1_liveclock.elf}"
if [ -f "/lib/firmware/$FW" ]; then
    echo "$FW :"
    ls -l "/lib/firmware/$FW"
    echo "sha256: $(sha256sum "/lib/firmware/$FW" | awk '{print $1}')"
else
    echo "✗ /lib/firmware/$FW 不存在"
fi
echo "/lib/firmware 下的 apu1 固件："
ls -l /lib/firmware 2>/dev/null | grep -i apu | sed 's/^/    /' || echo "    (没有)"

sec "5. 心跳守护进程"
if [ -f "$PROJECT_ROOT/logs/heartbeat_daemon.pid" ]; then
    PID=$(cat "$PROJECT_ROOT/logs/heartbeat_daemon.pid")
    if kill -0 "$PID" 2>/dev/null; then
        echo "✓ 在跑 pid=$PID"
    else
        echo "✗ pid 文件存在但进程已死（stale）"
    fi
else
    echo "✗ 没有 pid 文件 —— 心跳守护没起来"
fi
echo "日志尾部（logs/heartbeat_daemon.log）："
tail -n 20 "$PROJECT_ROOT/logs/heartbeat_daemon.log" 2>/dev/null | sed 's/^/    /' || echo "    (无日志)"

sec "6. 内核日志里的 remoteproc / rpmsg / virtio"
if command -v dmesg >/dev/null 2>&1; then
    dmesg 2>/dev/null | grep -i -E 'remoteproc|rpmsg|virtio|dr1m90' | tail -n 40 | sed 's/^/    /' \
        || echo "    (没有匹配行 —— 说明内核这边压根没走到)"
    echo
    echo "    找这两行的关键字："
    echo "      'KICK Firmware to start send messages'  ← 有=Linux 真的发过 SGI"
    echo "      'registered virtio0 device'             ← 有=vdev 链路建立成功"
else
    echo "    (没有 dmesg 命令)"
fi

sec "7. 中断统计（SGI 相关）"
echo "    (ARM GIC 的 SGI 在 /proc/interrupts 里通常不单独列，仅供参考)"
grep -i -E 'sgi|rpmsg|virtio|mailbox' /proc/interrupts 2>/dev/null | sed 's/^/    /' || echo "    (无匹配)"

sec "8. 一致性提醒"
cat <<'EOF'
    判断方向用这张表（对着 APU1 串口一起看）：

    APU1 串口显示                          | Linux 侧对应的现象              | 结论
    ---------------------------------------|--------------------------------|------------------
    [APU1][poll] ... (SGI rx=0) 一直不变   | 第 3 节没有 /dev/rpmsg0        | Linux 没建出节点
    [APU1][poll] ... (SGI rx=0) 一直不变   | 有 /dev/rpmsg0 但没人写过它     | 心跳守护没起来
    [APU1] SGI 14 received from APU0       | 第 6 节有 'KICK Firmware...'    | ★ 通路 OK
    ← PONG 1                               | 心跳守护日志有 '← PONG 1'       | ★★ 双向 OK
    [APU1][app] 启动后未收到主机心跳        | 心跳守护没跑 / 起太晚            | 心跳没送达
EOF

hr
echo " 体检结束。若第 3、5、6 节有 ✗，把本输出整段发给队友。"
hr
