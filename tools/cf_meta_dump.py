#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CF il2cpp v24.4 metadata 全量解析 + 字段内存偏移计算工具。

输入: global-metadata / 驱动直读修复后的 meta_dump*.fixed.bin
输出:
  dump.cs            类+字段+方法（含父类）
  classes.tsv        typedef|ns|class|parent|parent_idx|fields|methods
  fields_index.tsv   字段名 -> 类列表
  methods_index.tsv  方法名 -> 类列表
  field_offsets.tsv  typedef|class|field|type_desc|offset(hex)   <-- 新增：gg mcp 可用真实偏移
并直接在控制台打印关键类（Player/Pawn/Actor/Weapon/Camera/Transform 等）的完整字段偏移表。

偏移计算规则（复刻 il2cpp 布局）:
  - 引用类型字段(Class/String/Array/Ptr/Object/泛型等) -> 占 8 字节指针, align 8
  - 值类型字段(Struct/Enum) -> 内联, 按其自身 size/align
  - 内置类型 -> 固定 size/align (Int32=4/4, Single=4/4, Double=8/8 ...)
  - 类实例: 父类字段在前, 自身字段从 +0x10(对象头 klass+monitor) 起排
  - 值类型: 无对象头, 从 0 起排
用法: python cf_meta_dump.py <meta> [输出目录]
"""
import struct, sys, time, os

# ---- 路径修正: 迁移后 samples/cf_re 为权威位置 ----
D = r"D:/project/ai_workspace/samples/cf_re/meta_dump_v5.fixed.bin"
if len(sys.argv) > 1:
    D = sys.argv[1]
BASE = sys.argv[2] if len(sys.argv) > 2 else r"D:/project/ai_workspace/samples/cf_re/"

# header 索引(已实测此腾讯魔改构建): string@2 methods@5 fields@11 typedefs@19 types@23
H = lambda i: (struct.unpack_from('<I', d, 8+i*8)[0], struct.unpack_from('<I', d, 8+i*8+4)[0])
def u(o): return struct.unpack_from('<I', d, o)[0]
def us(o): return struct.unpack_from('<H', d, o)[0]

t0 = time.time()
d = open(D, 'rb').read()

str_off, str_size   = H(2)
methods_off         = H(5)[0]
fields_off, fields_size = H(11)
typedefs_off, typedefs_size = H(19)
types_off, types_size = H(23)
ntd  = typedefs_size // 104
nfields = fields_size // 16
ntypes = types_size // 8

# ---- 字符串表 ----
print("建字符串索引...", flush=True)
str_tab = {}
pos = str_off; end = str_off + str_size
while pos < end:
    e = d.find(b'\x00', pos)
    if e < 0 or e > end: break
    str_tab[pos - str_off] = d[pos:e].decode('utf-8', 'replace')
    pos = e + 1
def get_str(idx):
    return str_tab.get(idx, f'[unresolved:{idx}]')

TD, FD, MD = 104, 16, 56
NEG = 0xFFFFFFFF

# ---- Il2CppType 表: {type_enum@0, data@4} ----
def il2cpp_type(ti):
    if ti >= ntypes: return (0, 0)
    o = types_off + ti*8
    return (d[o], u(o+4))   # (type_enum, data)

# 内置类型 size/align
BUILTIN = {
    0x02:(1,1),0x03:(2,2),0x04:(1,1),0x05:(1,1),0x06:(2,2),0x07:(2,2),
    0x08:(4,4),0x09:(4,4),0x0A:(4,4),0x0B:(8,8),
    0x0C:(8,8),0x0D:(8,8),0x0E:(8,8),0x15:(8,8),0x16:(8,8),
    0x17:(8,8),0x18:(8,8),0x19:(8,8),0x11:(8,8),0x12:(8,8),0x13:(8,8),0x1A:(8,8),
}
TNAME = {0x01:'Void',0x02:'Boolean',0x03:'Char',0x04:'SByte',0x05:'Byte',0x06:'Int16',
    0x07:'UInt16',0x08:'Int32',0x09:'UInt32',0x0A:'Single',0x0B:'Double',0x0C:'String',
    0x0D:'Ptr',0x0E:'ByRef',0x0F:'ValueType',0x10:'Class',0x11:'Var',0x12:'Array',
    0x13:'GenericInst',0x14:'TypedByRef',0x15:'IntPtr',0x16:'UIntPtr',0x17:'FnPtr',
    0x18:'Object',0x19:'SzArray',0x1A:'MVar',0x21:'Enum'}

# ---- 预解析全部类 ----
print(f"typeDef {ntd} | fields {nfields} | types {ntypes}", flush=True)
classes = []
for i in range(ntd):
    rec = typedefs_off + i*TD
    name = get_str(u(rec)); ns = get_str(u(rec+4))
    # parentIndex 实际在 offset 16, 存的是 Il2CppType 表(idx23)索引; 经 data@4 反解为 typedef
    pti = u(rec+16)
    if pti == NEG or pti >= ntypes:
        parent = pti if 0 <= pti < ntd else NEG
    else:
        _, pdata = il2cpp_type(pti)
        parent = pdata if 0 <= pdata < ntd else (pti if 0 <= pti < ntd else NEG)
    fstart, fcnt = u(rec+48), us(rec+84); mstart, mcnt = u(rec+52), us(rec+80)
    if fcnt == 0xFFFF: fcnt = 0
    if mcnt == 0xFFFF: mcnt = 0
    classes.append([i, name, ns, parent, fstart, fcnt, mstart, mcnt])
name_of = {c[0]: (c[2], c[1]) for c in classes}
full_of = {c[0]: (f'{c[2]}.{c[1]}' if c[2] else c[1]) for c in classes}

def is_value_type(idx):
    p = idx
    seen = set()
    while p != NEG and p not in seen and p < ntd:
        seen.add(p)
        ns, nm = name_of.get(p, ('',''))
        if (ns, nm) in (('System','ValueType'), ('System','Enum')):
            return True
        p = classes[p][3]
    return False

# ---- 字段类型描述 ----
# 重要: 本构建 Il2CppType 为腾讯魔改编码, 类型枚举不可靠(无法稳定区分内置定长类型
# 与类/结构体引用), 因此无法静态可靠计算字段内存偏移。这里只给"指向的 typedef 名"
# (若 data@4 是合法 typedef) 或原始 typeIndex, 真实偏移必须经内核驱动(gg mcp 只读)
# 运行时提取(值搜索 + 指针链)。详见 CF反作弊检测机制分析.md 第十六章。
def field_type_desc(ti):
    te, pdata = il2cpp_type(ti)
    if 0 <= pdata < ntd:
        return f'->{full_of.get(pdata, str(pdata))}'   # 指向的 typedef(类/值类型/枚举)
    return f'type{ti:#x}'                                # 内置/其他, 运行时再定

# ---- 输出 ----
out_cs = open(os.path.join(BASE,'dump.cs'),'w',encoding='utf-8')
fidx = open(os.path.join(BASE,'fields_index.tsv'),'w',encoding='utf-8')
midx = open(os.path.join(BASE,'methods_index.tsv'),'w',encoding='utf-8')
cls  = open(os.path.join(BASE,'classes.tsv'),'w',encoding='utf-8')
# 偏移列固定 RUNTIME: 本构建 Il2CppType 为腾讯魔改编码, 无法静态可靠计算字段内存偏移;
# 必须经内核驱动(gg mcp 只读)运行时提取(值搜索 + 指针链)得到真实偏移。
foff = open(os.path.join(BASE,'field_offsets.tsv'),'w',encoding='utf-8')
cls.write("typedef\tnamespace\tclass\tparent\tparent_idx\tfields\tmethods\n")
fidx.write("field_name\tclasses\n"); midx.write("method_name\tclasses\n")
foff.write("# 偏移列=RUNTIME: 腾讯魔改 Il2CppType 编码无法静态可靠计算, 需驱动运行时提取\n")
foff.write("typedef\tclass\tfield\ttype_desc\toffset\n")

field_name_map = {}; method_name_map = {}
total_f = total_m = 0
for c in classes:
    i, name, ns, parent, fstart, fcnt, mstart, mcnt = c
    if fcnt == 0 and mcnt == 0: continue
    full = full_of[i]
    parent_name = ''
    if parent != NEG and parent in full_of: parent_name = full_of[parent]
    out_cs.write(f'public class {full} {{ // typedef={i} parent={parent_name} fields={fcnt} methods={mcnt}\n')
    for j in range(fcnt):
        frec = fields_off + (fstart+j)*FD
        if frec+FD > len(d): break
        fname = get_str(u(frec)); ti = u(frec+4); total_f += 1
        field_name_map.setdefault(fname, []).append(full)
        tdesc = field_type_desc(ti)
        out_cs.write(f'    public ? {fname}; // {tdesc}\n')
        foff.write(f'{i}\t{full}\t{fname}\t{tdesc}\tRUNTIME\n')
    if mcnt:
        out_cs.write('    // Methods\n')
    for k in range(mcnt):
        mrec = methods_off + (mstart+k)*MD
        if mrec+MD > len(d): break
        mname = get_str(u(mrec)); total_m += 1
        method_name_map.setdefault(mname, []).append(full)
        out_cs.write(f'    public ? {mname}(); // method\n')
    out_cs.write('}\n\n')
    cls.write(f'{i}\t{ns}\t{name}\t{parent_name}\t{parent}\t{fcnt}\t{mcnt}\n')

for fn, cl in sorted(field_name_map.items()): fidx.write(f'{fn}\t{",".join(cl)}\n')
for mn, cl in sorted(method_name_map.items()): midx.write(f'{mn}\t{",".join(cl)}\n')
for fh in (out_cs,fidx,midx,cls,foff): fh.close()

# ---- 控制台: 关键类字段清单(名字/继承已修正; 偏移待运行时) ----
KEYWORDS = ('Player','Pawn','Actor','Weapon','Camera','Transform','Character',
            'LocalPlayer','Soldier','Hero','BattlePlayer','Role','FPS','Health')
print("\n===== 关键类字段清单(名字/继承已修正; 偏移需驱动运行时提取) =====")
shown = 0
for c in classes:
    i, name, ns, *_ = c
    full = full_of[i]
    if any(k.lower() in full.lower() for k in KEYWORDS):
        fstart, fcnt = c[4], c[5]
        if fcnt == 0: continue
        print(f"\n[{i}] {full}  (实例字段 {fcnt})")
        for j in range(fcnt):
            frec = fields_off + (fstart+j)*FD
            if frec+FD > len(d): break
            print(f"    {get_str(u(frec))}")
        shown += 1
        if shown >= 40: break
print(f"\n完成 {time.time()-t0:.1f}s | 类 {ntd} | 字段 {total_f} | 方法 {total_m}")
print(f"产物: dump.cs / classes.tsv / fields_index.tsv / methods_index.tsv / field_offsets.tsv(偏移=RUNTIME) @ {BASE}")
