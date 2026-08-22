#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# CF 定制 dump：metadata 字段名 + 运行时 fieldOffsets 合并 → 完整偏移表
# 用法: cf_build_runtime_offsets.py [输出tsv]
import struct, json, os, sys

META = r"D:/project/ai_workspace/samples/cf_re/meta_dump_v5.fixed.bin"
FOFF = r"D:/project/ai_workspace/temp/fieldoffsets.json"
DATA_FILES = [r"D:/project/ai_workspace/temp/cf_offsets_full.bin"]
OUT = sys.argv[1] if len(sys.argv) > 1 else r"D:/project/ai_workspace/samples/cf_re/cf_runtime_offsets.tsv"

d = open(META, 'rb').read()
u = lambda o: struct.unpack_from('<I', d, o)[0]
us = lambda o: struct.unpack_from('<H', d, o)[0]
H = lambda i: (u(8+i*8), u(8+i*8+4))
str_off, str_size = H(2)
fields_off = H(11)[0]
typedefs_off = H(19)[0]
ntd = H(19)[1] // 104
TD, FD = 104, 16

# 字符串表
st = {}
p = str_off; e = str_off + str_size
while p < e:
    q = d.find(b'\x00', p)
    if q < 0 or q > e: break
    st[p - str_off] = d[p:q].decode('utf-8', 'replace')
    p = q + 1
def gs(i): return st.get(i, f'[?{i}]')

# typedef 记录: name/ns/fcnt/fstart
classes = []
for i in range(ntd):
    rec = typedefs_off + i*TD
    name = gs(u(rec)); ns = gs(u(rec+4))
    fstart, fcnt = u(rec+48), us(rec+84)
    if fcnt == 0xFFFF: fcnt = 0
    classes.append((i, ns, name, fstart, fcnt))

# 运行时 fieldOffsets 指针表
foffs = json.load(open(FOFF, encoding='utf-8'))
foffs = {int(k): int(v, 16) for k, v in foffs.items()}

# 偏移数组数据镜像（0x7141980000 基址）
img = b''.join(open(f, 'rb').read() for f in DATA_FILES)
IMG_BASE = 0x7141978000

def read_offs_arr(ptr, maxlen=4096):
    """从数据镜像读偏移数组（int32 序列），直到非偏移值或 maxlen"""
    off = ptr - IMG_BASE
    if off < 0 or off + 4 > len(img): return []
    vals = []
    for j in range(maxlen):
        o = off + j*4
        if o + 4 > len(img): break
        v = struct.unpack_from('<i', img, o)[0]
        # 偏移数组值特征: 0x0~0x1000 之间, 或 -1(静态)
        if v == -1 or (0 <= v <= 0x10000):
            vals.append(v)
        else:
            break
    return vals

# 合并输出
out = open(OUT, 'w', encoding='utf-8')
out.write("typedef\tclass\tfield\tfield_idx\toffset\n")
stats = {'total_field': 0, 'with_offs': 0, 'no_offs': 0, 'mismatch': 0}
for i, ns, name, fstart, fcnt in classes:
    if fcnt == 0: continue
    full = f'{ns}.{name}' if ns else name
    ptr = foffs.get(i, 0)
    if not ptr:
        stats['no_offs'] += 1
        continue
    arr = read_offs_arr(ptr, fcnt + 8)
    # 截断到 fcnt（或数组长度）
    n = min(fcnt, len(arr))
    if n < fcnt: stats['mismatch'] += 1
    for j in range(n):
        frec = fields_off + (fstart + j)*FD
        if frec + FD > len(d): break
        fname = gs(u(frec))
        off = arr[j]
        out.write(f"{i}\t{full}\t{fname}\t{j}\t0x{off & 0xffffffff:x}\n")
        stats['total_field'] += 1
    stats['with_offs'] += 1
out.close()
print(f"完成: 有字段类 {stats['with_offs']}, 无偏移 {stats['no_offs']}, 长度不匹配 {stats['mismatch']}, 字段总数 {stats['total_field']}")
print(f"输出: {OUT}")
