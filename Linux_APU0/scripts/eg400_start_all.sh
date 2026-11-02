set -e
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FW=apu1_proto_v1_liveclock.elf
RPROC=/sys/class/remoteproc/remoteproc0
if [ "$(id -u)" != "0" ]; then echo "✗ 用 sudo 跑"; exit 1; fi
echo "==> [1/3] 起固件 ($FW)"
echo "$FW" > "$RPROC/firmware"
[ "$(cat "$RPROC/state" 2>/dev/null)" = "running" ] && { echo stop > "$RPROC/state"; sleep 0.5; }
echo start > "$RPROC/state"
i=0; while [ "$i" -lt 50 ]; do [ "$(cat "$RPROC/state" 2>/dev/null)" = "running" ] && break; i=$((i+1)); sleep 0.1; done
echo "    固件状态: $(cat "$RPROC/state" 2>/dev/null)"
echo "==> [2/3] 敲门建端点"
python3 "$SCRIPT_DIR/apu1_heartbeat_daemon.py" --once -v || true
echo "==> [3/3] 起 bridge + web"
sh "$SCRIPT_DIR/run_web_rpmsg_real.sh"
echo "完成。浏览器: http://192.168.137.100:5000/"
