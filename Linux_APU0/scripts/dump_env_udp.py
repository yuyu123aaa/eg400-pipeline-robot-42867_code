# 临时抓帧工具：监听 UDP 9998（与 Web 并存，SO_REUSEADDR），
# 把守护进程解析后的 ENV JSON 打 20 秒，用于确认固件实际上报值。
import json
import socket
import time

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 9998))
print("listening 9998 for 20s ...", flush=True)
t0 = time.time()
n = 0
while time.time() - t0 < 20:
    try:
        d, _ = s.recvfrom(2048)
        j = json.loads(d.decode("utf-8", "ignore"))
        if "turbidity" in j or "gas_o2" in j:
            print(j, flush=True)
            n += 1
    except Exception:
        pass
print("done, env frames =", n, flush=True)
