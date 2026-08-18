#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CF metadata 全量解析工具 v2（AI 快速定位增强版）。
v24.4 布局：TypeDef 104B(fieldStart@48/field_count@84/methodStart@52/method_count@80/parent@24)；
Field 16B；Method 56B(CF 魔改，nameIndex@0/declaringType@4)。
输出：
  dump.cs            类+字段+方法（含父类）
  classes.tsv        typedef|ns|class|parent|fields|methods
  fields_index.tsv   字段名 -> 类列表（AI 按字段名秒定位）
  methods_index.tsv  方法名 -> 类列表
用法: python cf_meta_dump.py <meta> [前缀]
"""
import struct, sys, time, os

D = r"D:/project/ai_workspace/temp/gg_hillclimb/cf_re/meta_dump_v5.fixed.bin"
if len(sys.argv) > 1:
    D = sys.argv[1]
BASE = sys.argv[2] if len(sys.argv) > 2 else r"D:/project/ai_workspace/temp/gg_hillclimb/cf_re/"

t0 = time.time()
d = open(D, 'rb').read()
u = lambda o: struct.unpack_from('<I', d, o)[0]
us = lambda o: struct.unpack_from('<H', d, o)[0]

str_off, str_size = u(24), u(28)
fields_off = u(96)
methods_off = u(48)
typedefs_off, typedefs_size = u(160), u(164)

print("建字符串索引...", flush=True)
str_tab = {}
pos = str_off; end = str_off + str_size
while pos < end:
    e = d.find(b'\x00', pos)
    if e < 0 or e > end: break
    str_tab[pos - str_off] = d[pos:e].decode('utf-8', 'replace')
    pos = e + 1

def get_str(idx):
    if idx in str_tab:
        return str_tab[idx]
    return f'[unresolved:{idx}]'

TD, FD, MD = 104, 16, 56
ntd = typedefs_size // TD
NEG = 0xFFFFFFFF

print(f"typeDef {ntd} 条, fields 表 0x{fields_off:x}, methods 表 0x{methods_off:x} (56B/条)", flush=True)

# ---- 预解析所有类（名/ns/父类/字段/方法）----
t1 = time.time()
classes = []          # 全部类（含无字段类）
for i in range(ntd):
    rec = typedefs_off + i * TD
    name = get_str(u(rec))
    ns = get_str(u(rec + 4))
    parent = u(rec + 24)
    fstart, fcnt = u(rec + 48), us(rec + 84)
    mstart, mcnt = u(rec + 52), us(rec + 80)
    if fcnt == 0xFFFF: fcnt = 0
    if mcnt == 0xFFFF: mcnt = 0
    classes.append((i, name, ns, parent, fstart, fcnt, mstart, mcnt))

# 父类名映射（先建 name 表）
name_of = {i: (ns, name) for i, name, ns, *_ in classes}

# ---- 输出 dump.cs ----
out_cs = open(os.path.join(BASE, 'dump.cs'), 'w', encoding='utf-8')
out_cs.write("// CF il2cpp metadata dump v2 (v24.4)  %s\n" % time.strftime('%Y-%m-%d %H:%M:%S'))
out_cs.write("// 类 %d | 字段 ~302k | 方法 56B/条 | 类型索引需运行时 Il2CppType 数组解析\n\n" % ntd)

fidx = open(os.path.join(BASE, 'fields_index.tsv'), 'w', encoding='utf-8')
midx = open(os.path.join(BASE, 'methods_index.tsv'), 'w', encoding='utf-8')
cls  = open(os.path.join(BASE, 'classes.tsv'), 'w', encoding='utf-8')
cls.write("typedef\tnamespace\tclass\tparent\tparent_idx\tfields\tmethods\n")
fidx.write("field_name\tclasses\n")
midx.write("method_name\tclasses\n")

field_name_map = {}   # field_name -> [class_full]
method_name_map = {}  # method_name -> [class_full]

total_f = total_m = 0
for i, name, ns, parent, fstart, fcnt, mstart, mcnt in classes:
    if fcnt == 0 and mcnt == 0:
        continue
    full = f'{ns}.{name}' if ns else name
    parent_name, parent_idx = '', parent
    if parent != NEG and parent in name_of:
        pns, pname = name_of[parent]
        parent_name = f'{pns}.{pname}' if pns else pname
    out_cs.write(f'public class {full} {{ // typedef={i} parent={parent_name} fields={fcnt} methods={mcnt}\n')
    # 字段
    for j in range(fcnt):
        frec = fields_off + (fstart + j) * FD
        if frec + FD > len(d): break
        fname = get_str(u(frec))
        ftype = u(frec + 4)
        total_f += 1
        field_name_map.setdefault(fname, []).append(full)
        out_cs.write(f'    public ? {fname}; // type={ftype}\n')
    # 方法
    if mcnt:
        out_cs.write('    // Methods\n')
    for k in range(mcnt):
        mrec = methods_off + (mstart + k) * MD
        if mrec + MD > len(d): break
        mname = get_str(u(mrec))
        total_m += 1
        method_name_map.setdefault(mname, []).append(full)
        out_cs.write(f'    public ? {mname}(); // method\n')
    out_cs.write('}\n\n')
    cls.write(f'{i}\t{ns}\t{name}\t{parent_name}\t{parent_idx}\t{fcnt}\t{mcnt}\n')

# ---- 索引文件 ----
for fname, cl in sorted(field_name_map.items()):
    fidx.write(f'{fname}\t{",".join(cl)}\n')
for mname, cl in sorted(method_name_map.items()):
    midx.write(f'{mname}\t{",".join(cl)}\n')

fidx.close(); midx.close(); cls.close(); out_cs.close()
el = time.time() - t1
print(f"完成 {el:.1f}s: 类 {ntd} | 字段 {total_f} (唯一名 {len(field_name_map)}) | 方法 {total_m} (唯一名 {len(method_name_map)})")
print(f"输出: dump.cs / classes.tsv / fields_index.tsv / methods_index.tsv @ {BASE}")
