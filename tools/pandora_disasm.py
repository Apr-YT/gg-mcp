# -*- coding: utf-8 -*-
"""libpandora.so 反汇编工具：按 VA 反汇编，输出带注释。
用法: python pandora_disasm.py <va_hex> [len] [--refs <csv>]
--refs: 附加的符号/地址标注 csv "va,name,size"
"""
import struct, sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

SO = r"D:/project/ai_workspace/temp/scratch/libpandora.so"
data = open(SO, "rb").read()

# ---- ELF 解析 ----
def load_segments():
    assert data[:4] == b"\x7fELF"
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags = struct.unpack_from("<II", data, off)
        p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from("<QQQQQQ", data, off + 8)
        segs.append((p_type, p_offset, p_vaddr, p_filesz, p_memsz))
    return segs

SEGS = load_segments()

def va2off(va):
    for p_type, p_offset, p_vaddr, p_filesz, p_memsz in SEGS:
        if p_type == 1 and p_vaddr <= va < p_vaddr + p_filesz:
            return p_offset + (va - p_vaddr)
    return None

def read_va(va, n):
    off = va2off(va)
    if off is None: return None
    return data[off:off+n]

def get_syms():
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]
    def sh(i):
        off = e_shoff + i * e_shentsize
        return struct.unpack_from("<IIQQQQIIQQ", data, off)
    shstr = sh(e_shstrndx)
    shstr_data = data[shstr[4]:shstr[4]+shstr[5]]
    def sh_name(idx):
        end = shstr_data.find(b"\0", idx)
        return shstr_data[idx:end].decode(errors="replace")
    dynsym = dynstr = None
    for i in range(e_shnum):
        s = sh(i)
        nm = sh_name(s[0])
        if nm == ".dynsym": dynsym = s
        elif nm == ".dynstr": dynstr = s
    syms = {}
    for i in range(dynsym[5] // 24):
        off = dynsym[4] + i * 24
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, off)
        if st_shndx == 0 or st_name == 0: continue
        end = dynstr[4] + st_name
        end2 = data.find(b"\0", end)
        name = data[end:end2].decode(errors="replace")
        syms[st_value] = (name, st_size)
    return syms

SYMS = get_syms()

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    va = int(args[0], 16)
    length = int(args[1], 16) if len(args) > 1 else 0x400
    code = read_va(va, length)
    if code is None:
        print(f"VA 0x{va:x} 不在文件段内")
        return
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    print(f";; disasm 0x{va:x} len=0x{length:x} (file off 0x{va2off(va):x})")
    for insn in md.disasm(code, va):
        line = f"0x{insn.address:x}: {insn.mnemonic:8s} {insn.op_str}"
        # 符号标注
        mark = ""
        if insn.mnemonic == "bl":
            tgt = int(insn.op_str.replace(" ", "").lstrip("#"), 16)
            if tgt in SYMS:
                mark = f"   ; -> {SYMS[tgt][0]}"
        elif insn.mnemonic == "b" and insn.op_str.startswith("0x"):
            tgt = int(insn.op_str.replace(" ", "").lstrip("#"), 16)
            if tgt in SYMS:
                mark = f"   ; -> {SYMS[tgt][0]}"
        print(line + mark)

if __name__ == "__main__":
    main()
