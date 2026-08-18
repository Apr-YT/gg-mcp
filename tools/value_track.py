#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
value_track.py — 值变化追踪器
连续读取指定地址的值, 输出时间序列, 变化时高亮标记（分析谁在什么时候改数据）
用法:
    python value_track.py <addr> [type] [interval_ms] [max_samples]
    python value_track.py libgame+0x702ED4 dword 500 120
退出: Ctrl+C
"""
import sys
import time
from ggghost import GG, _resolve, GGHostError

TYPES = ["byte", "word", "dword", "qword", "float", "double"]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    addr = sys.argv[1]
    typ = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] in TYPES else "dword"
    interval = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
    max_samples = int(sys.argv[4]) if len(sys.argv) > 4 else 0

    gg = GG()
    try:
        a = _resolve(gg, addr)
    except GGHostError:
        a = addr
    print(f"[追踪] {a} ({typ}) 每 {interval}s 采样, Ctrl+C 退出\n")

    last = None
    n = 0
    start = time.time()
    try:
        while True:
            try:
                v = gg.read(a, typ).get("value")
            except Exception as e:
                v = f"<err {e}>"
            ts = time.time() - start
            changed = (last is not None and v != last)
            mark = "  <<< 变化" if changed else ""
            print(f"  t={ts:7.2f}s  {v!s:>14}{mark}")
            last = v
            n += 1
            if max_samples and n >= max_samples:
                print(f"\n[完成] {n} 次采样")
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        print(f"\n[结束] {n} 次采样, 最后值 {last}")


if __name__ == "__main__":
    main()
