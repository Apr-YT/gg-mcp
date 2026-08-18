#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
hillclimb_coin.py — 登山赛车(Hill Climb Racing)金币脚本
等价 GG 修改器的 .lua 脚本。用法:
    python hillclimb_coin.py freeze [值]   # 按静态偏移 libgame+0x702ED4 冻结金币(默认 999999)
    python hillclimb_coin.py unfreeze       # 解冻
    python hillclimb_coin.py check          # 读当前金币(验证偏移仍有效)
    python hillclimb_coin.py locate         # 重新定位金币(两轮精确交叉, 存档值变化后用)
"""
import sys
from ggghost import GG, _resolve, GGHostError

# 静态偏移（2026-08-18 两轮 ASLR 验证稳定）
COIN_OFFSET = "libgame+0x702ED4"
MIRROR_HINT = "镜像副本在 heap(无静态偏移); 逻辑金币=libgame+0x702ED4"
_STATE = "hillclimb_coin.state"


def freeze(value="999999"):
    gg = GG()
    addr = _resolve(gg, COIN_OFFSET)
    cur = gg.read(addr, "dword").get("value")
    print(f"[金币] 当前 {cur} @ {addr}")
    r = gg.freeze(addr, value, "dword")
    print(f"[冻结] {value} -> ok={r.get('ok')} frozen={r.get('frozen')}")
    print(f"[提示] {MIRROR_HINT}")


def unfreeze():
    gg = GG()
    addr = _resolve(gg, COIN_OFFSET)
    print(gg.unfreeze(addr))


def check():
    gg = GG()
    addr = _resolve(gg, COIN_OFFSET)
    v = gg.read(addr, "dword").get("value")
    print(f"[金币] @ {addr} = {v}  (偏移有效)")


def locate(v1=None):
    """第一步: 搜当前金币值, 保存候选到 state, 提示操作。
    用法: python hillclimb_coin.py locate [当前金币值]  (不传则自动读偏移地址现值)"""
    gg = GG()
    if not v1:
        try:
            v1 = str(gg.read(_resolve(gg, COIN_OFFSET), "dword").get("value"))
        except Exception:
            v1 = input("当前金币值: ").strip()
    n1 = gg.search(v1, "dword", "all").get("count")
    print(f"[第一轮] {v1} -> {n1} 候选")
    r1 = set(a["addr"] for a in gg.results(200, "dword").get("results", []))
    with open(_STATE, "w") as f:
        for a in sorted(r1):
            f.write(a + "\n")
    print(f"[状态] {len(r1)} 候选已存 {_STATE}")
    print("[操作] 让金币变化(买升级/撞毁/玩一局), 然后:")
    print(f"        python {sys.argv[0]} locate2 <新金币值>")


def locate2(v2):
    """第二步: 搜新值, 与第一步候选交叉收敛, 自动写测+冻结测判定"""
    import time
    gg = GG()
    if not v2:
        v2 = input("新金币值: ").strip()
    r1 = set(l.strip() for l in open(_STATE) if l.strip())
    gg.search(v2, "dword", "all")
    r2 = set(a["addr"] for a in gg.results(200, "dword").get("results", []))
    inter = r1 & r2
    print(f"[交叉] r1={len(r1)} r2={len(r2)} -> {len(inter)} 候选: {sorted(inter)}")
    if not inter:
        print("[提示] 无交集——值可能非 dword 存储, 试 float/qword 或增大变化量")
        return
    # 写测试: 逐个改 999999, 1.5s 还原, 观察 UI 是否闪变(排除显示源)
    for a in sorted(inter):
        gg.write(a, "999999", "dword")
    time.sleep(1.5)
    for a in sorted(inter):
        gg.write(a, v2, "dword")
    print("[写测] 已逐个写 999999 并还原。游戏金币有闪变→显示源(排除), 无闪变→可能是逻辑源")
    print(f"[冻结测] 逐个: python {sys.argv[0]} freeze {list(sorted(inter))[0]} 999999")
    print("         然后游戏里买最贵的车, 能买=逻辑金币(锁定)")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "freeze"
    if cmd == "freeze":
        freeze(sys.argv[2] if len(sys.argv) > 2 else "999999")
    elif cmd == "unfreeze":
        unfreeze()
    elif cmd == "check":
        check()
    elif cmd == "locate":
        locate(sys.argv[2] if len(sys.argv) > 2 else None)
    elif cmd == "locate2":
        locate2(sys.argv[2] if len(sys.argv) > 2 else None)
    else:
        print(__doc__)
