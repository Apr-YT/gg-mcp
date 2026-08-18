#!/usr/bin/env python3
"""patch_module_layout_crc.py — 用设备厂商模块的 module_layout CRC 修补自编译 .ko 的 __versions
原理: CONFIG_MODVERSIONS 下内核按符号 CRC 校验; module_layout 是复合 ABI 指纹,
      设备内核(Xiaomi abogki 改动)与 vanilla GKI 的该 CRC 不同, 其余符号 CRC 一致。
      从设备已加载厂商模块的 __versions 提取真实 CRC, 覆写目标 .ko 的对应条目。
用法: python patch_module_layout_crc.py <target.ko> <device_vendor.ko> [符号名=module_layout]
"""
import struct, sys, os

def elf_sections(path):
    """返回 {section_name: (sh_offset, sh_size, sh_type)}"""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF':
        raise SystemExit(f"{path}: not ELF")
    ei_class = data[4]
    ei_data = data[5]
    endian = '<' if ei_data == 1 else '>'
    if ei_class != 2:
        raise SystemExit(f"{path}: not ELF64")
    # ELF64 header
    e_shoff = struct.unpack_from(endian + 'Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from(endian + 'H', data, 0x3a)[0]
    e_shnum = struct.unpack_from(endian + 'H', data, 0x3c)[0]
    e_shstrndx = struct.unpack_from(endian + 'H', data, 0x3e)[0]
    # section header: sh_name(4) sh_type(4) sh_flags(8) sh_addr(8) sh_offset(8) sh_size(8) sh_link(4) sh_info(4) sh_addralign(8) sh_entsize(8)
    shstr_off = struct.unpack_from(endian + 'Q', data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]
    shstr_size = struct.unpack_from(endian + 'Q', data, e_shoff + e_shstrndx * e_shentsize + 0x20)[0]
    shstr = data[shstr_off:shstr_off + shstr_size]
    secs = {}
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name_off = struct.unpack_from(endian + 'I', data, off)[0]
        sh_type = struct.unpack_from(endian + 'I', data, off + 4)[0]
        sh_offset = struct.unpack_from(endian + 'Q', data, off + 0x18)[0]
        sh_size = struct.unpack_from(endian + 'Q', data, off + 0x20)[0]
        name = shstr[name_off:shstr.find(b'\0', name_off)].decode('ascii', 'replace')
        secs[name] = (sh_offset, sh_size, sh_type)
    return secs, data

def read_versions(ko_path):
    """解析 __versions: 返回 {name: (file_offset_of_crc, crc)}"""
    secs, data = elf_sections(ko_path)
    if '__versions' not in secs:
        return {}, None
    off, size, _ = secs['__versions']
    MODULE_NAME_LEN = 56          # 64 - sizeof(unsigned long)
    entry = 8 + MODULE_NAME_LEN   # 64
    out = {}
    for i in range(size // entry):
        eoff = off + i * entry
        crc = struct.unpack_from('<I', data, eoff)[0]
        name = data[eoff + 8: eoff + 8 + MODULE_NAME_LEN].split(b'\0')[0].decode('ascii', 'replace')
        out[name] = (eoff, crc)
    return out, data

def main():
    target, vendor = sys.argv[1], sys.argv[2]
    sym = sys.argv[3] if len(sys.argv) > 3 else 'module_layout'
    tv, tdata = read_versions(target)
    vv, vdata = read_versions(vendor)
    if sym not in tv:
        raise SystemExit(f"{target}: __versions 无 {sym}")
    if sym not in vv:
        raise SystemExit(f"{vendor}: __versions 无 {sym} (换一个厂商模块)")
    t_off, t_crc = tv[sym]
    v_off, v_crc = vv[sym]
    print(f"{target} {sym} CRC = 0x{t_crc:08x}")
    print(f"{vendor} {sym} CRC = 0x{v_crc:08x}")
    if t_crc == v_crc:
        print("CRC 已一致, 无需修补")
        return 0
    with open(target, 'r+b') as f:
        f.seek(t_off)
        f.write(struct.pack('<I', v_crc))
    print(f"PATCHED: 0x{t_crc:08x} -> 0x{v_crc:08x} @ file offset 0x{t_off:x}")
    # verify
    tv2, _ = read_versions(target)
    print(f"VERIFY: {sym} CRC = 0x{tv2[sym][1]:08x}")
    return 0

if __name__ == '__main__':
    sys.exit(main())
