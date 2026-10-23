#!/usr/bin/env python3
"""sync_ko_src.py — 把 KO 模块源码从工作副本同步到入库快照（单向）

工作副本 build/rwProcMem33/ 是上游克隆的落地处（有它自己的 .git、remote、
未推送提交），日常改模块就在这里改；改完并**真机验证过**之后，用本脚本把
源码同步到 ko/rwProcMem33/ 并提交。

为什么方向是 work → snap，不是反过来：
  build/ 是唯一被内核编译链路（scripts/build_rwproc_ko.sh 等）读取的位置，
  快照只是仓库里的备份。反向覆盖会静默丢掉内核侧刚做的修改。

用法: python scripts/sync_ko_src.py <gg-root> [--dry-run]
退出码: 0=成功（或无需改动）, 2=参数/IO 错误
"""
import filecmp
import os
import shutil
import sys

SRC_EXT = (".c", ".h", ".S")
SRC_NAMES = ("Makefile",)
SNAP_REL = os.path.join("ko", "rwProcMem33")
WORK_REL = os.path.join("build", "rwProcMem33")
SKIP_DIRS = ("prebuilt", "assets")


def want(name):
    return name.endswith(SRC_EXT) or name in SRC_NAMES


def collect(root):
    out = set()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and d != ".git"]
        for fn in filenames:
            if want(fn):
                out.add(os.path.relpath(os.path.join(dirpath, fn), root).replace("\\", "/"))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    if not args:
        print("[ko-sync] usage: sync_ko_src.py <gg-root> [--dry-run]", file=sys.stderr)
        return 2
    root = args[0]
    snap = os.path.join(root, SNAP_REL)
    work = os.path.join(root, WORK_REL)
    if not os.path.isdir(work):
        print("[ko-sync] 工作副本不存在: %s" % work, file=sys.stderr)
        return 2
    if not os.path.isdir(snap):
        print("[ko-sync] 快照目录不存在: %s" % snap, file=sys.stderr)
        return 2

    a, b = collect(snap), collect(work)
    added = sorted(b - a)
    removed = sorted(a - b)
    changed = []
    for p in sorted(a & b):
        if not filecmp.cmp(os.path.join(snap, p), os.path.join(work, p), shallow=False):
            changed.append(p)

    if not (added or removed or changed):
        print("[ko-sync] 已一致，无需同步 (%d files)" % len(a))
        return 0

    for p in added + changed:
        dst = os.path.join(snap, p)
        if not dry:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(os.path.join(work, p), dst)
        print("  %s %s" % ("M" if p in changed else "+", p))
    for p in removed:
        if not dry:
            os.remove(os.path.join(snap, p))
            # 顺手清掉空目录，避免快照里留下无意义的空壳
            d = os.path.dirname(os.path.join(snap, p))
            while d.startswith(snap) and d != snap:
                try:
                    os.rmdir(d)
                except OSError:
                    break
                d = os.path.dirname(d)
        print("  - %s" % p)

    print("[ko-sync] %s: +%d ~%d -%d" % ("dry-run" if dry else "synced",
                                         len(added), len(changed), len(removed)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
