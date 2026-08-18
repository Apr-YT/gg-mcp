#!/usr/bin/env python3
"""extract_kernel.py — 从 Android boot.img 提取内核 Image 并解析导出符号 CRC 表
boot_img_hdr_v4: 4096 字节头; kernel 在 page_size 处; header 里 kernel_size + page_size 字段。
输出:
  kernel.bin         — 原始内核(可能 gzip)
  kernel.elf         — 解压后的 ELF (若 gzip)
  ksymtab.json       — {symbol: crc} 全表 (来自 __ksymtab + __kcrctab)
用法: python extract_kernel.py <boot.img> <outdir>
"""
import struct, sys, os, json, gzip

def parse_bootimg(path):
    with open(path, 'rb') as f:
        hdr = f.read(4096)
    magic = hdr[:8]
    print(f"magic: {magic}")
    if magic == b'ANDROID!':
        kernel_size = struct.unpack_from('<I', hdr, 8)[0]
        page_size = struct.unpack_from('<I', hdr, 40)[0]
        # v3/v4: 头 4096, kernel 从 4096 开始
        kernel_off = 4096
        print(f"v3/v4: kernel_size={kernel_size} page={page_size} off={kernel_off}")
        return kernel_off, kernel_size
    # v2 或更老 (4096 头, kernel 在 page_size)
    page_size = struct.unpack_from('<I', hdr, 40)[0] or 2048
    kernel_size = struct.unpack_from('<I', hdr, 8)[0]
    kernel_off = page_size
    print(f"v2: kernel_size={kernel_size} page={page_size} off={kernel_off}")
    return kernel_off, kernel_size

def read_kernel(path, off, size):
    with open(path, 'rb') as f:
        f.seek(off)
        return f.read(size)

def maybe_gunzip(data):
    if data[:2] == b'\x1f\x8b':
        print("kernel is gzip, decompressing...")
        return gzip.decompress(data)
    return data

def elf_sections(data):
    if data[:4] != b'\x7fELF':
        raise SystemExit("not ELF (maybe Image.gz or other format)")
    ei_class, ei_data = data[4], data[5]
    endian = '<' if ei_data == 1 else '>'
    if ei_class != 2:
        raise SystemExit("not ELF64")
    e_shoff = struct.unpack_from(endian + 'Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from(endian + 'H', data, 0x3a)[0]
    e_shnum = struct.unpack_from(endian + 'H', data, 0x3c)[0]
    e_shstrndx = struct.unpack_from(endian + 'H', data, 0x3e)[0]
    shstr_off = struct.unpack_from(endian + 'Q', data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]
    shstr_size = struct.unpack_from(endian + 'Q', data, e_shoff + e_shstrndx * e_shentsize + 0x20)[0]
    shstr = data[shstr_off:shstr_off + shstr_size]
    secs = {}
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name_off = struct.unpack_from(endian + 'I', data, off)[0]
        sh_type = struct.unpack_from(endian + 'I', data, off + 4)[0]
        sh_addr = struct.unpack_from(endian + 'Q', data, off + 0x10)[0]
        sh_offset = struct.unpack_from(endian + 'Q', data, off + 0x18)[0]
        sh_size = struct.unpack_from(endian + 'Q', data, off + 0x20)[0]
        name = shstr[name_off:shstr.find(b'\0', name_off)].decode('ascii', 'replace')
        secs[name] = (sh_addr, sh_offset, sh_size)
    return secs

def parse_ksymtab(data, secs):
    """__ksymtab: struct kernel_symbol { int value_offset; int name_offset; int namespace_offset; }
    每个条目地址 = 条目在 vmlinux 中的虚拟地址; name = __ksymtab_strings + name_offset (相对条目地址).
    __kcrctab: u32 数组, 顺序与 __ksymtab 一致 (CONFIG_MODVERSIONS).
    注意: 内核镜像没有 ELF section 头 (Image 是 stripped), 用符号表不可行;
    需要 __ksymtab 的 虚拟地址 与 文件偏移 的映射 → 通过 phdr 计算。"""
    # 用 program headers 建 vaddr->offset 映射
    ei_data = data[5]
    endian = '<' if ei_data == 1 else '>'
    e_phoff = struct.unpack_from(endian + 'Q', data, 0x20)[0]
    e_phentsize = struct.unpack_from(endian + 'H', data, 0x36)[0]
    e_phnum = struct.unpack_from(endian + 'H', data, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from(endian + 'I', data, off)[0]
        p_offset = struct.unpack_from(endian + 'Q', data, off + 8)[0]
        p_vaddr = struct.unpack_from(endian + 'Q', data, off + 0x10)[0]
        p_filesz = struct.unpack_from(endian + 'Q', data, off + 0x20)[0]
        if p_type == 1:  # PT_LOAD
            segs.append((p_vaddr, p_offset, p_filesz))
    def v2o(va):
        for (v, o, sz) in segs:
            if v <= va < v + sz:
                return o + (va - v)
        return None
    ksym_addr, ksym_off, ksym_size = secs.get('__ksymtab', (0, 0, 0))
    kcrc_addr, kcrc_off, kcrc_size = secs.get('__kcrctab', (0, 0, 0))
    str_addr, str_off, str_size = secs.get('__ksymtab_strings', (0, 0, 0))
    print(f"__ksymtab: addr=0x{ksym_addr:x} off=0x{ksym_off:x} size={ksym_size}")
    print(f"__kcrctab: addr=0x{kcrc_addr:x} off=0x{kcrc_off:x} size={kcrc_size}")
    print(f"__ksymtab_strings: addr=0x{str_addr:x} off=0x{str_off:x} size={str_size}")
    if not ksym_size or not kcrc_size:
        print("WARN: sections empty (kernel stripped sections?)")
    n = ksym_size // 12
    crcs = [struct.unpack_from('<I', data, kcrc_off + i * 4)[0] for i in range(min(n, kcrc_size // 4))]
    out = {}
    for i in range(n):
        ent_va = ksym_addr + i * 12
        ent_off = v2o(ent_va)
        if ent_off is None:
            continue
        name_off = struct.unpack_from('<i', data, ent_off + 4)[0]
        name_va = ent_va + 4 + name_off
        noff = v2o(name_va)
        if noff is None:
            continue
        end = data.find(b'\0', noff)
        name = data[noff:end].decode('ascii', 'replace')
        crc = crcs[i] if i < len(crcs) else 0
        out[name] = crc
    return out

def main():
    boot, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    off, size = parse_bootimg(boot)
    k = read_kernel(boot, off, size)
    open(os.path.join(outdir, 'kernel.bin'), 'wb').write(k)
    elf = maybe_gunzip(k)
    open(os.path.join(outdir, 'kernel.elf'), 'wb').write(elf)
    secs = elf_sections(elf)
    print("sections:", {s: hex(v[2]) for s, v in secs.items() if 'symtab' in s or s in ('__ksymtab', '__kcrctab', '__ksymtab_strings')})
    table = parse_ksymtab(elf, secs)
    with open(os.path.join(outdir, 'ksymtab.json'), 'w') as f:
        json.dump(table, f, indent=0)
    print(f"exported symbols: {len(table)} -> ksymtab.json")
    # 打印几个关键符号
    for s in ('module_layout', 'proc_create', 'proc_remove', 'copy_from_kernel_nofault', 'kallsyms_lookup_name'):
        print(f"  {s}: 0x{table.get(s, 0):08x}" if s in table else f"  {s}: (absent)")

if __name__ == '__main__':
    main()
