"""ue4_objects.py — 枚举 UE4 的 UObject 并按 Class 聚类

不需要 GName 也能干活：同一类的对象 ClassPrivate 指针相同，
按它分组就能看出"有哪些类型、每类多少实例"。
（要类名还得解析 FName，那是下一步。）

用法：
    python scripts/ue4_objects.py --pid 31990 --base 0x6D1647C000
    python scripts/ue4_objects.py --pid 31990            # 自动取模块基址

依赖 gg MCP（127.0.0.1:8788）。
"""
import argparse
import json
import struct
import urllib.request
from collections import defaultdict

MCP = "http://127.0.0.1:8788/mcp"
GUOBJ_OFF = 0xA76B028        # base + 这个 = GUObjectArray 指针槽（今天实测）
ITEM_SIZE = 0x18             # FUObjectItem
READ_CHUNK = 0x10000         # 单次读取上限（64KB）


def call(name, args, timeout=120):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request(MCP, body, {"Content-Type": "application/json"})
    j = json.loads(urllib.request.urlopen(req, timeout=timeout).read())
    if j["result"].get("isError"):
        raise RuntimeError(j["result"]["content"][0]["text"])
    return j["result"]["content"][0]["text"]


def rd(addr, n):
    """读 n 字节，返回 bytes（失败返回空）"""
    out = b""
    got = 0
    while got < n:
        want = min(READ_CHUNK, n - got)
        try:
            j = json.loads(call("gg_read_bytes", {"addr": hex(addr + got), "len": want}))
        except Exception:
            break
        h = j.get("hex") or j.get("data") or ""
        if not h:
            break
        b = bytes.fromhex(h)
        if not b:
            break
        out += b
        got += len(b)
        if len(b) < want:      # 短读：下一页可能没驻留，跳过去
            break
    return out


def q(b, off):
    return struct.unpack_from("<Q", b, off)[0]


def i32(b, off):
    return struct.unpack_from("<i", b, off)[0]


def module_base(name):
    j = json.loads(call("gg_module_base", {"name": name}))
    return int(j["base"], 16)


def enum_objects(base):
    """返回 (对象指针列表, GUObjectArray 地址)"""
    slot = base + GUOBJ_OFF
    b = rd(slot, 8)
    if len(b) < 8:
        raise SystemExit("读 GUObjectArray 槽失败")
    guobj = q(b, 0)
    b = rd(guobj, 0x30)
    if len(b) < 0x30:
        raise SystemExit("读 FUObjectArray 失败")
    num = i32(b, 0x00)            # 实测这个字段是对象总数
    objs_ptr = q(b, 0x10)         # FUObjectItem** 块指针数组
    chunk0 = q(rd(objs_ptr, 8), 0)

    per_chunk = 65536             # UE4 默认 64K item/块
    ptrs = []
    for ci in range((num + per_chunk - 1) // per_chunk):
        cb = q(rd(objs_ptr + ci * 8, 8), 0)
        if cb < 0x1000:
            continue
        n_this = min(per_chunk, num - ci * per_chunk)
        raw = rd(cb, n_this * ITEM_SIZE)
        for i in range(len(raw) // ITEM_SIZE):
            p = q(raw, i * ITEM_SIZE)
            if 0x1000 < p < 0x8000000000:
                ptrs.append(p)
    return ptrs, guobj, chunk0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--base", default=None, help="libUE4.so 基址；不给就自动取")
    ap.add_argument("--sample", type=int, default=400, help="读多少个对象的 Class 做聚类")
    args = ap.parse_args()

    call("gg_attach", {"pid": args.pid})
    base = int(args.base, 16) if args.base else module_base("libUE4.so")
    print(f"libUE4.so base = 0x{base:X}")

    ptrs, guobj, chunk0 = enum_objects(base)
    print(f"GUObjectArray = 0x{guobj:X}   chunk[0] = 0x{chunk0:X}")
    print(f"枚举到 {len(ptrs)} 个 UObject\n")

    # 按 Class 聚类（抽样）
    step = max(1, len(ptrs) // args.sample)
    groups = defaultdict(list)
    for idx in range(0, len(ptrs), step):
        p = ptrs[idx]
        b = rd(p, 0x28)
        if len(b) < 0x28:
            continue
        cls = q(b, 0x10)          # UClass*
        flags = i32(b, 0x08)
        name_idx = i32(b, 0x18)   # FName 的 index 部分
        groups[cls].append((p, flags, name_idx))

    total_sampled = sum(len(v) for v in groups.values())
    print(f"抽样 {total_sampled} 个对象，分成 {len(groups)} 个类\n")
    print(f"{'类地址':>14}  {'本类实例':>8}  {'占抽样':>7}  示例对象(NameIdx,Flags)")
    print("-" * 78)
    for cls, items in sorted(groups.items(), key=lambda kv: -len(kv[1]))[:15]:
        p, flags, nidx = items[0]
        pct = 100.0 * len(items) / max(1, total_sampled)
        print(f"0x{cls:>12X}  {len(items):>8}  {pct:>6.1f}%  0x{p:X} (idx={nidx}, flags=0x{flags:X})")

    # 反查：这些 Class 对象自己在对象数组里吗（能拿到 Class 的 Name）
    print()
    pset = set(ptrs)
    in_arr = [c for c in groups if c in pset]
    print(f"其中 {len(in_arr)}/{len(groups)} 个 Class 指针本身也在对象数组里"
          f"（说明可以顺着 Class 拿它的 FName）")


if __name__ == "__main__":
    main()
