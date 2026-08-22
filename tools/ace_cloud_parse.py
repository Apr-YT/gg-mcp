#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ACE 云下发检测文件分析工具（三角洲/CF 系）
============================================
解析 /data/data/<pkg>/files/ano_tmp/ 下发的 ACE 检测文件：
  - ob_x.zip -> a64.dat   （QDF 专属载荷，TLV 规则表，明文可读）
  - comm.zip -> comm.dat  （tlf 魔数 + 长度前缀表）
  - mrpcs_a*.data         （ZIP 壳 + 加密规则库）

用法:
  python ace_cloud_parse.py <文件路径> [--dump-raw] [--blocks]
示例:
  python ace_cloud_parse.py samples/delta_re/cloud_dispatch/a64.dat --blocks
  python ace_cloud_parse.py samples/delta_re/cloud_dispatch/comm.dat

输出:
  - 头部魔数/版本戳解析
  - TLV 记录表（a64.dat）：魔数/类型ID/载荷/填充/长度
  - 块模板：连续记录序列，跨偏移完全复现 = 明文结构证据
  - 字段类型枚举 + 数据值分布
"""
import re
import sys
import math
import struct
import zipfile
import collections


def entropy(data):
    if not data:
        return 0.0
    c = collections.Counter(data)
    n = len(data)
    return -sum((v / n) * math.log2(v / n) for v in c.values())


def printable_ratio(data):
    if not data:
        return 0.0
    return sum(1 for b in data if 0x20 <= b < 0x7F or b in (0x0A, 0x0D, 0x09)) / len(data)


def parse_header(data, name):
    """识别头部魔数与版本戳"""
    print(f"=== {name}: {len(data)} bytes, entropy={entropy(data):.3f} ===")
    print(f"head 32B: {data[:32].hex()}")

    # BCD 版本戳探测: 20 xx xx xx 或 20 2x 0x xx
    if len(data) >= 4 and data[0] == 0x20:
        bcd = f"{data[0]:02x}{data[1]:02x}{data[2]:02x}{data[3]:02x}"
        print(f"BCD 版本戳候选: 0x{bcd} -> 年份 {data[0]:02x}{data[1]:02x}")
    # 魔数探测
    for magic in (b"QDF", b"tlf", b"TSS", b"ACE", b"]CBP"):
        idx = data.find(magic)
        if idx >= 0 and idx < 64:
            print(f"魔数 {magic!r} @ offset {idx:#x}")


def parse_a64_tlv(data):
    """解析 a64.dat 的 TLV 记录表"""
    print("\n=== a64.dat TLV 记录表 ===")
    records = []
    pat = re.compile(b"\x85\x96\x4b\x43\xdc(.{3})\x83\x83\x83\x14\x83\x83\x83")
    for m in pat.finditer(data):
        records.append((m.start(), m.group(1)))
    print(f"记录数: {len(records)}")
    if not records:
        print("未匹配到 TLV 模式 (85 96 4b 43 dc ... 83 83 83 14 83 83 83)")
        return

    # 字段类型分组
    groups = collections.defaultdict(list)
    for off, r in records:
        groups[r[:2]].append((off, r[2]))
    print("\n字段类型枚举 (载荷前 2B):")
    for typ, items in sorted(groups.items(), key=lambda x: -len(x[1])):
        vals = [f"{v:02x}" for _, v in items]
        print(f"  {typ.hex()}: {len(items)}条, 值: {vals}")

    # 块模板: 间隔 15B 的连续记录序列
    print("\n块模板 (间隔15B连续记录):")
    i = 0
    blocks = []
    while i < len(records):
        off, _ = records[i]
        j = i + 1
        while j < len(records) and records[j][0] == records[j - 1][0] + 15:
            j += 1
        blocks.append((off, [r for _, r in records[i:j]]))
        i = j
    print(f"共 {len(blocks)} 块:")
    for off, seq in blocks:
        print(f"  [{off:#06x}] len={len(seq)}: {' '.join(r.hex() for r in seq)}")

    # 相同模板检测
    templates = collections.Counter(tuple(seq) for _, seq in blocks)
    print("\n模板重复性:")
    for tpl, n in templates.most_common(5):
        print(f"  x{n}: {' '.join(r.hex() for r in tpl)}")
    if len(templates) < len(blocks):
        print(">>> 存在跨偏移完全复现的模板 -> 明文结构（非流密码加密）")
    else:
        print(">>> 无完全复现模板 -> 可能含位置相关加密，需结合 so 解密函数")


def parse_comm(data):
    """解析 comm.dat: tlf 魔数 + 长度前缀表 + 嵌套 TLV"""
    print("\n=== comm.dat 结构 ===")
    print(f"head 64B: {data[:64].hex()}")
    if data[:4] == b"\x00\x00\x00\x03":
        print("头: [u32=3][tlf\\0] 魔数")
    # 头部索引表结构: [u32 偏移][u32 长度] 成对 (非简单 u32 数组)
    # 直接打印原始 hex 供人工分析
    print(f"0x8000 块标记出现: {data.count(b'\\x00\\x80\\x00\\x00')} 次")
    parse_comm_tlv(data)


def parse_comm_tlv(data):
    """解析 comm.dat 字符串池的嵌套 TLV（类别前缀 + 控制字节 + 加密串）"""
    print("\n--- comm.dat 字符串池 TLV 提取 ---")
    i = 0x200  # 字符串池起点
    records = []
    while i < len(data) - 8:
        flag = struct.unpack("<I", data[i:i + 4])[0]
        ln = struct.unpack("<I", data[i + 4:i + 8])[0]
        if flag == 1 and 0 < ln < 0x100 and i + 8 + ln + 1 < len(data):
            body = data[i + 8:i + 8 + ln]
            # 前缀: 连续可打印 ASCII (S_/S^_/]T_/QC_/WP_/BPK^/RX\)
            prefix = ""
            j = 0
            while j < len(body) and 0x20 <= body[j] < 0x7F:
                prefix += chr(body[j])
                j += 1
            ctrl = body[j] if j < len(body) else None
            enc = body[j + 1:] if j + 1 < len(body) else b""
            enc_str = "".join(chr(b) if 0x20 <= b < 0x7F else f"[{b:02x}]" for b in enc)
            records.append((prefix, ctrl, enc_str))
            i += 8 + ln + 1
        else:
            i += 1
    print(f"解析出 TLV 记录: {len(records)} 条")
    # 前缀类别统计
    cats = collections.Counter(r[0] for r in records)
    print("类别前缀统计:")
    for cat, n in cats.most_common():
        label = cat if cat else "(空)"
        print(f"  {label:<6s} {n}")
    # 高频加密串 (同源匹配用)
    enc_counter = collections.Counter(r[2] for r in records if len(r[2]) >= 4)
    print("高频加密串 (top10):")
    for s, n in enc_counter.most_common(10):
        print(f"  x{n}: {s}")
    return records


def parse_mrpcs(path):
    """解析 mrpcs_a*.data: ZIP 壳 + 同源串提取"""
    print(f"\n=== {path}: ZIP 壳检查 ===")
    try:
        with zipfile.ZipFile(path) as z:
            for n in z.namelist():
                info = z.getinfo(n)
                print(f"  {n}: size={info.file_size} comp={info.compress_size}")
            z.extractall(path + "_x")
            inner = open(path + "_x/" + z.namelist()[0], "rb").read()
            print(f"  解压后: {len(inner)}B, entropy={entropy(inner):.3f}")
            # 提取可打印串 (>=4) 用于同源匹配
            strs = []
            cur = b""
            for b in inner:
                if 0x20 <= b < 0x7F:
                    cur += bytes([b])
                else:
                    if len(cur) >= 4:
                        strs.append(cur.decode(errors="replace"))
                    cur = b""
            if len(cur) >= 4:
                strs.append(cur.decode(errors="replace"))
            print(f"  可打印串: {len(strs)} 个, 高频(top8):")
            for s, n in collections.Counter(strs).most_common(8):
                print(f"    x{n}: {s!r}")
            if entropy(inner) > 7.5:
                print("  >> 满熵: 强加密")
            else:
                print("  >> 低熵: 压缩/编码数据, 非强加密 (加密串在 comm.dat 同源)")
    except zipfile.BadZipFile:
        print("  非 ZIP 或损坏")


# ---------------------------------------------------------------------------
# comm.dat 已知明文 keystream 提取 + 交叉验证（2026-08-22 逆向成果）
# ---------------------------------------------------------------------------
CAT_MARKERS = [b"S_", b"S^_", b"]T_", b"QC_", b"WP_", b"BPK^"]
# 每层嵌套: [u32 len][cat 2-4B 明文][ctrl 1B][cipher N B]，密文到下一个 TLV 边界(00 00 00 0x01/02/03)结束
# 注: 实际结构为 [u32 total_len][u32 flag=1][payload]，payload 内 cat 后跟 ctrl 再跟密文

def extract_comm_entries(data):
    """提取所有 (offset, category, ctrl, cipher) 三元组"""
    entries = []
    pos = 8  # 跳过 [u32=3][tlf\0]
    n = len(data)
    while pos < n - 4:
        matched = None
        for c in CAT_MARKERS:
            if data[pos:pos + len(c)] == c:
                matched = c
                break
        if matched:
            ctrl = data[pos + len(matched)]
            start = pos + len(matched) + 1
            end = start
            while end < n - 3:
                if data[end] == 0 and data[end + 1] == 0 and data[end + 2] == 0 and data[end + 3] in (1, 2, 3):
                    break
                end += 1
            entries.append((pos, matched.decode("latin1"), ctrl, data[start:end]))
            pos = end
        else:
            pos += 1
    return entries


def keystream_of(cipher, plain):
    n = min(len(cipher), len(plain))
    return bytes(cipher[i] ^ plain[i] for i in range(n))


def analyze_keystream(data, plain_cands=None):
    """已知明文攻击: 对每个候选明文前缀假设，求 keystream 并统计同 ctrl 下的分布一致性"""
    if plain_cands is None:
        plain_cands = [b"mrpcs", b"device", b"config", b"kernel", b"screen",
                       b"model", b"wifi", b"android", b"https://", b"http://",
                       b"com.tencent", b"version", b"rule_", b"os_"]
    entries = extract_comm_entries(data)
    print(f"=== comm.dat keystream 分析: {len(entries)} 条加密串 ===")
    # 按类别 + ctrl 分布
    cat_ctrl = collections.Counter((cat, ctrl) for _, cat, ctrl, _ in entries)
    print("类别/ctrl 分布:")
    for (cat, ctrl), cnt in sorted(cat_ctrl.items()):
        print(f"  {cat}: ctrl={hex(ctrl)} count={cnt}")

    # 对每个候选明文，统计同 ctrl 下 keystream[0:8] 的分散度
    print("\n已知明文交叉验证 (key 是否只依赖 ctrl?):")
    for cand in plain_cands:
        dist = collections.defaultdict(int)
        for _, cat, ctrl, c in entries:
            if len(c) >= len(cand):
                ks = keystream_of(c[:len(cand)], cand)
                dist[ks] += 1
        if not dist:
            continue
        distinct = len(dist)
        top = dist.most_common(3)
        # 若 key 只依赖 ctrl，所有同 ctrl 条 keystream 应相同 -> distinct 应很小
        print(f"  明文={cand!r:12} 命中={sum(dist.values())} 不同keystream={distinct} "
              f"top={[(k.hex(), v) for k, v in top]}")
    return entries


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()

    name = path.split("/")[-1]
    parse_header(data, name)

    if "a64" in name or "ob_x" in name:
        parse_a64_tlv(data)
    elif name == "comm.dat":
        parse_comm(data)
        if "--keystream" in sys.argv:
            analyze_keystream(data)
    elif name.startswith("mrpcs"):
        parse_mrpcs(path)
    else:
        print("未知类型，输出基本信息")

    if "--blocks" in sys.argv and "a64" in name:
        pass  # 已在 parse_a64_tlv 输出


if __name__ == "__main__":
    main()
