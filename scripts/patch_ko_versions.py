#!/usr/bin/env python3
"""patch_ko_versions.py — 用设备厂商模块的 __versions 建符号→CRC 映射, 批量修补自编译 .ko
用法: python patch_ko_versions.py <device_ko_dir> <target.ko> [target.ko ...]
对每个 target: 其 __versions 中每个符号若在映射表里且 CRC 不同 → 覆写为设备 CRC。
映射表中没有的符号 → 报告 (需内核镜像 __kcrctab 或 kallsyms 方案兜底)。
"""
import struct, sys, os, glob

MODULE_NAME_LEN = 56
ENTRY = 8 + MODULE_NAME_LEN

def read_versions(ko_path):
    with open(ko_path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF':
        raise SystemExit(f"{ko_path}: not ELF")
    ei_class, ei_data = data[4], data[5]
    endian = '<' if ei_data == 1 else '>'
    if ei_class != 2:
        raise SystemExit(f"{ko_path}: not ELF64")
    e_shoff = struct.unpack_from(endian + 'Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from(endian + 'H', data, 0x3a)[0]
    e_shnum = struct.unpack_from(endian + 'H', data, 0x3c)[0]
    e_shstrndx = struct.unpack_from(endian + 'H', data, 0x3e)[0]
    shstr_off = struct.unpack_from(endian + 'Q', data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]
    shstr_size = struct.unpack_from(endian + 'Q', data, e_shoff + e_shstrndx * e_shentsize + 0x20)[0]
    shstr = data[shstr_off:shstr_off + shstr_size]
    versions = {}
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name_off = struct.unpack_from(endian + 'I', data, off)[0]
        sh_offset = struct.unpack_from(endian + 'Q', data, off + 0x18)[0]
        sh_size = struct.unpack_from(endian + 'Q', data, off + 0x20)[0]
        name = shstr[name_off:shstr.find(b'\0', name_off)].decode('ascii', 'replace')
        if name != '__versions':
            continue
        for j in range(sh_size // ENTRY):
            eoff = sh_offset + j * ENTRY
            crc = struct.unpack_from('<I', data, eoff)[0]
            sym = data[eoff + 8: eoff + 8 + MODULE_NAME_LEN].split(b'\0')[0].decode('ascii', 'replace')
            versions[sym] = (eoff, crc)
    return versions, data

def build_map(ko_dir):
    """扫描目录内所有 .ko, 建 symbol -> crc 映射 (以最后出现的为准)"""
    m = {}
    files = glob.glob(os.path.join(ko_dir, '*.ko'))
    for fp in files:
        try:
            v, _ = read_versions(fp)
        except Exception as e:
            print(f"  skip {os.path.basename(fp)}: {e}")
            continue
        for sym, (_, crc) in v.items():
            m[sym] = crc
    print(f"map: {len(files)} modules, {len(m)} unique symbols")
    return m

def patch(target, m):
    v, data = read_versions(target)
    if not v:
        print(f"{target}: no __versions")
        return
    patched, missing, same = 0, [], 0
    with open(target, 'r+b') as f:
        for sym, (off, crc) in v.items():
            if sym not in m:
                missing.append(sym)
                continue
            dcr = m[sym]
            if dcr == crc:
                same += 1
                continue
            f.seek(off)
            f.write(struct.pack('<I', dcr))
            patched += 1
            print(f"  {target}: {sym} 0x{crc:08x} -> 0x{dcr:08x}")
    print(f"{target}: patched={patched} same={same} missing={len(missing)}")
    if missing:
        print(f"  MISSING from device map: {', '.join(missing)}")

if __name__ == '__main__':
    ko_dir, targets = sys.argv[1], sys.argv[2:]
    m = build_map(ko_dir)
    for t in targets:
        patch(t, m)
