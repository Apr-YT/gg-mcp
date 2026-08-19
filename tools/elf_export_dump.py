#!/usr/bin/env python3
"""elf_export_dump.py — 解析 ELF (ARM64/ARM) 导出函数符号 + 提取可打印字符串

用法:
  python elf_export_dump.py exports <so>...     # 导出符号
  python elf_export_dump.py strings <so>...     # 字符串(ASCII+UTF-8, >=4字节)
  python elf_export_dump.py strfilter <pattern> <so>...  # 按正则过滤字符串

对多文件: 逐个输出, === 分隔。
"""
import struct, sys, re, glob

def parse_elf_exports(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF':
        return None, []
    is64 = data[4] == 2
    if is64:
        e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
        e_shentsize = struct.unpack_from('<H', data, 0x3A)[0]
        e_shnum = struct.unpack_from('<H', data, 0x3C)[0]
        e_shstrndx = struct.unpack_from('<H', data, 0x3E)[0]
    else:
        e_shoff = struct.unpack_from('<I', data, 0x20)[0]
        e_shentsize = struct.unpack_from('<H', data, 0x2E)[0]
        e_shnum = struct.unpack_from('<H', data, 0x30)[0]
        e_shstrndx = struct.unpack_from('<H', data, 0x32)[0]
    shdrs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if is64:
            sh_name, sh_type = struct.unpack_from('<II', data, off)
            sh_flags, sh_addr = struct.unpack_from('<QQ', data, off + 8)
            sh_offset, sh_size = struct.unpack_from('<QQ', data, off + 0x18)
            sh_link, sh_info = struct.unpack_from('<II', data, off + 0x28)
            sh_addralign, sh_entsize = struct.unpack_from('<QQ', data, off + 0x30)
        else:
            sh_name, sh_type = struct.unpack_from('<II', data, off)
            sh_flags, sh_addr = struct.unpack_from('<II', data, off + 8)
            sh_offset, sh_size = struct.unpack_from('<II', data, off + 0x10)
            sh_link, sh_info = struct.unpack_from('<II', data, off + 0x18)
            sh_addralign, sh_entsize = struct.unpack_from('<II', data, off + 0x20)
        shdrs.append(dict(name=sh_name, type=sh_type, offset=sh_offset, size=sh_size,
                          link=sh_link, info=sh_info, entsize=sh_entsize, addr=sh_addr))
    shstr = shdrs[e_shstrndx]
    shstr_data = data[shstr['offset']:shstr['offset'] + shstr['size']]
    def sname(off):
        end = shstr_data.find(b'\x00', off)
        return shstr_data[off:end].decode('utf-8', 'replace')
    names = {i: sname(s['name']) for i, s in enumerate(shdrs)}
    dynsym = dynstr = None
    for i, s in enumerate(shdrs):
        if names[i] == '.dynsym':
            dynsym = s
        if names[i] == '.dynstr':
            dynstr = s
    if dynsym is None or dynstr is None:
        return data, []
    entsize = dynsym['entsize'] or (24 if is64 else 16)
    n = dynsym['size'] // entsize
    strtab = data[dynstr['offset']:dynstr['offset'] + dynstr['size']]
    exports = []
    for i in range(1, n):
        off = dynsym['offset'] + i * entsize
        if is64:
            st_name, st_info, st_shndx, _ = struct.unpack_from('<IBBH', data, off)
            st_value = struct.unpack_from('<Q', data, off + 8)[0]
        else:
            st_name, st_value = struct.unpack_from('<II', data, off)
            st_info, _, st_shndx = struct.unpack_from('<BBH', data, off + 4)
        bind = st_info >> 4
        typ = st_info & 0xf
        if typ == 2 and bind == 1:  # STT_FUNC + STB_GLOBAL
            end = strtab.find(b'\x00', st_name)
            nm = strtab[st_name:end].decode('utf-8', 'replace')
            exports.append((st_value, nm))
    return data, sorted(exports)

ASCII_RE = re.compile(rb'[\x20-\x7e]{4,}')
def extract_strings(data, minlen=4):
    out = []
    for m in ASCII_RE.finditer(data):
        try:
            s = m.group().decode('utf-8')
        except Exception:
            continue
        if len(s) >= minlen:
            out.append(s)
    return out

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    op = sys.argv[1]
    files = []
    for a in sys.argv[2:]:
        files += glob.glob(a)
    for path in files:
        data, exps = parse_elf_exports(path)
        print(f'===== {path} =====')
        if op == 'exports':
            print(f'[导出函数 {len(exps)} 个]')
            for a, n in exps:
                print(f'  0x{a:x}  {n}')
        elif op == 'strings':
            strs = extract_strings(data)
            print(f'[字符串 {len(strs)} 条]')
            for s in strs:
                print(f'  {s}')
        elif op.startswith('strfilter'):
            pat = sys.argv[2]
            files2 = [a for a in sys.argv[3:]]
            # 简单重入: pattern 已消费
            rx = re.compile(pat.encode() if isinstance(pat, str) else pat)
            strs = [s for s in extract_strings(data) if rx.search(s)]
            print(f'[匹配 "{pat}" 的字符串 {len(strs)} 条]')
            for s in strs:
                print(f'  {s}')
        print()

if __name__ == '__main__':
    main()
