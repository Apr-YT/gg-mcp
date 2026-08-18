#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ptrscan.py — 多级指针扫描（CE 式），脚本化自动追到模块基址还原 base+offset
用法:
    python ptrscan.py <目标地址> [最大层数=4]
    python ptrscan.py libgame+0x702ED4

原理: 目标值 → ① gg_set_results 设为结果集 → ② pointer_scan 找"谁指向它"(容器)
      → ③ 对容器 pointer_chain 追级, 命中模块基址 → 输出静态路径
      → ④ 未命中则容器作为新目标迭代, 直到到达模块基址或空
输出: 所有 [模块基址 + 偏移 ... → 目标] 的指针链路径
"""
import sys
from ggghost import GG, _resolve


def chain_hits_module(gg, container):
    """追链, 返回 (是否命中模块, 命中细节)"""
    try:
        r = gg.pointer_chain(container, 8)
    except Exception as e:
        return False, None
    for lv in r.get("levels", []):
        if lv.get("module"):
            return True, lv
    return False, None


def scan(gg, target, max_depth):
    cur = target
    paths = []
    for d in range(max_depth):
        # 设结果集 = 当前目标, 扫描指向它的指针容器
        try:
            gg._call("gg_set_results", {"type": "qword",
                                        "values": [{"addr": cur, "value": "0"}]})
            r = gg.pointer_scan("qword", 5000)
        except Exception as e:
            print(f"[d{d}] scan err: {e}")
            break
        hits = r.get("hits", [])
        containers = [h["container"] for h in hits]
        print(f"[d{d}] {len(containers)} 个容器指向 {cur}")
        if not containers:
            print(f"[d{d}] 无指针容器 —— 目标可能被值直接引用(非指针链)")
            break

        # 对每个容器追链, 找命中模块基址的
        new_cur = None
        for c in containers[:12]:
            ok, lv = chain_hits_module(gg, c)
            if ok:
                paths.append({
                    "depth": d,
                    "container": c,
                    "module": lv["module"],
                    "module_offset": lv.get("module_offset", ""),
                    "final": lv.get("addr", ""),
                })
                print(f"   ✓ 容器 {c} 追到模块 {lv['module']} @ {lv.get('module_offset')}")
        if new_cur is None and containers:
            new_cur = containers[0]
        cur = new_cur
        if cur is None:
            break

    print("\n=== 静态指针路径（基址+偏移, 重启后仍可用）===")
    if not paths:
        print("(无模块命中路径——目标由 heap 直引, 无静态指针)")
    for p in paths:
        print(f"  {p['module']} + {p['module_offset']}  ←  容器 {p['container']}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    gg = GG()
    tgt = _resolve(gg, sys.argv[1])
    md = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    print(f"[目标] {tgt}")
    scan(gg, tgt, md)
