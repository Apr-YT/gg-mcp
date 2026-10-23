"""arm64_globals.py — 从 ARM64 代码里反推全局变量地址（ADRP/ADD/LDR）

为什么要它
----------
社区工具（frida-ue4dump / UE4Dumper）靠 hook 函数读寄存器拿全局指针
（比如 GNames）。hook 需要注入器和执行流拦截，我们没装 frida。

但同一个答案可以静态算出来：编译器访问全局变量只有两种套路

    ADRP xN, page          ; xN = (PC & ~0xFFF) + (imm << 12)
    ADD  xM, xN, #off      ; 目标 = xN + off          ← 直接取址
    LDR  xM, [xN, #off]    ; 目标 = xN + off，再解引用 ← 取指针内容

把这 3 条指令解出来就够了，不需要完整反汇编器（也不需要 capstone）。

用法
----
    # 离线：在 .so 文件里分析某个函数（要求 vaddr==file offset，或自动按段换算）
    python scripts/arm64_globals.py build/libUE4.so 0x662ca64 --count 16

    # 已知字节（比如从 gg_hex_dump 拿到的运行时内存）
    python scripts/arm64_globals.py --hex "480216f0 08014091" --base 0x1234

输出每个 ADRP 及其后续 ADD/LDR 的解析结果，标注是"取址"还是"解引用"。
"""

import struct
import sys

# ---- 指令编码 ----------------------------------------------------------
# ADRP : op(1)=1 immlo(2) 10000 immhi(19) Rd(5)
ADRP_MASK, ADRP_VAL = 0x9F000000, 0x90000000
# ADD  (imm, 64bit) : sf=1 op=0 S=0 100010 sh imm12 Rn Rd
ADD64_MASK, ADD64_VAL = 0xFF800000, 0x91000000
# SUB  (imm, 64bit) 也常用来算地址（负偏移）
SUB64_MASK, SUB64_VAL = 0xFF800000, 0xD1000000
# LDR  (imm, 64bit) : size=11 111 V=0 01 opc=01 imm12 Rn Rt
LDR64_MASK, LDR64_VAL = 0xFFC00000, 0xF9400000
# LDR  (imm, 32bit)
LDR32_MASK, LDR32_VAL = 0xFFC00000, 0xB9400000
# LDRB (imm)
LDRB_MASK, LDRB_VAL = 0xFFC00000, 0x39400000


def sext(v, bits):
    """符号扩展到 64 位"""
    if v & (1 << (bits - 1)):
        v -= 1 << bits
    return v


def decode_adrp(insn, pc):
    """返回 (rd, target_page)"""
    immlo = (insn >> 29) & 0x3
    immhi = (insn >> 5) & 0x7FFFF
    imm = sext((immhi << 2) | immlo, 21) << 12
    rd = insn & 0x1F
    return rd, ((pc & ~0xFFF) + imm) & 0xFFFFFFFFFFFFFFFF


def decode_add_sub(insn):
    """返回 (rn, rd, imm, is_sub, shift) —— 仅 64 位立即数形式"""
    is_sub = (insn & SUB64_MASK) == SUB64_VAL
    rn = (insn >> 5) & 0x1F
    rd = insn & 0x1F
    imm12 = (insn >> 10) & 0xFFF
    sh = (insn >> 22) & 0x1
    imm = imm12 << (12 if sh else 0)
    return rn, rd, imm, is_sub, sh


def decode_ldr(insn):
    """返回 (rn, rt, disp, kind) —— kind: 'ldr64'/'ldr32'/'ldrb'"""
    rn = (insn >> 5) & 0x1F
    rt = insn & 0x1F
    imm12 = (insn >> 10) & 0xFFF
    if (insn & LDR64_MASK) == LDR64_VAL:
        return rn, rt, imm12 * 8, "ldr64"
    if (insn & LDR32_MASK) == LDR32_VAL:
        return rn, rt, imm12 * 4, "ldr32"
    if (insn & LDRB_MASK) == LDRB_VAL:
        return rn, rt, imm12, "ldrb"
    return None


