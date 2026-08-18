#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
memdiff.py — 内存 diff（找变化区域/新对象分配）
对指定内存区域做两次采样对比, 输出所有变化的偏移与新旧值。
用途: 操作游戏后看哪些区域变了（找新对象/新数据结构/隐藏计数）
用法:
    python memdiff.py <addr> <size>            # 交互: 回车=采样, 再回车=对比
    python memdiff.py <addr> <size> auto <秒>  # 自动: 立即采样, 等 N 秒后自动对比
    addr 支持 libgame+0x 语法
"""
import sys
import time
from ggghost import GG, _resolve, GGHostError


def dump(gg, addr, size):
    """按 4 字节块读区域, 返回 {offset: value}"""
    vals = {}
    try:
        r = gg.view(hex(addr), "int32", size // 4 + 1)
        for e in r.get("elements", []):
            off = e.get("offset", 0)
            v = e.get("value", 0)
            if off < size:
                vals[off] = v
    except Exception:
        pass
    return vals


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    addr_s = sys.argv[1]
    size = int(sys.argv[2])
    auto_wait = float(sys.argv[4]) if len(sys.argv) > 3 and sys.argv[3] == "auto" else None

    gg = GG()
    try:
        base = int(_resolve(gg, addr_s), 16)
    except GGHostError:
        base = int(addr_s, 16)
    print(f"[区域] 0x{base:x} 大小 {size} 字节")

    def sample(tag):
        print(f"[{tag}] 采样中...")
        return dump(gg, base, size)

    s1 = sample("第一次")
    if auto_wait is not None:
        print(f"[等待] {auto_wait}s 后自动对比（此时去操作游戏）...")
        time.sleep(auto_wait)
    else:
        input("[操作] 让游戏数据变化后回车, 进行第二次采样...")
    s2 = sample("第二次")

    changes = sorted(set(s1.keys()) | set(s2.keys()))
    diffs = []
    for off in changes:
        v1 = s1.get(off)
        v2 = s2.get(off)
        if v1 != v2:
            diffs.append((off, v1, v2))

    print(f"\n=== 变化结果: {len(diffs)} 处 4 字节块 ===")
    if not diffs:
        print("(无变化)")
        return
    # 合并连续变化块为区间
    ranges = []
    for off, v1, v2 in diffs:
        if ranges and off == ranges[-1][1] + 4:
            ranges[-1] = (ranges[-1][0], off, ranges[-1][2])
        else:
            ranges.append((off, off, (v1, v2)))
    for r0, r1, (v1, v2) in ranges[:40]:
        span = (r1 - r0) // 4 + 1
        print(f"  0x{base + r0:x} [+0x{r0:x}] ({span} 块)  {v1} -> {v2}")
    if len(ranges) > 40:
        print(f"  ... 共 {len(ranges)} 个区间")


if __name__ == "__main__":
    main()
