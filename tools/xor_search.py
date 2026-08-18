#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xor_search.py — XOR 加密值搜索工具（GG 修改器"此值被加密"等价物）
用法:
    python xor_search.py <明文值> [type] [region]     # 第一步: 对常用 XOR key 批量搜索变体
    python xor_search.py refine <新值>                 # 第二步: 玩家操作后, 对新值批量搜变体 + 交叉收敛

原理: 游戏用 明文 ^ key = 密文 存内存。key 未知时, 对一组常用 key 逐个搜索 v^key,
     玩家操作值变化后, 同组 key 搜索 v'^key 与第一轮交叉 → 同一 key 下两轮都命中的地址
     = 加密存储位置。

变体 key 集（可加）: 0(明文), 0xFFFFFFFF(全反), 0xFF/0xFFFF(字节/半字反), 常见魔数
"""
import sys
import time
from ggghost import GG, GGHostError

# 常用 XOR key 集（dword 32 位）
XOR_KEYS = [
    0x00000000,      # 明文（可能无加密）
    0xFFFFFFFF,      # 全位取反（最常见）
    0x000000FF,      # 低字节取反
    0x0000FF00,
    0x00FF0000,
    0xFF000000,
    0x0000FFFF,      # 低半字取反
    0xFFFF0000,
    0x12345678,      # 常见调试魔数
    0xDEADBEEF,
    0x5A5A5A5A,
    0xA5A5A5A5,
    0x55AA55AA,
    0x3C6F248,
]

STATE = "xor_search.state"


def search_variants(gg, value, typ="dword", region="all", limit=300):
    """对 value 的每个 XOR 变体搜索, 返回 {(addr, key)} 集合"""
    hits = {}
    for k in XOR_KEYS:
        cand = value ^ k
        try:
            r = gg.search(cand, typ, region)
            cnt = r.get("count", 0)
            if cnt == 0 or cnt > 5000:      # 命中过多视为噪声 key, 跳过
                print(f"  [key 0x{k:08X}] {cand} -> {cnt} (跳过: 过多/无)")
                continue
            res = gg.results(limit, typ).get("results", [])
            for it in res:
                a = it["addr"]
                hits.setdefault(a, set()).add(k)
            print(f"  [key 0x{k:08X}] {cand} -> {cnt} 命中")
        except GGHostError as e:
            print(f"  [key 0x{k:08X}] err: {e}")
    return hits


def first(value, typ="dword", region="all"):
    gg = GG()
    print(f"[第一步] 明文 {value} 的 XOR 变体搜索（{len(XOR_KEYS)} 个 key）:")
    hits = search_variants(gg, int(value), typ, region)
    with open(STATE, "w") as f:
        for addr in sorted(hits):
            f.write(f"{addr} {' '.join(hex(k) for k in hits[addr])}\n")
    print(f"\n[状态] {len(hits)} 候选存 {STATE}")
    print("[操作] 让数值变化(买/赚/升级), 然后: python xor_search.py refine <新值>")


def refine(new_value, typ="dword", region="all"):
    gg = GG()
    if not new_value:
        print("[用法] python xor_search.py refine <新值>")
        return
    # 读第一轮 state
    r1 = {}
    for line in open(STATE):
        parts = line.split()
        if parts:
            r1[parts[0]] = set(parts[1:])
    print(f"[第二轮] 明文 {new_value} 的 XOR 变体搜索:")
    hits2 = search_variants(gg, int(new_value), typ, region)
    # 交叉: 第一轮与第二轮同地址, 且有共同 key
    inter = {}
    for addr, keys1 in r1.items():
        if addr in hits2:
            common = keys1 & {hex(k) for k in hits2[addr]}
            if common:
                inter[addr] = common
    print(f"\n[交叉] 共 {len(inter)} 候选（两轮同地址且共同 key 一致）:")
    for addr in sorted(inter):
        print(f"  {addr}  key={'/'.join(sorted(inter[addr]))}")
    if not inter:
        print("[提示] 无交叉——key 不在常用集, 试 XOR_KEYS 加 key 或换 float/qword")
        return
    # 写测试: 每个候选改 999999, 1.5s 还原（看 UI 是否闪变 → 排除显示源）
    print("[写测] 逐个写 999999 后还原, 观察游戏数值是否闪变:")
    for addr in sorted(inter):
        gg.write(addr, "999999", typ)
    time.sleep(1.5)
    for addr in sorted(inter):
        gg.write(addr, new_value, typ)
    print("[写测] 完成。闪变=显示源(排除); 无闪变=可能逻辑源")
    print(f"[冻结测] 逐个: python ggghost.py freeze {sorted(inter)[0]} 999999")
    print("        游戏里买最贵的东西, 能买=逻辑源(锁定)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "refine":
        refine(sys.argv[2] if len(sys.argv) > 2 else None,
               sys.argv[3] if len(sys.argv) > 3 else "dword",
               sys.argv[4] if len(sys.argv) > 4 else "all")
    else:
        first(sys.argv[1],
              sys.argv[2] if len(sys.argv) > 2 else "dword",
              sys.argv[3] if len(sys.argv) > 3 else "all")
