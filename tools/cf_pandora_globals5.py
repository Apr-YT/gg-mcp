# -*- coding: utf-8 -*-
"""
Pandora _G 定位 v5: 字符串反推法
1. 本地算 "print"/"GamePlay" 等的 Pandora 哈希 (魔改 luaS_hash: h=(h*33+b)^h, 尾部步进, 初值=len)
2. 运行时索引 strt.hash 找 TString 对象
3. 搜 heap 指向 TString 的 qword 引用 -> 这些是把它当 key 的表 node
4. 回溯表头 -> 验证含全局特征 -> _G
"""
import json, socket, struct, time

HOST, PORT = "127.0.0.1", 34127
MAGIC, VER = 0x47474D43, 1
GGWF_REQ, GGWF_ERR = 0x0001, 0x0004
CMD = dict(SEARCH_RANGE=11, GET_RESULTS=13, READ=16)

class Conn:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=60)
    def call(self, cmd, params=None):
        payload = json.dumps({"p": params or {}}).encode()
        hdr = struct.pack("<IHHIII", MAGIC, VER, CMD[cmd], 1, GGWF_REQ, len(payload))
        self.s.sendall(hdr + payload)
        h = b""
        while len(h) < 20:
            h += self.s.recv(20 - len(h))
        _, _, _, _, flags, plen = struct.unpack("<IHHIII", h)
        body = b""
        while len(body) < plen:
            body += self.s.recv(plen - len(body))
        if flags & GGWF_ENC:  # 0x0010 响应 XOR 混淆
            body = bytes(b ^ ((0xA5 + i) & 0xFF) for i, b in enumerate(body))
        j = json.loads(body)
        if flags & GGWF_ERR:
            return {"err": j.get("e")}
        return j.get("r", j)
    def bulk(self, addr, ln):
        out = b""
        pos = 0
        while pos < ln:
            chunk = min(ln - pos, 0x8000)
            r = self.call("READ", {"addr": hex(addr + pos), "len": chunk})
            hx = r.get("hex", "")
            b = bytes.fromhex(hx) if hx else b""
            out += b
            if len(b) < chunk:
                break
            pos += len(b)
        return out
    def u64(self, addr):
        return struct.unpack_from("<Q", self.bulk(addr, 8), 0)[0]

def pandora_hash(s):
    """标准 Lua luaS_hash (seed=0): h ^= ((h<<5)+(h>>2)+byte); 尾部步进; 初值=len"""
    l = len(s)
    h = l & 0xFFFFFFFF
    step = (l >> 5) + 1
    i = l
    while i >= step:
        i -= step
        b = s[i]
        h = (h ^ ((h << 5) + (h >> 2) + b)) & 0xFFFFFFFF
    return h

def main():
    c = Conn()
    L = 0x7b02dedb50
    strt_hash = c.u64(L + 0xc0)
    strt_nuse = struct.unpack_from("<I", c.bulk(L + 0xc8, 4), 0)[0]
    strt_size = struct.unpack_from("<I", c.bulk(L + 0xcc, 4), 0)[0]
    print("[0] strt: hash=0x%x nuse=%d size=%d" % (strt_hash, strt_nuse, strt_size))

    for name in ("print", "require", "type", "GamePlay", "PlayerMgr"):
        h = pandora_hash(name.encode())
        slot = h & (strt_size - 1)
        ts = struct.unpack_from("<Q", c.bulk(strt_hash + slot * 8, 8), 0)[0]
        found = None
        depth = 0
        while ts and depth < 64:
            tb = c.bulk(ts, 0x20)
            ln = struct.unpack_from("<Q", tb, 0x10)[0]
            if ln == len(name) and tb[0x18:0x18 + ln] == name.encode():
                found = ts
                break
            ts = struct.unpack_from("<Q", tb, 0)[0]
            depth += 1
        if found:
            print("[1] '%s' hash=0x%x slot=%d -> TString=0x%x (chain depth=%d)"
                  % (name, h, slot, found, depth))
            r = c.call("SEARCH_RANGE", {"min": 0x7000000000, "max": 0x8000000000,
                                        "type": 4, "region": 3})
            # 上面是全量 qword 引用, 改用精确值搜索:
            r2 = c.call("SEARCH_RANGE", {"min": found, "max": found,
                                         "type": 4, "region": 3})
            cnt = r2.get("count", 0)
            print("    精确值匹配(0x%x): %d" % (found, cnt))
            refs = []
            for off in range(0, min(cnt, 20000), 10000):
                rr = c.call("GET_RESULTS", {"offset": off, "limit": 10000})
                rs = rr.get("results", [])
                if not rs:
                    break
                refs += [int(x["addr"], 16) for x in rs]
                if len(rs) < 10000:
                    break
            refs = list(set(refs))
            print("    引用位置 (过滤 node key 槽 off%0x28==0x10):")
            for rf in refs[:60]:
                mark = " <<< node-key槽" if (rf % 0x28) == 0x10 else ""
                print("      0x%x (off=0x%x)%s" % (rf, rf % 0x28, mark))
        else:
            print("[1] '%s' 未在 strt 找到 (h=0x%x slot=%d)" % (name, h, slot))
    c.s.close()

if __name__ == "__main__":
    main()