def analyze(code, base_addr, max_insns=64, verbose=True):
    """扫描一段代码，返回 [(insn_addr, kind, target, extra), ...]

    kind: 'adrp+add'  → target 是变量地址本身
          'adrp+ldr'  → target 是"存放指针的槽位"，需要再解引用一次
    """
    out = []
    adrp_state = {}          # rd -> (page, 产生它的指令地址)
    n = min(len(code) // 4, max_insns)
    for i in range(n):
        insn = struct.unpack_from("<I", code, i * 4)[0]
        pc = base_addr + i * 4

        if (insn & ADRP_MASK) == ADRP_VAL:
            rd, page = decode_adrp(insn, pc)
            adrp_state[rd] = (page, pc)
            continue

        ldr = decode_ldr(insn)
        if ldr:
            rn, rt, disp, kind = ldr
            if rn in adrp_state:
                page, src = adrp_state[rn]
                out.append((pc, "adrp+ldr", (page + disp) & 0xFFFFFFFFFFFFFFFF,
                            {"reg": f"x{rt}", "dst_reg": f"x{rn}", "src_pc": src,
                             "load": kind, "disp": disp}))
            continue

        if (insn & ADD64_MASK) in (ADD64_VAL, SUB64_VAL):
            rn, rd, imm, is_sub, sh = decode_add_sub(insn)
            if rn in adrp_state:
                page, src = adrp_state[rn]
                target = (page - imm if is_sub else page + imm) & 0xFFFFFFFFFFFFFFFF
                out.append((pc, "adrp+add", target,
                            {"reg": f"x{rd}", "dst_reg": f"x{rn}", "src_pc": src,
                             "imm": -imm if is_sub else imm}))
            continue
    return out


def load_so_section(path, vaddr, length):
    """从 ELF 文件按虚拟地址读代码（按 program header 换算文件偏移）"""
    with open(path, "rb") as f:
        hdr = f.read(0x1000)
        e_phoff, = struct.unpack_from("<Q", hdr, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", hdr, 0x36)
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, = struct.unpack_from("<I", hdr, off)
            p_offset, p_vaddr, _pa, p_filesz, _ms, _fl, _al = struct.unpack_from(
                "<QQQQQQQ", hdr, off + 8)
            if p_type == 1 and p_vaddr <= vaddr < p_vaddr + p_filesz:
                f.seek(p_offset + (vaddr - p_vaddr))
                return f.read(length)
    raise SystemExit(f"vaddr 0x{vaddr:X} 不在任何 PT_LOAD 段内")


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__)
        return 1

    if "--hex" in argv:
        i = argv.index("--hex")
        hexs = argv[i + 1].replace(" ", "")
        base = 0
        if "--base" in argv:
            base = int(argv[argv.index("--base") + 1], 0)
        code = bytes.fromhex(hexs)
        results = analyze(code, base)
    else:
        path = argv[0]
        vaddr = int(argv[1], 0)
        count = 64
        if "--count" in argv:
            count = int(argv[argv.index("--count") + 1])
        code = load_so_section(path, vaddr, count * 4)
        results = analyze(code, vaddr, count)

    if not results:
        print("没找到 ADRP+ADD / ADRP+LDR 组合")
        return 1

    print(f"{'指令地址':>14}  {'类型':<10} {'目标':>16}  说明")
    print("-" * 78)
    for pc, kind, target, meta in results:
        note = ""
        if kind == "adrp+ldr":
            note = f"{meta['load']} x{meta['dst_reg']}->[{meta['dst_reg']}+0x{meta['disp']:X}] 取指针内容"
        else:
            note = f"add/sub #{meta['imm']:+d}，直接取址"
        print(f"0x{pc:>12X}  {kind:<10} 0x{target:>14X}  {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
