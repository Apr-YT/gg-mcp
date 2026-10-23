#!/usr/bin/env python3
"""check_ko_src_sync.py — KO 模块源码快照与工作副本的一致性护栏

背景（2026-10-23）：整套系统的地基是两个内核模块（rwNova33 内存通道 /
rwNova 断点），但它们的源码长期只存在于 build/rwProcMem33/ —— 那是上游
rwProcMem33-afei 的 git 克隆，被 .gitignore 整体排除。结果是：仓库里的
.gitignore 只跟踪了一个产物 .ko，源码（含四条真机修复）在任何一台新机器上
都无法重建。

为什么不做成 submodule：那个克隆有 4 个未推送提交 + 若干未提交修改，
submodule 只记 commit 指针，克隆方拉不到这些内容，等于没入库。

现在的做法：源码以**快照**形式提交在 ko/rwProcMem33/，工作副本仍留在
build/rwProcMem33/（保留上游 history 与 remote）。本脚本负责发现两边漂移 ——
改了工作副本却忘了同步快照，是这套方案的唯一失效模式。

用法: python check_ko_src_sync.py <gg-root>
退出码: 0=一致（或工作副本不存在，按通过处理）, 1=漂移, 2=参数错误

同步方法（在工作副本改动**已经验证过**之后执行）:
    python scripts/sync_ko_src.py <gg-root>
"""
import hashlib
import os
import sys

# 只比源码：编译产物、第三方工具包、图片不在快照范围内。
SRC_EXT = (".c", ".h", ".S")
SRC_NAMES = ("Makefile",)

SNAP_REL = os.path.join("ko", "rwProcMem33")
WORK_REL = os.path.join("build", "rwProcMem33")
SKIP_DIRS = ("prebuilt", "assets")


def want(name):
    return name.endswith(SRC_EXT) or name in SRC_NAMES


def collect(root):
    """返回 {相对路径: sha256}，只含源码类文件。"""
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and d != ".git"]
        for fn in filenames:
            if not want(fn):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace("\\", "/")
            h = hashlib.sha256()
            with open(full, "rb") as f:
                for chunk in iter(lambda: f.read(65536), b""):
                    h.update(chunk)
            out[rel] = h.hexdigest()
    return out


def main():
    if len(sys.argv) < 2:
        print("[ko-sync] usage: check_ko_src_sync.py <gg-root>", file=sys.stderr)
        return 2
    root = sys.argv[1]
    snap = os.path.join(root, SNAP_REL)
    work = os.path.join(root, WORK_REL)

    if not os.path.isdir(snap):
        print("[ko-sync] FAIL: 快照目录不存在: %s" % snap)
        print("         ko/ 是 KO 源码的唯一入库副本，不能缺。")
        return 1
    if not os.path.isdir(work):
        # 新克隆的机器上不会有工作副本：快照本身就是真相，按通过处理。
        print("[ko-sync] OK (no working copy at %s; snapshot is the only source)" % WORK_REL)
        return 0

    a = collect(snap)
    b = collect(work)

    only_snap = sorted(set(a) - set(b))
    only_work = sorted(set(b) - set(a))
    differ = sorted(p for p in (set(a) & set(b)) if a[p] != b[p])

    if not (only_snap or only_work or differ):
        print("[ko-sync] OK (%d source files identical)" % len(a))
        return 0

    print("[ko-sync] FAIL: 工作副本与快照不一致（%d 个文件）" % (len(only_snap) + len(only_work) + len(differ)))
    if only_work:
        print("  只在工作副本（build/）里 —— 改了但没同步到 ko/：")
        for p in only_work[:10]:
            print("    + %s" % p)
    if only_snap:
        print("  只在快照（ko/）里 —— 工作副本被删或被回退：")
        for p in only_snap[:10]:
            print("    - %s" % p)
    if differ:
        print("  内容不同：")
        for p in differ[:10]:
            print("    ~ %s" % p)
    print("\n  如果你确认 build/ 里的是要保留的版本，执行：")
    print("    python scripts/sync_ko_src.py %s" % root)
    print("  如果是 build/ 被弄脏了，反向同步（用快照覆盖工作副本）请手动 cp，")
    print("  并注意那会覆盖掉内核侧的本地修改 —— 先 git -C build/rwProcMem33 status 看清楚。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
