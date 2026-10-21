#!/usr/bin/env python3
"""patch_ko_versions.py — 用设备厂商模块的符号 CRC 映射，批量修补自编译 .ko

用法: python patch_ko_versions.py <device_ko_dir> <target.ko> [target.ko ...]

背景（2026-10-21 补充）：
  设备内核开 CONFIG_EXTENDED_MODVERSIONS 时，`check_version()` 优先读
  `__version_ext_names` + `__version_ext_crcs`，**旧式 `__versions` 只作回退**。
  旧脚本只改 `__versions`，在 ext 内核上等于没改（仍报
  "disagrees about version of symbol module_layout"）。这里两种都改。

  Xiaomi 改过内核结构体，符号 CRC 与 vanilla GKI 大面积不符，
  因此映射表必须来自**设备自带的厂商模块**（/vendor/lib/modules），
  它们与设备内核是同一套源码编出来的。
"""
import glob
import os
import struct
import sys

MODULE_NAME_LEN = 56
OLD_ENTRY = 8 + MODULE_NAME_LEN          # struct modversion_info: u32 crc + char name[56]


def sections(data):
    """返回 {section_name: (offset, size)}，仅解析 64 位 ELF。"""
    if data[:4] != b'\x7fELF':
        raise SystemExit("not ELF")
    if data[4] != 2:
        raise SystemExit("not ELF64")
    endian = '<' if data[5] == 1 else '>'
    e_shoff = struct.unpack_from(endian + 'Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from(endian + 'H', data, 0x3A)[0]
    e_shnum = struct.unpack_from(endian + 'H', data, 0x3C)[0]
    e_shstrndx = struct.unpack_from(endian + 'H', data, 0x3E)[0]
    shstr_off = struct.unpack_from(endian + 'Q', data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]
    shstr_size = struct.unpack_from(endian + 'Q', data, e_shoff + e_shstrndx * e_shentsize + 0x20)[0]
    shstr = data[shstr_off:shstr_off + shstr_size]

    out = {}
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name_off = struct.unpack_from(endian + 'I', data, off)[0]
        sh_offset = struct.unpack_from(endian + 'Q', data, off + 0x18)[0]
        sh_size = struct.unpack_from(endian + 'Q', data, off + 0x20)[0]
        end = shstr.find(b'\0', name_off)
        name = shstr[name_off:end].decode('ascii', 'replace')
        out[name] = (sh_offset, sh_size)
    return out


def crc_sites(data):
    """列出所有需要改写的 CRC 位置: {sym: [file_offset, ...]}（可能新旧两套）。"""
    sec = sections(data)
    sites = {}

    # 旧式 __versions: 每条 = u32 crc + char name[56]
    if '__versions' in sec:
        off, size = sec['__versions']
        for j in range(size // OLD_ENTRY):
            eoff = off + j * OLD_ENTRY
            sym = data[eoff + 8:eoff + 8 + MODULE_NAME_LEN].split(b'\0')[0].decode('ascii', 'replace')
            if sym:
                sites.setdefault(sym, []).append(eoff)

    # 扩展式: __version_ext_names（NUL 分隔）与 __version_ext_crcs（u32 数组）一一对应
    if '__version_ext_names' in sec and '__version_ext_crcs' in sec:
        noff, nsize = sec['__version_ext_names']
        coff, csize = sec['__version_ext_crcs']
        blob = data[noff:noff + nsize]
        names = [x.decode('ascii', 'replace') for x in blob.split(b'\0') if x]
        n = min(len(names), csize // 4)
        for i in range(n):
            sites.setdefault(names[i], []).append(coff + i * 4)

    return sites


def build_map(ko_dir):
    """扫描设备厂商模块，建 symbol -> crc。ext 优先（与内核读取顺序一致）。"""
    m = {}
    files = sorted(glob.glob(os.path.join(ko_dir, '*.ko')))
    for fp in files:
        try:
            data = open(fp, 'rb').read()
            sec = sections(data)
        except Exception as e:
            print(f"  skip {os.path.basename(fp)}: {e}")
            continue
        # 先旧式，再 ext 覆盖（ext 更权威）
        if '__versions' in sec:
            off, size = sec['__versions']
            for j in range(size // OLD_ENTRY):
                eoff = off + j * OLD_ENTRY
                crc = struct.unpack_from('<I', data, eoff)[0]
                sym = data[eoff + 8:eoff + 8 + MODULE_NAME_LEN].split(b'\0')[0].decode('ascii', 'replace')
                if sym:
                    m[sym] = crc
        if '__version_ext_names' in sec and '__version_ext_crcs' in sec:
            noff, nsize = sec['__version_ext_names']
            coff, csize = sec['__version_ext_crcs']
            names = [x.decode('ascii', 'replace') for x in data[noff:noff + nsize].split(b'\0') if x]
            for i in range(min(len(names), csize // 4)):
                m[names[i]] = struct.unpack_from('<I', data, coff + i * 4)[0]
    print(f"map: {len(files)} modules, {len(m)} unique symbols")
    return m


def patch(target, m):
    data = open(target, 'rb').read()
    sites = crc_sites(data)
    if not sites:
        print(f"{target}: 没有 __versions / __version_ext_* 段")
        return 0

    buf = bytearray(data)
    patched = same = 0
    missing = []
    for sym, offs in sites.items():
        if sym not in m:
            missing.append(sym)
            continue
        dcr = m[sym]
        for off in offs:
            cur = struct.unpack_from('<I', buf, off)[0]
            if cur == dcr:
                same += 1
                continue
            struct.pack_into('<I', buf, off, dcr)
            patched += 1
            print(f"  {os.path.basename(target)}: {sym} 0x{cur:08x} -> 0x{dcr:08x}")

    if patched:
        open(target, 'wb').write(buf)
    print(f"{target}: patched={patched} same={same} missing={len(missing)}")
    if missing:
        print(f"  MISSING from device map: {', '.join(sorted(missing))}")
    return patched


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    ko_dir, targets = sys.argv[1], sys.argv[2:]
    mapping = build_map(ko_dir)
    for t in targets:
        patch(t, mapping)
