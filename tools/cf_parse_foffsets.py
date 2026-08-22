#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 解析 gg_read_bytes 结果文件（64KB hex）并追加到聚合文件
# 用法: cf_parse_foffsets.py <结果文件路径> <聚合文件> <基址> <起始索引>
import re, sys, json, os

resfile = sys.argv[1]
aggfile = sys.argv[2]
base = int(sys.argv[3], 16)
start_idx = int(sys.argv[4])

txt = open(resfile, encoding='utf-8', errors='replace').read()
m = re.search(r'"hex"\s*:\s*"([0-9a-f]+)"', txt)
if not m:
    print("未找到 hex"); sys.exit(1)
raw = bytes.fromhex(m.group(1))
n = len(raw)//8
out = {}
for i in range(n):
    v = int.from_bytes(raw[i*8:i*8+8], 'little')
    if v:
        out[start_idx + i] = f"0x{v:x}"
data = {}
if os.path.exists(aggfile):
    data = json.load(open(aggfile, encoding='utf-8'))
data.update(out)
json.dump(data, open(aggfile, 'w', encoding='utf-8'), indent=0)
print(f"段[{start_idx}..{start_idx+n-1}] 非0 {len(out)} 个, 聚合共 {len(data)} 个")
