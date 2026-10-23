"""ue4_esp_probe.py — 一条命令走完 UE4 实体链路（GWorld → Actor → 坐标）

链路（枪战特训2 实测，UE4.2x）：
    GWorld (libUE4.so + 0xAAB2D50)      -> UWorld*
    UWorld + 0x30                       -> ULevel*
    ULevel + 0x98                       -> TArray<AActor*> data
    ULevel + 0xA0                       -> TArray count
    AActor + 0x2D0                      -> FVector 世界坐标

这些偏移**只对枪战特训2 这个版本成立**。UE4 换游戏/换版本全都要重扫，
别直接照抄（文档里暗区突围的值 0xf429248 / 0xD18 在这里就不适用）。

偏移怎么重扫：
    GWorld  —— .dynsym 里搜 GWorld（这个游戏恰好有导出）
    UWorld->ULevel  —— 扫 UWorld 前 0x100 的指针，挑"内部含 TArray 且
                       data 首元素是指向堆的指针"的那个
    Actors  —— 在 ULevel 前 0x200 里找 (data_ptr, count) 相邻且
               count 合理、data[0] 有效的组合
    坐标    —— 遍历所有 Actor，统计哪个偏移在多个 Actor 里同时出现
               "合理的三元组"（|x| 在 1..20000），命中数最多的就是
               （实测 +0x2D0 命中 13/66，其余偏移最多 7）

用法：
    python scripts/ue4_esp_probe.py --pid 14161
"""
import argparse
import json
import struct
import urllib.request

MCP = "http://127.0.0.1:8788/mcp"

# ---- 枪战特训2 实测偏移（2026-10-23）----
OFF_GWORLD = 0xAAB2D50
OFF_UWORLD_LEVEL = 0x30
OFF_LEVEL_ACTORS = 0x98
OFF_LEVEL_COUNT = 0xA0
OFF_ACTOR_LOC = 0x2D0

HEAP_LO, HEAP_HI = 0x6C00000000, 0x7100000000


def call(name, args, timeout=120):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request(MCP, body, {"Content-Type": "application/json"})
    j = json.loads(urllib.request.urlopen(req, timeout=timeout).read())
    if j["result"].get("isError"):
        raise RuntimeError(j["result"]["content"][0]["text"])
    return j["result"]["content"][0]["text"]


def rd(addr, n):
    try:
        return bytes.fromhex(json.loads(call("gg_read_bytes", {"addr": hex(addr), "len": n})).get("hex", ""))
    except Exception:
        return b""


q = lambda b, o: struct.unpack_from("<Q", b, o)[0]
i32 = lambda b, o: struct.unpack_from("<i", b, o)[0]
is_heap = lambda v: HEAP_LO < v < HEAP_HI


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--all", action="store_true", help="列出全部 Actor（默认只列有坐标的）")
    args = ap.parse_args()

    call("gg_attach", {"pid": args.pid})
    base = int(json.loads(call("gg_module_base", {"name": "libUE4.so"}))["base"], 16)

    gw = q(rd(base + OFF_GWORLD, 8), 0)
    if not is_heap(gw):
        raise SystemExit(f"GWorld 无效 (0x{gw:X}) —— 游戏可能还没进关卡")
    level = q(rd(gw + OFF_UWORLD_LEVEL, 8), 0)
    if not is_heap(level):
        raise SystemExit(f"ULevel 无效 (0x{level:X}) —— 偏移可能变了")

    d = rd(level + OFF_LEVEL_ACTORS, 0x10)
    data, cnt = q(d, 0), i32(d, 8)
    print(f"libUE4.so   0x{base:X}")
    print(f"GWorld      0x{gw:X}")
    print(f"ULevel      0x{level:X}")
    print(f"Actors      data=0x{data:X}  count={cnt}")
    if not is_heap(data) or cnt <= 0:
        raise SystemExit("Actors 数组无效 —— 偏移可能变了")

    raw = rd(data, cnt * 8)
    actors = [q(raw, i * 8) for i in range(len(raw) // 8)]
    actors = [a for a in actors if is_heap(a)]

    rows = []
    for a in actors:
        b = rd(a + OFF_ACTOR_LOC, 12)
        if len(b) < 12:
            continue
        v = struct.unpack_from("<3f", b, 0)
        ok = all(x == x and 1.0 < abs(x) < 30000 for x in v)
        rows.append((a, v, ok))

    withloc = [r for r in rows if r[2]]
    print(f"\nActor 总数 {len(rows)}，其中 {len(withloc)} 个有有效世界坐标\n")
    print(f"{'Actor':>14}  {'X':>10} {'Y':>10} {'Z':>10}")
    print("-" * 50)
    for a, v, ok in (rows if args.all else withloc):
        mark = "" if ok else "   （坐标无效）"
        print(f"0x{a:>12X}  {v[0]:>10.1f} {v[1]:>10.1f} {v[2]:>10.1f}{mark}")


if __name__ == "__main__":
    main()
