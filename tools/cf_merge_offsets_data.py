#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 拼接 gg_read_bytes 结果文件 → 完整偏移数组数据镜像
# 段清单: 地址 → 结果文件后缀（在 tool-results 目录）
import re, os, glob

RES_DIR = r"C:\Users\AprYT\.workbuddy\projects\d-project-workbuddy-工具\60d0f164-81a8-4890-873f-f303c4388f47\tool-results"
OUT = r"D:/project/ai_workspace/temp/cf_offsets_full.bin"
IMG_BASE = 0x7141978000

# 地址 → 文件后缀
segments = {
    0x7141978000: "c1648f", 0x7141980000: "991c3e", 0x7141990000: "90b2a0",
    0x71419A0000: "7173fc", 0x71419B0000: "c61fcf",
    0x71419C0000: "02a6ba", 0x71419D0000: "fb3740",
    0x71419E0000: "3f0320", 0x71419F0000: "e33249",
    0x7141A00000: "8c8af2", 0x7141A10000: "23d23e",
    0x7141A20000: "d6c13d", 0x7141A30000: "93d199",
    0x7141A40000: "b3a808", 0x7141A50000: "516ec4",
    0x7141A60000: "9d1b1c", 0x7141A70000: "33311b",
    0x7141A80000: "58d96e", 0x7141A90000: "a54400",
    0x7141AA0000: "3933a8", 0x7141AB0000: "ebf72b",
    0x7141AC0000: "9c5eed", 0x7141AD0000: "24b8c2",
    0x7141AE0000: "640da3", 0x7141AF0000: "df01bb",
    0x7141B00000: "3d134a", 0x7141B04000: "7b0936",
    0x7141B10000: "e05bad", 0x7141B14000: "07fd43",
    0x7141B24000: "0b21e8", 0x7141B34000: "54fcf2",
}

def get_hex(path):
    txt = open(path, encoding='utf-8', errors='replace').read()
    m = re.search(r'"hex"\s*:\s*"([0-9a-f]+)"', txt)
    return m.group(1) if m else None

# 匹配结果文件（含后缀标识）
files = glob.glob(os.path.join(RES_DIR, "mcp-connector-proxy-gg-ghost_gg_read_bytes-*.txt"))
by_suffix = {}
for f in files:
    base = os.path.basename(f)
    m = re.search(r'-([0-9a-f]+)\.txt$', base)
    if m: by_suffix[m.group(1)] = f

missing = []
# 计算总范围
lo = min(segments); hi = max(segments) + 0x10000
data = bytearray(hi - IMG_BASE)
for addr in sorted(segments):
    suffix = segments[addr]
    if suffix not in by_suffix:
        missing.append((hex(addr), suffix)); continue
    hx = get_hex(by_suffix[suffix])
    if not hx:
        missing.append((hex(addr), suffix)); continue
    chunk = bytes.fromhex(hx)
    pos = addr - IMG_BASE
    data[pos:pos+len(chunk)] = chunk
    print(f"0x{addr:x} <- {suffix} ({len(chunk)} B) @img 0x{pos:x}")

open(OUT, 'wb').write(data)
print(f"\n拼接完成: {len(data)} 字节 -> {OUT}")
if missing:
    print(f"缺失: {missing}")
