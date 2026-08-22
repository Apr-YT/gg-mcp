#!/usr/bin/env python3
"""gg_direct.py — 直连 agentd 原始 wire 协议（不依赖 MCP）

用途：MCP 断连/不想起 server 时，直接对设备 agentd 做只读调用。
协议见 shared/wire_protocol.h：20 字节头 + JSON payload。
只实现只读命令（PING/SELF_TEST/KERNEL_STATUS/ATTACH/MODULE_BASE/READ_BYTES/MODULE_LIST），
绝无写操作——与 CF 铁律（只读直读）一致。

用法:
  python gg_direct.py ping
  python gg_direct.py status
  python gg_direct.py attach <pid|name>
  python gg_direct.py modbase <name>
  python gg_direct.py read <addr> <len> [--out file.bin]
  python gg_direct.py list
"""
import json, socket, struct, sys

HOST, PORT = "127.0.0.1", 34127   # adb forward 34127 -> 19100
MAGIC = 0x47474D43
VER = 1
GGWF_REQ = 0x0001
GGWF_ERR = 0x0004

# 命令码（shared/wire_protocol.h）
CMD = dict(PING=1, SELF_TEST=2, LIST_PROCESSES=3, ATTACH=4, DETACH=5,
           TARGET_INFO=6, MODULE_LIST=7, SEARCH=8, SEARCH_UNKNOWN_INIT=9,
           SEARCH_REFINE=10, SEARCH_RANGE=11, RESULT_COUNT=12, GET_RESULTS=13,
           CLEAR_RESULTS=14, READ=15, READ_BYTES=16, HEX_DUMP=17, WRITE=18,
           SET_RESULTS=19, FREEZE=20, UNFREEZE=21, MODULE_BASE=22, CALC_OFFSET=23,
           POINTER_SCAN=24, PAUSE=25, RESUME=26, KERNEL_STATUS=27, LOAD_GG=28,
           CORE_VERSIONS=29, SHELL=30, DEVICE_INFO=31, WATCH=32, DISASSEMBLE=33,
           VIEW_MEMORY=34, RESOLVE_SYMBOL=35, POINTER_CHAIN=36, SUBSCRIBE=37,
           UNSUBSCRIBE=38, RELAY=39, GHOST_TAP=40, GHOST_SWIPE=41)

def call(cmd_name, params=None):
    if cmd_name not in CMD:
        sys.exit(f"unknown cmd {cmd_name}; 可用: {list(CMD)}")
    payload = json.dumps({"p": params or {}}).encode()
    hdr = struct.pack("<IHHIII", MAGIC, VER, CMD[cmd_name], 1, GGWF_REQ, len(payload))
    with socket.create_connection((HOST, PORT), timeout=10) as s:
        s.sendall(hdr + payload)
        h = s.recv(20)
        while len(h) < 20:
            h += s.recv(20 - len(h))
        magic, ver, cmd, seq, flags, plen = struct.unpack("<IHHIII", h)
        if magic != MAGIC:
            sys.exit(f"bad magic 0x{magic:x}")
        body = b""
        while len(body) < plen:
            body += s.recv(plen - len(body))
        if flags & GGWF_ENC:  # 0x0010 响应 XOR 混淆
            body = bytes(b ^ ((0xA5 + i) & 0xFF) for i, b in enumerate(body))
        j = json.loads(body)
        if flags & GGWF_ERR:
            e = j.get("e", {})
            sys.exit(f"ERR cmd={cmd} code={e.get('code')} msg={e.get('message')}")
        return j.get("r", j)

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    op = sys.argv[1]
    if op == "ping":
        print(call("PING"))
    elif op == "status":
        r = call("KERNEL_STATUS")
        for k in ("state", "state_str", "ko_type", "ko_loaded", "ko_module",
                  "ioctl_read", "ioctl_write", "error"):
            print(f"  {k}: {r.get(k)}")
    elif op == "list":
        r = call("LIST_PROCESSES")
        ps = r.get("processes") or r.get("list") or []
        print(f"{len(ps)} 进程")
        for p in ps[:40]:
            print(f"  {p}")
    elif op == "attach":
        arg = sys.argv[2] if len(sys.argv) > 2 else ""
        params = {"pid": int(arg)} if arg.isdigit() else {"name": arg}
        r = call("ATTACH", params)
        print(r)
    elif op == "modbase":
        r = call("MODULE_BASE", {"name": sys.argv[2]})
        print(r)
    elif op == "read":
        addr = sys.argv[2]; length = int(sys.argv[3])
        out = None
        if "--out" in sys.argv:
            out = sys.argv[sys.argv.index("--out") + 1]
        r = call("READ_BYTES", {"addr": addr, "len": length})
        hx = r.get("hex", "")
        raw = bytes.fromhex(hx)
        if out:
            open(out, "wb").write(raw)
            print(f"saved {len(raw)}B -> {out}")
        else:
            print(f"bytes={len(raw)} hex={hx[:64]}{'...' if len(hx) > 64 else ''}")
    elif op == "mods":
        r = call("MODULE_LIST")
        print(r)
    elif op == "search":
        # search <value> <type> [region]   value 支持 0x/decimal; type=bytes 时 hex
        val = sys.argv[2]
        t = {"byte": 1, "word": 2, "dword": 3, "qword": 4, "float": 5, "double": 6, "bytes": 7}.get(
            sys.argv[3] if len(sys.argv) > 3 else "dword", 3)
        region = {"anonymous": 1, "anon": 1, "writable": 2, "all": 3}.get(
            sys.argv[4] if len(sys.argv) > 4 else "all", 3)
        r = call("SEARCH", {"value": val, "type": t, "region": region})
        print(r)
    elif op == "results":
        off = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        lim = int(sys.argv[3]) if len(sys.argv) > 3 else 50
        r = call("GET_RESULTS", {"offset": off, "limit": lim})
        print(r)
    else:
        sys.exit(__doc__)

if __name__ == "__main__":
    main()
