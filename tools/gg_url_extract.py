#!/usr/bin/env python3
"""gg_url_extract.py — 用 gg 内核只读通道批量提取进程内存中的 URL/API 路径

流程: SEARCH 搜 "https://"(bytes) -> GET_RESULTS 拿地址 -> READ_BYTES 读 256B -> 正则提取 URL -> 去重排序
用法: python gg_url_extract.py [--limit N] [--out urls.txt]
"""
import json, socket, struct, sys, re, argparse

HOST, PORT = "127.0.0.1", 34127
MAGIC = 0x47474D43
GGWF_REQ = 0x0001
GGWF_ERR = 0x0004
CMD = dict(SEARCH=8, GET_RESULTS=13, READ_BYTES=16)

def call(cmd_name, params=None):
    payload = json.dumps({"p": params or {}}).encode()
    hdr = struct.pack("<IHHIII", MAGIC, 1, CMD[cmd_name], 1, GGWF_REQ, len(payload))
    with socket.create_connection((HOST, PORT), timeout=10) as s:
        s.sendall(hdr + payload)
        h = s.recv(20)
        while len(h) < 20:
            h += s.recv(20 - len(h))
        magic, ver, cmd, seq, flags, plen = struct.unpack("<IHHIII", h)
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
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=400)
    ap.add_argument("--out", default="urls.txt")
    ap.add_argument("--pat", default="https?://", help="URL 正则")
    args = ap.parse_args()

    # 1. 搜 https:// (bytes 类型=7)
    r = call("SEARCH", {"value": "68747470733a2f2f", "type": 7, "region": 3})
    count = r.get("count", 0)
    print(f"[*] 命中 {count} 处 https://")
    if count == 0:
        return

    # 2. 分批取结果
    addrs = []
    off = 0
    while len(addrs) < min(count, args.limit):
        r = call("GET_RESULTS", {"offset": off, "limit": 200})
        batch = r.get("results") or r.get("list") or []
        if not batch:
            break
        for it in batch:
            a = it.get("addr") if isinstance(it, dict) else it
            if a:
                addrs.append(int(a) if not isinstance(a, str) else int(a, 16))
        off += len(batch)
        if len(batch) < 200:
            break
    print(f"[*] 待读取地址 {len(addrs)} 个")

    # 3. 读内存提取 URL
    rx = re.compile(args.pat.encode() + rb'[^\x00-\x20"\'<>]{4,120}')
    urls = set()
    for i, a in enumerate(addrs):
        try:
            r = call("READ_BYTES", {"addr": hex(a), "len": 256})
            raw = bytes.fromhex(r.get("hex", ""))
        except Exception:
            continue
        for m in rx.finditer(raw):
            s = m.group().decode("utf-8", "replace")
            # 清理尾部截断的坏字符
            s = s.rstrip('.,;:)])"\'')
            urls.add(s)
        if (i + 1) % 100 == 0:
            print(f"[*] {i+1}/{len(addrs)}")
    urls = sorted(urls)
    print(f"[+] 提取 URL {len(urls)} 条")
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(urls))
    print(f"[+] 保存 -> {args.out}")
    for u in urls[:80]:
        print(f"  {u}")

if __name__ == "__main__":
    main()
