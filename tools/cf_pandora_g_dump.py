# -*- coding: utf-8 -*-
"""
CF Pandora Lua _G 全链一键 dump (2026-08-20 最终版)
全链: pua_State(特征扫描) -> L+0x160 registry -> 相邻 _G -> 全键 dump
关键地址(本轮已固化): pua_State=0x7b02dedb50, registry=0x7ab27ed660, _G=0x7ab27eded0
用法: python cf_pandora_g_dump.py [--g] [--mods APICtrl,GameBook]
"""
import json, socket, struct, sys

HOST, PORT = "127.0.0.1", 34127
MAGIC, VER = 0x47474D43, 1
GGWF_REQ, GGWF_ERR = 0x0001, 0x0004
CMD = dict(SEARCH_RANGE=11, GET_RESULTS=13, READ=16, CLEAR_RESULTS=14, MODULE_LIST=7)

TT = {0: "nil", 3: "num", 4: "str", 5: "tbl", 6: "fn", 7: "ud", 8: "thr"}
PAN_BASE = 0x7919e01000

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

def u64at(buf, off):
    return struct.unpack_from("<Q", buf, off)[0] if off + 8 <= len(buf) else 0
def u32at(buf, off):
    return struct.unpack_from("<I", buf, off)[0] if off + 4 <= len(buf) else 0

def find_puastate(c):
    """特征扫描 pua_State: alloc@0xd0∈libpandora + 自指@0x170 + registry@0x160 tt==5"""
    ml = c.call("MODULE_LIST")
    pan = None
    for m in ml.get("modules", []):
        if "pandora" in (m.get("path", "") or ""):
            pan = m
            break
    if not pan:
        return None, None
    base = int(pan["base"], 16)
    end = base + int(pan.get("size", 0x75000))
    r = c.call("SEARCH_RANGE", {"min": base, "max": end, "type": 4, "region": 3})
    total = r.get("count", 0)
    refs = []
    for off in range(0, min(total, 60000), 10000):
        rr = c.call("GET_RESULTS", {"offset": off, "limit": 10000})
        rs = rr.get("results", [])
        if not rs:
            break
        refs += [int(x["addr"], 16) for x in rs]
        if len(rs) < 10000:
            break
    c.call("CLEAR_RESULTS", {})
    for ref in set(refs):
        pua = ref - 0xd0
        if not (0x7000000000 <= pua <= 0x7FFFFFFFFF):
            continue
        raw = c.bulk(pua, 0x178)
        if len(raw) < 0x178:
            continue
        if u64at(raw, 0x170) == pua and base + 0x1000 <= u64at(raw, 0xd0) < end:
            if u32at(raw, 0x168) == 5:  # registry tt==5
                return pua, u64at(raw, 0x160)
    return None, None

def table_keys(c, T, want=(), max_show=0):
    hd = c.bulk(T, 0x40)
    if len(hd) < 0x40:
        return [], {}
    ls = hd[0xb]
    node = u64at(hd, 0x20)
    array = u64at(hd, 0x18)
    alimit = u32at(hd, 0x38)
    n = 1 << ls
    keys = []
    dic = {}
    if node and n:
        nbuf = c.bulk(node, n * 0x28)
        for i in range(n):
            base = i * 0x28
            kv = u64at(nbuf, base + 0x10)
            kt = u32at(nbuf, base + 0x18)
            vv = u64at(nbuf, base + 0x00)
            vt = u32at(nbuf, base + 0x08)
            if kt == 4 and kv:
                sbuf = c.bulk(kv, 0x20)
                ln = u64at(sbuf, 0x10)
                ss = sbuf[0x18:0x18 + min(ln, 60)].decode(errors="replace") if ln else "?"
                keys.append((ss, vt, vv))
                dic[ss] = (vt, vv)
            elif kv:
                keys.append(("#%d" % kt, vt, vv))
                dic["#%d" % kt] = (vt, vv)
    return keys, dic

def fmt_val(c, vt, vv):
    if vt == 4:
        sbuf = c.bulk(vv, 0x20)
        ln = u64at(sbuf, 0x10)
        return repr(sbuf[0x18:0x18 + min(ln, 80)].decode(errors="replace")) if ln else "''"
    if vt == 3:
        return repr(struct.unpack("<d", struct.pack("<Q", vv))[0])
    if vt == 1:
        return str(vv != 0)
    return "0x%x" % vv

def main():
    args = sys.argv[1:]
    c = Conn()
    L, reg = find_puastate(c)
    if not L:
        print("pua_State 未找到"); return
    print("[0] pua_State=0x%x registry=0x%x" % (L, reg))
    # _G = registry + 0x870? 不对, 直接特征: _G 表含 _G 自引用键
    # 已知 _G=0x7ab27eded0 (相邻), 但为通用性: 用 L+0x78 槽 (mainthread=_G 缓存)
    lbuf = c.bulk(L, 0x100)
    g = u64at(lbuf, 0x78)
    kk, dd = table_keys(c, g)
    print("[1] _G 候选 0x%x 键数=%d" % (g, len(kk)))
    if "_G" in dd and dd["_G"][1] == g:
        print("    ✔ 自引用验证通过 (_G._G == 0x%x)" % g)
    out = []
    for k, vt, vv in kk:
        out.append("%s\t%s\t%s" % (k, TT.get(vt, "?"), fmt_val(c, vt, vv)))
    open("cf_g_dump.tsv", "w", encoding="utf-8").write("\n".join(out))
    print("[2] 已存 cf_g_dump.tsv (%d 键)" % len(kk))
    # gb_* 详情
    print("\n[3] gb_* 游戏全局变量:")
    for k, vt, vv in kk:
        if k.startswith("gb_") or k in ("_VERSION",):
            print("    %s = %s" % (k, fmt_val(c, vt, vv)))
    # 指定模块
    mods = []
    for a in args:
        if a.startswith("--mods="):
            mods = a[7:].split(",")
    if "--allmods" in args:
        mods = [k for k, (vt, vv) in dd.items() if vt == 5]
    for m in mods:
        if m in dd and dd[m][0] == 5:
            T = dd[m][1]
            k2, d2 = table_keys(c, T)
            print("\n[4] 模块表 %s (0x%x) %d 键:" % (m, T, len(k2)))
            for k, vt, vv in k2[:50]:
                print("    %s = %s" % (k, fmt_val(c, vt, vv)))
    c.s.close()

if __name__ == "__main__":
    main()
