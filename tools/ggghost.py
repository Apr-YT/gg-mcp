#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ggghost.py — GG-Ghost MCP 客户端（GG 修改器 Lua 脚本的等价物）

等价关系: GG 的 .lua 脚本 = 本文件 + 任务脚本(如 hillclimb_coin.py)
用法:
    from ggghost import gg
    gg.search("925059", "dword", "all")
    gg.freeze("0x707c510ed4", "999999", "dword")

命令行模式:
    python ggghost.py search <value> [type] [region]
    python ggghost.py read <addr> [type]
    python ggghost.py write <addr> <value> [type]
    python ggghost.py freeze <addr> <value> [type]
    python ggghost.py unfreeze <addr>
    python ggghost.py watch <addr> [type] [len]     # type: 1=read 2=write 3=rw
    python ggghost.py poll
    python ggghost.py ghost-tap <x> <y> [hold_ms]
    python ggghost.py attach <pid>
    python ggghost.py modules
    python ggghost.py disasm <addr> [count]

地址: 支持 "libgame+0x702ED4" 形式 (自动解析模块基址+偏移)
"""
import json
import sys
import urllib.request

MCP_URL = "http://127.0.0.1:8788/mcp"


class GGHostError(Exception):
    pass


class GG:
    def __init__(self, url=MCP_URL):
        self.url = url
        self._seq = 0

    def _call(self, tool, args=None):
        self._seq += 1
        req = {
            "jsonrpc": "2.0",
            "id": self._seq,
            "method": "tools/call",
            "params": {"name": tool, "arguments": args or {}},
        }
        data = json.dumps(req).encode()
        r = urllib.request.urlopen(self.url, data, timeout=120)
        resp = json.loads(r.read().decode())
        if "error" in resp:
            raise GGHostError(f"{tool}: {resp['error']}")
        try:
            text = resp["result"]["content"][0]["text"]
            return json.loads(text)
        except Exception:
            return resp.get("result")

    # ---- 进程 ----
    def attach(self, pid):
        return self._call("gg_attach", {"pid": pid})

    def detach(self):
        return self._call("gg_detach", {})

    def modules(self):
        return self._call("gg_module_list", {})

    def module_base(self, name_substr):
        """按名字子串找模块基址(字符串)"""
        for m in self.modules().get("modules", []):
            if name_substr in m.get("path", ""):
                return m["base"]
        return None

    # ---- 搜索 ----
    def search(self, value, typ="dword", region="all"):
        return self._call("gg_search", {"value": str(value), "type": typ, "region": region})

    def refine(self, op):
        return self._call("gg_search_refine", {"op": op})

    def results(self, limit=100, typ="dword"):
        return self._call("gg_get_results", {"limit": limit, "type": typ})

    # ---- 内存 ----
    def read(self, addr, typ="dword"):
        return self._call("gg_read", {"addr": addr, "type": typ})

    def write(self, addr, value, typ="dword"):
        return self._call("gg_write", {"addr": addr, "value": str(value), "type": typ})

    def view(self, addr, elem="int32", count=16):
        return self._call("gg_view_memory", {"addr": addr, "element_type": elem, "range": count})

    def freeze(self, addr, value, typ="dword"):
        return self._call("gg_freeze", {"addr": addr, "value": str(value), "type": typ})

    def unfreeze(self, addr):
        return self._call("gg_unfreeze", {"addr": addr})

    # ---- 指针 ----
    def pointer_scan(self, typ="qword", cap=2000):
        """可写区找指向当前搜索结果地址的指针"""
        return self._call("gg_pointer_scan", {"type": typ, "cap": cap})

    def pointer_chain(self, addr, max_depth=8):
        """从存有指针的容器地址逐级解引用到基址, 还原 base+offset"""
        return self._call("gg_pointer_chain", {"addr": addr, "max_depth": max_depth})

    # ---- 断点 ----
    def watch_set(self, addr, typ=2, length=4):
        return self._call("gg_watch_set", {"addr": addr, "len": length, "type": typ})

    def watch_poll(self):
        return self._call("gg_watch_poll", {})

    def watch_clear(self):
        return self._call("gg_watch_clear", {})

    # ---- 反汇编 ----
    def disasm(self, addr, count=16):
        return self._call("gg_disassemble", {"addr": addr, "count": count})

    # ---- 触摸 ----
    def tap(self, x, y, hold=80):
        return self._call("gg_ghost_tap", {"x": x, "y": y, "hold_ms": hold})

    def swipe(self, x1, y1, x2, y2, ms=300):
        return self._call("gg_ghost_swipe",
                          {"x1": x1, "y1": y1, "x2": x2, "y2": y2, "ms": ms})


def _resolve(gg, addr):
    """libgame+0x702ED4 → 绝对地址"""
    if "+0x" in addr:
        name, off = addr.split("+0x")
        base = gg.module_base(name)
        if not base:
            raise GGHostError(f"module not found: {name}")
        return hex(int(base, 16) + int(off, 16))
    return addr


def main():
    gg = GG()
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]

    if cmd == "search":
        args = {"value": sys.argv[2]}
        if len(sys.argv) > 3: args["type"] = sys.argv[3]
        if len(sys.argv) > 4: args["region"] = sys.argv[4]
        print(gg.search(**args))
    elif cmd == "read":
        a = _resolve(gg, sys.argv[2])
        typ = sys.argv[3] if len(sys.argv) > 3 else "dword"
        print(gg.read(a, typ))
    elif cmd == "write":
        a = _resolve(gg, sys.argv[2])
        typ = sys.argv[4] if len(sys.argv) > 4 else "dword"
        print(gg.write(a, sys.argv[3], typ))
    elif cmd == "freeze":
        a = _resolve(gg, sys.argv[2])
        typ = sys.argv[4] if len(sys.argv) > 4 else "dword"
        print(gg.freeze(a, sys.argv[3], typ))
    elif cmd == "unfreeze":
        print(gg.unfreeze(_resolve(gg, sys.argv[2])))
    elif cmd == "watch":
        a = _resolve(gg, sys.argv[2])
        typ = int(sys.argv[3]) if len(sys.argv) > 3 else 2
        ln = int(sys.argv[4]) if len(sys.argv) > 4 else 4
        print(gg.watch_set(a, typ, ln))
    elif cmd == "poll":
        print(gg.watch_poll())
    elif cmd == "ptrs":
        """扫指向当前结果集的指针, 并对前 N 个追链"""
        cap = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
        r = gg.pointer_scan("qword", cap)
        print(json.dumps(r, ensure_ascii=False)[:600])
    elif cmd == "chain":
        a = _resolve(gg, sys.argv[2])
        md = int(sys.argv[3]) if len(sys.argv) > 3 else 8
        r = gg.pointer_chain(a, md)
        print(json.dumps(r, ensure_ascii=False, indent=1)[:1500])
    elif cmd == "ghost-tap":
        print(gg.tap(int(sys.argv[2]), int(sys.argv[3])))
    elif cmd == "attach":
        print(gg.attach(int(sys.argv[2])))
    elif cmd == "modules":
        for m in gg.modules().get("modules", []):
            print(f"{m['base']}  {m['size']:>10}  {m['path']}")
    elif cmd == "disasm":
        a = _resolve(gg, sys.argv[2])
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
        for i in gg.disasm(a, n).get("instructions", []):
            print(f"  {i.get('addr')}: {i.get('mnemonic','')} {i.get('op_str','')}")
    else:
        print(f"unknown cmd: {cmd}")
        print(__doc__)


if __name__ == "__main__":
    main()
