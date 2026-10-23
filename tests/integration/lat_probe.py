"""lat_probe.py — 直连 agentd 量单次 wire 请求的往返延迟。

为什么需要它：MCP 路径（HTTP + JSON-RPC + PowerShell）会把 ms 级差异埋掉，
而 wire 层的小帧延迟直接决定"AI 连续读点"的手感。2026-10-23 靶场就是靠它
抓到 40ms/请求——TCP Nagle + delayed ACK，合并 header/payload 并关 NODELAY
后降到 0.7ms（57x）。

覆盖：
  READ（单点读）/ WRITE（回写）/ READ_BYTES（64KB、256KB）
  以及三个已删除的事件命令（期望 command removed）

用法：
  python tests/integration/lat_probe.py            # 用当前 attach 的会话
  python tests/integration/lat_probe.py 13287      # 指定 pid（会重新 attach）

前提：adb forward tcp:34127 在位，configs/gg-ghost.json 里有 wire_key。
退出码：任一请求报错为 1，便于脚本串联。
"""
import json
import os
import socket
import struct
import sys
import time

HOST, PORT = "127.0.0.1", 34127
MAGIC, VER, SEED = 0x47474D43, 1, 0xA5
F_REQ, F_RESP, F_ERR, F_EVENT, F_ENC = 1, 2, 4, 8, 0x10

# cmd 码取自 shared/wire_protocol.h 的枚举顺序，改协议时必须同步。
CMD = {
    "ping": 1, "self_test": 2, "list": 3, "attach": 4, "detach": 5,
    "search": 8, "result_count": 12, "get_results": 13,
    "read": 15, "read_bytes": 16, "write": 18,
    "kernel_status": 27, "auth": 42,
    "subscribe": 37, "unsubscribe": 38, "relay": 39,
}

HDR = struct.Struct("<IHHIII")


def xor_inplace(b: bytearray) -> None:
    """负载混淆：与设备端 gg_xor_payload 同一算法（按位置偏移 XOR）。"""
    for i in range(len(b)):
        b[i] ^= (SEED + i) & 0xFF


class Client:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=600)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.seq = 0

    def call(self, cmd, payload):
        self.seq += 1
        body = bytearray(json.dumps(payload, ensure_ascii=False).encode())
        xor_inplace(body)
        t0 = time.perf_counter()
        self.sock.sendall(HDR.pack(MAGIC, VER, cmd, self.seq, F_REQ | F_ENC, len(body)) + body)
        while True:
            head = self._recv_exact(HDR.size)
            _, _, _, _, flags, plen = HDR.unpack(head)
            raw = bytearray(self._recv_exact(plen)) if plen else bytearray()
            if flags & F_ENC:
                xor_inplace(raw)
            if flags & F_EVENT:      # 事件帧（已废弃，理论上不会再出现）
                continue
            dt = (time.perf_counter() - t0) * 1000.0
            return bool(flags & F_ERR), json.loads(raw.decode() or "{}"), dt

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise RuntimeError("connection closed")
            buf += chunk
        return buf


def load_key():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    cfg = os.path.join(root, "configs", "gg-ghost.json")
    with open(cfg, encoding="utf-8") as f:
        key = json.load(f).get("wire_key", "")
    if not key:
        raise SystemExit("configs/gg-ghost.json 里没有 wire_key（先跑 scripts/deploy.ps1）")
    return key


def bench(label, fn, n):
    ts = []
    for _ in range(n):
        ts.append(fn())
    ts_sorted = sorted(ts)
    print("  %-22s n=%-3d 平均 %6.2f ms  中位 %6.2f ms  最小 %6.2f ms"
          % (label, n, sum(ts) / len(ts), ts_sorted[len(ts) // 2], ts_sorted[0]))
    return sum(ts) / len(ts)


def main():
    c = Client()
    err, res, _ = c.call(CMD["auth"], {"p": {"key": load_key(), "frag": 1}})
    if err:
        print("auth failed:", res)
        return 1
    print("auth: ok")

    if len(sys.argv) > 1:
        pid = int(sys.argv[1])
        err, res, dt = c.call(CMD["attach"], {"p": {"pid": pid}})
        if err:
            print("attach failed:", res)
            return 1
        print("attach: ok pid=%d (%.1f ms)" % (pid, dt))

    # 找一条自己的命中当读/写靶子，比硬编码地址稳（进程重启后地址会变）
    err, res, _ = c.call(CMD["search"], {"p": {"type": "dword", "value": 1000, "region": "writable"}})
    if err or not res.get("count"):
        print("search 没命中，换个值再跑；原始返回:", res)
        return 1
    err, res, _ = c.call(CMD["get_results"], {"p": {"offset": 0, "limit": 1}})
    hits = res.get("results") or []
    if not hits:
        print("get_results 为空")
        return 1
    addr = hits[0]["addr"]
    print("靶地址: %s (值 %s)\n" % (addr, hits[0].get("value")))

    print("单次请求延迟：")
    bench("READ dword", lambda: c.call(CMD["read"], {"p": {"addr": addr, "type": "dword"}})[2], 20)
    bench("WRITE 回写同值", lambda: c.call(CMD["write"], {"p": {"addr": addr, "type": "dword", "value": "1000"}})[2], 10)
    bench("READ_BYTES 64KB", lambda: c.call(CMD["read_bytes"], {"p": {"addr": addr, "len": 65536}})[2], 10)
    bench("READ_BYTES 256KB", lambda: c.call(CMD["read_bytes"], {"p": {"addr": addr, "len": 262144}})[2], 10)

    print("\n已删除的事件命令（应全部 command removed）：")
    bad = 0
    for name in ("subscribe", "unsubscribe", "relay"):
        err, res, _ = c.call(CMD[name], {"p": {}})
        msg = (res.get("e") or {}).get("message", "")
        ok = err and "command removed" in msg
        print("  %-12s %s %s" % (name, "OK " if ok else "FAIL", msg[:70]))
        if not ok:
            bad += 1
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
