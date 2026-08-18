#!/usr/bin/env python3
"""check_kernel_only.py — 内核通信护栏（"只走内核通信，不要用户层面"）

断言 agentd/driver.c 满足约束：
  1) driver_read / driver_write 中，KO_TYPE_RWPROC / KO_TYPE_QX / KO_TYPE_DEV
     专用 KO 驱动分支内不得出现 vm_read() / vm_write() / mem_io() 调用
     （裸 process_vm_readv / /proc/pid/mem 属用户态方法，驱动失败应报错而非降级）。
  2) driver_self_test 中，vm_read() / mem_io() 调用必须位于 KO_TYPE_HOOK 分支内；
     其它专用驱动型不得被裸 process_vm_readv 假阳性掩盖为可用。

用法: python check_kernel_only.py <gg-root>
退出码: 0=通过, 1=违反, 2=参数/IO 错误
"""
import re
import sys

USER_LEVEL_CALLS = ("vm_read(", "vm_write(", "mem_io(")
DRV_TYPES = ("KO_TYPE_RWPROC", "KO_TYPE_QX", "KO_TYPE_DEV")


def find_func(lines, name):
    """返回函数体 [start, end]（含签名行与结束 '}' 行）; 找不到返回 None。"""
    sig = re.compile(r"^(?:ssize_t|int)\s+" + re.escape(name) + r"\s*\(")
    for i, ln in enumerate(lines):
        if sig.match(ln):
            depth = 0
            for j in range(i, len(lines)):
                depth += lines[j].count("{") - lines[j].count("}")
                if depth <= 0:
                    return i, j
            return None
    return None


def find_if_block(lines, start, guard_re):
    """从 start 起找 guard_re 匹配的 'if' 行，返回其块 [if_line, close_line] 或 None。"""
    for i in range(start, len(lines)):
        if re.search(guard_re, lines[i]) and "{" in lines[i]:
            depth = 0
            for j in range(i, len(lines)):
                depth += lines[j].count("{") - lines[j].count("}")
                if depth <= 0:
                    return i, j
            return None
    return None


def block_text(lines, span):
    return "\n".join(lines[span[0]:span[1] + 1])


def user_calls_in(text):
    hits = []
    for c in USER_LEVEL_CALLS:
        idx = text.find(c)
        while idx >= 0:
            hits.append(c)
            idx = text.find(c, idx + 1)
    return hits


def main():
    if len(sys.argv) < 2:
        print("[FAIL] usage: check_kernel_only.py <gg-root>", file=sys.stderr)
        return 2
    root = sys.argv[1]
    path = root.rstrip("\\/") + "/agentd/driver.c"
    try:
        with open(path, "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError as e:
        print(f"[FAIL] cannot read {path}: {e}", file=sys.stderr)
        return 2

    fails = []

    # 1) driver_read / driver_write: 专用驱动分支内不得有用户态调用
    for fn in ("driver_read", "driver_write"):
        span = find_func(lines, fn)
        if not span:
            fails.append(f"{fn}: function body not found")
            continue
        body = lines[span[0]:span[1] + 1]
        for t in DRV_TYPES:
            blk = find_if_block(body, 1, r"if\s*\(d->ko_type\s*==\s*" + t)
            if not blk:
                fails.append(f"{fn}: {t} branch not found")
                continue
            hits = user_calls_in(block_text(body, blk))
            if hits:
                fails.append(
                    f"{fn}/{t}: user-level call(s) inside dedicated KO branch: {sorted(set(hits))}")

    # 2) driver_self_test: 用户态调用只能在 KO_TYPE_HOOK 块内
    span = find_func(lines, "driver_self_test")
    if not span:
        fails.append("driver_self_test: function body not found")
    else:
        body = lines[span[0]:span[1] + 1]
        blk = find_if_block(body, 1, r"if\s*\(d->ko_type\s*==\s*KO_TYPE_HOOK")
        if not blk:
            fails.append("driver_self_test: KO_TYPE_HOOK branch not found")
        else:
            txt = block_text(body, blk)
            for c in ("vm_read(", "mem_io("):
                if c not in txt:
                    fails.append(f"driver_self_test: {c} must be inside KO_TYPE_HOOK branch")
        for t in DRV_TYPES:
            blk = find_if_block(body, 1, r"if\s*\(d->ko_type\s*==\s*" + t)
            if blk:
                hits = user_calls_in(block_text(body, blk))
                if hits:
                    fails.append(
                        f"driver_self_test/{t}: user-level call inside branch: {sorted(set(hits))}")

    if fails:
        print("[FAIL] kernel-only guardrail violations:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("[OK] driver.c kernel-only guardrail passed "
          "(no user-level fallback in dev/qx/rwproc paths)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
