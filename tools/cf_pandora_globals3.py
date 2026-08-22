# -*- coding: utf-8 -*-
"""
Pandora _G 定位 v3 (批量读模式, 2026-08-20)
v2 教训: 逐字段/逐候选小读 -> 上万次 wire 往返 -> 9 分钟。
v3: 一切按"大块批量读" -> 全程 < 10 次 READ。
已知(上一轮已验证): pua_State=0x7b02dedb50 registry=0x7ab27ed660
  Table: lsizenode@0xb metatable@0x10 array@0x18 node@0x20 lastfree@0x28 alimit@0x38
  Node : 0x28B {value@0, tt@8, key.value@0x10, key.tt@0x18, next@0x20}
  TString: {next@0, tt@8, marked@9, hash@0xc(u32), len@0x10, data@0x18}
"""
import json, socket, struct, sys, time

HOST, PORT = "127.0.0.1", 34127
MAGIC, VER = 0x47474D43, 1
GGWF_REQ, GGWF_ERR = 0x0001, 0x0004
CMD = dict(MODULE_LIST=7, READ=16)

TT = {0: "nil", 3: "number", 4: "string", 5: "table", 6: "function",
      7: "userdata", 8: "thread", 1: "bool", 2: "?"}

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
            body = bytes(b ^ (0xA5 + i) for i, b in enumerate(body))
        j = json.loads(body)
        if flags & GGWF_ERR:
            return {"err": j.get("e")}
        return j.get("r", j)
    def bulk(self, addr, ln):
        """大块读，超长自动分段"""
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
    def u64at(self, buf, off):
        return struct.unpack_from("<Q", buf, off)[0] if off + 8 <= len(buf) else 0
    def u32at(self, buf, off):
        return struct.unpack_from("<I", buf, off)[0] if off + 4 <= len(buf) else 0

def parse_table(c, T, name, want=None):
    """批量解析 Pandora Table: 一次读 0x40 头 + 整个 node 区 + array 区"""
    head = c.bulk(T, 0x40)
    if len(head) < 0x40:
        print("  [x] %s table 0x%x head read fail" % (name, T))
        return None
    lsizenode = head[0xb]
    metatable = c.u64at(head, 0x10)
    array     = c.u64at(head, 0x18)
    node      = c.u64at(head, 0x20)
    lastfree  = c.u64at(head, 0x28)
    alimit    = c.u32at(head, 0x38)
    n = 1 << lsizenode
    print("  == %s 0x%x: lsizenode=%d(n=%d) array=0x%x alimit=%d node=0x%x lastfree=0x%x meta=0x%x"
          % (name, T, lsizenode, n, array, alimit, node, lastfree, metatable))
    res = {"keys": [], "tables": {}}
    # array 区一次读
    if array and alimit:
        abuf = c.bulk(array, min(alimit, 128) * 16)
        for i in range(min(alimit, 128)):
            v = c.u64at(abuf, i * 16)
            t = c.u32at(abuf, i * 16 + 8)
            if v:
                res["keys"].append(("[%d]" % (i + 1), t, v))
    # node 区一次读全
    if node and n:
        nbuf = c.bulk(node, n * 0x28)
        for i in range(n):
            base = i * 0x28
            kv = c.u64at(nbuf, base + 0x10)
            kt = c.u32at(nbuf, base + 0x18)
            vv = c.u64at(nbuf, base + 0x00)
            vt = c.u32at(nbuf, base + 0x08)
            if not kv:
                continue
            if kt == 4:
                sbuf = c.bulk(kv, 0x20)
                ln_ = c.u64at(sbuf, 0x10)
                ss = sbuf[0x18:0x18 + min(ln_, 64)].decode(errors="replace") if len(sbuf) >= 0x18 else "?"
                res["keys"].append((ss, vt, vv))
                res["tables"][ss] = (vt, vv)
            else:
                res["keys"].append(("#%d:%x" % (kt, kv), vt, vv))
    return res

def main():
    c = Conn()
    # 模块确认
    ml = c.call("MODULE_LIST")
    pan = None
    for m in ml.get("modules", []):
        if "pandora" in (m.get("path", "") or ""):
            pan = m
            break
    print("[0] libpandora base=0x%x" % (int(pan["base"], 16) if pan else 0))

    L = 0x7b02dedb50
    lbuf = c.bulk(L, 0x200)
    print("[1] pua_State=0x%x (0x200B 一次读)" % L)
    print("    selfref@0x170=0x%x alloc@0xd0=0x%x registry@0x160={0x%x,tt=%d}"
          % (c.u64at(lbuf, 0x170), c.u64at(lbuf, 0xd0), c.u64at(lbuf, 0x160), c.u32at(lbuf, 0x168)))
    print("    L+0x28=0x%x  L+0x88={0x%x,tt=%d}  L+0x78(mainthread)=0x%x"
          % (c.u64at(lbuf, 0x28), c.u64at(lbuf, 0x88), c.u32at(lbuf, 0x90), c.u64at(lbuf, 0x78)))

    reg = c.u64at(lbuf, 0x160)
    r = parse_table(c, reg, "REGISTRY")
    # 找 _G 特征: 含 print/require/type 键的表
    print("\n[2] 遍历 registry 内全部表, 找含 print/require 的全局表...")
    cands = []
    for k, vt, vv in (r["keys"] if r else []):
        if vt == 5 and vv:
            rr = parse_table(c, vv, "T[%s]" % k[:20])
            ks = [x[0] for x in (rr["keys"] if rr else [])]
            hit = [x for x in ("print", "require", "pairs", "ipairs", "type", "string", "table", "math") if x in ks]
            if hit:
                cands.append((vv, k, hit))
                print("    !! %s 含全局特征 %s" % (k[:30], hit))
    if not cands:
        print("    registry 内无 _G 特征表")

    # _LOADED 模块表
    if r and "_LOADED" in r["tables"]:
        vt, vv = r["tables"]["_LOADED"]
        print("\n[3] _LOADED 表 0x%x:" % vv)
        parse_table(c, vv, "_LOADED", want=["GamePlay", "PlayerMgr"])
    # __luabase
    if r and "__luabase" in r["tables"]:
        vt, vv = r["tables"]["__luabase"]
        print("\n[4] __luabase 表 0x%x:" % vv)
        parse_table(c, vv, "__luabase")

    # 候选B: [L+0x28] 链 (ci 链)
    a = c.u64at(lbuf, 0x28)
    print("\n[5] L+0x28=0x%x -> +8=0x%x -> [0]=0x%x -> +0x18=0x%x"
          % (a, c.u64at(c.bulk(a + 8, 8), 0) if a else 0, 0, 0))
    c.s.close()

if __name__ == "__main__":
    main()
