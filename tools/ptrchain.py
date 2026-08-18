#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ptrchain.py — 正向指针链追踪（基址 + 偏移链 → 逐级解引用 → 目标）
用法:
    python ptrchain.py <base> <off1> [off2 ...] [--show N]
    python ptrchain.py libil2cpp.so+0x702ED4 0x10 0x28 0x8 --show 32
    python ptrchain.py 0x700592d000 0x30 0x100
base 支持: 0x... / 十进制 / 模块名+0x偏移 (复用模块基址解析)
每级: addr = prev + off → 读 qword 指针 → 下一级
指针读取用 view(int32,2) 拼 LE（绕开 gg_read qword 高低 32 位颠倒的 server bug）
"""
import sys
from ggghost import GG, _resolve


def parse_off(s):
    """0x10 / 16 / -0x8 → int"""
    return int(s, 16) if s.lower().startswith(('0x', '-0x')) else int(s)


def read_ptr(gg, addr):
    """view(int32,2) 拼 LE qword 指针, 返回 (ptr, [lo,hi])"""
    v = gg.view(hex(addr), elem="int32", count=2)
    els = v.get("elements", []) if isinstance(v, dict) else []
    if len(els) < 2:
        return None, None
    lo = els[0]["value"]
    hi = els[1]["value"]
    ptr = (lo & 0xFFFFFFFF) | ((hi & 0xFFFFFFFF) << 32)
    return ptr, [lo, hi]


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0)
    show = 16
    if "--show" in args:
        i = args.index("--show")
        show = int(args[i + 1])
        args = args[:i] + args[i + 2:]

    gg = GG()
    base = int(_resolve(gg, args[0]), 16)
    offs = args[1:]
    print(f"[基址] {args[0]} -> 0x{base:x}")

    prev = base
    for i, o in enumerate(offs):
        off = parse_off(o)
        addr = prev + off
        ptr, parts = read_ptr(gg, addr)
        if ptr is None:
            print(f"[级{i}] 0x{addr:x} 读取失败 (不可读/越界)")
            sys.exit(1)
        sign = "+" if off >= 0 else "-"
        print(f"[级{i}] 0x{prev:x} {sign}0x{abs(off):x} = 0x{addr:x} → ptr = 0x{ptr:x}  [{parts[0]:#x},{parts[1]:#x}]")
        if ptr == 0:
            print(f"  ⚠ 指针为 0 (链断裂/未初始化), 停止")
            sys.exit(1)
        prev = ptr

    print(f"\n[最终] 0x{prev:x}")
    if show:
        try:
            v = gg.view(hex(prev), elem="int32", count=max(1, show // 4))
            els = v.get("elements", []) if isinstance(v, dict) else []
            vals = [f"{e['value']:#x}" for e in els]
            print(f"  前 {len(els)} 个 int32: {', '.join(vals)}")
            p0, _ = read_ptr(gg, prev)
            print(f"  首 8 字节作指针 = 0x{p0:x}" if p0 is not None else "  首 8 字节不可读")
        except Exception as e:
            print(f"  读取数据失败: {e}")


if __name__ == "__main__":
    main()
