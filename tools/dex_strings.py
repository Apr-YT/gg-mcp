#!/usr/bin/env python3
"""dex_strings.py — 从 DEX 文件提取字符串池, 过滤 API/URL 相关

用法: python dex_strings.py <apk或dex> [--pattern 正则] [--out 输出]
默认 pattern: https?://|/api/|/v\d+/|yuanfudao|fbcontent
"""
import zipfile, struct, sys, re, argparse

def uleb128(data, off):
    result = 0; shift = 0
    while True:
        b = data[off]; off += 1
        result |= (b & 0x7f) << shift
        if not (b & 0x80): break
        shift += 7
    return result, off

def dex_strings(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'dex\n035\x00' and data[:8] != b'dex\n037\x00' and data[:8] != b'dex\n038\x00':
        # 可能是 039
        if data[:4] != b'dex\n':
            return []
    string_ids_size = struct.unpack_from('<I', data, 0x38)[0]
    string_ids_off = struct.unpack_from('<I', data, 0x3C)[0]
    out = []
    for i in range(string_ids_size):
        off = string_ids_off + i * 4
        sdata_off = struct.unpack_from('<I', data, off)[0]
        _, p = uleb128(data, sdata_off)  # utf16_size
        start = p
        while data[p] != 0:
            p += 1
        try:
            s = data[start:p].decode('utf-8', 'replace')
        except Exception:
            continue
        out.append(s)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input')
    ap.add_argument('--pattern', default=r'https?://|/api/|/v\d+/|yuanfudao|fbcontent|aliyuncs|\.yfd|/leo|/solar|/odin|/frog|/planet')
    ap.add_argument('--out', default='')
    args = ap.parse_args()
    rx = re.compile(args.pattern, re.I)
    hits = []
    files = []
    if args.input.endswith('.apk') or args.input.endswith('.zip'):
        z = zipfile.ZipFile(args.input)
        files = [n for n in z.namelist() if n.endswith('.dex')]
        for n in files:
            d = z.read(n)
            import tempfile, os
            tf = tempfile.NamedTemporaryFile(delete=False, suffix='.dex')
            tf.write(d); tf.close()
            ss = dex_strings(tf.name)
            os.unlink(tf.name)
            for s in ss:
                if rx.search(s):
                    hits.append((n, s))
    else:
        ss = dex_strings(args.input)
        for s in ss:
            if rx.search(s):
                hits.append((args.input, s))
    # 去重
    seen = set(); uniq = []
    for n, s in hits:
        if s not in seen:
            seen.add(s); uniq.append((n, s))
    print(f'[+] {len(uniq)} 条匹配字符串', flush=True)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            for n, s in uniq:
                f.write(f'[{n}] {s}\n')
        print(f'[+] 保存 -> {args.out}', flush=True)
    for n, s in uniq[:50]:
        print(f'  [{n}] {s}')

if __name__ == '__main__':
    main()
