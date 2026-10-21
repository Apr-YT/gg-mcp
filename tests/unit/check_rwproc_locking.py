#!/usr/bin/env python3
"""check_rwproc_locking.py — rwproc 共享缓冲的临界区护栏

背景（2026-10-21 审查 F-01）：
    agentd/driver.c 的 rwproc_buf 是**进程级共享**静态缓冲。三处并发使用它：
      1) 搜索 worker 线程池（search.c 多线程并行 driver_read）
      2) 冻结线程（freeze.c 独立线程调 driver_write）
      3) 主线程 handler（在 g_eng.mtx 内）
    只有 (3) 之间串行。若请求写入 / read() / 结果拷出不在同一临界区，
    线程间会互相覆盖请求与结果 —— 表现为读到别的地址的数据、句柄错位、
    搜索命中值对不上。这类错误不崩溃、不报错，只是数据是错的。

本护栏断言（不满足即失败）：
    A. rwproc_buf 的真实代码引用全部位于 rwproc_call_ex 函数体内
       （块注释里提到名字不算引用）。
    B. 这些引用都落在「唯一的 lock 行」与「最后一处 unlock 行」之间。
    C. 函数内只允许一处 lock（单入口），unlock 至少一处。
    D. rwproc_mtx 必须以 PTHREAD_MUTEX_INITIALIZER 静态初始化。

注意：护栏不能替代真机验证锁的正确性（例如某条错误路径漏了 unlock），
它只负责把「结果拷贝被挪到锁外」这类回退钉死。

用法: python check_rwproc_locking.py <gg-root>
退出码: 0=通过, 1=违反, 2=参数/IO 错误
"""
import re
import sys

FUNC = "rwproc_call_ex"
BUF = "rwproc_buf"
LOCK = "pthread_mutex_lock(&rwproc_mtx)"
UNLOCK = "pthread_mutex_unlock(&rwproc_mtx)"


def code_only(line, in_block_comment):
    """去掉 // 与 /* */ 注释，返回 (代码部分, 新的块注释状态)。"""
    out = []
    i = 0
    while i < len(line):
        if in_block_comment:
            end = line.find("*/", i)
            if end < 0:
                return "".join(out), True
            i = end + 2
            in_block_comment = False
            continue
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            in_block_comment = True
            i += 2
            continue
        out.append(line[i])
        i += 1
    return "".join(out), in_block_comment


def code_lines(lines):
    """逐行返回 (行号, 去注释后的代码, 是否整行在块注释内)。"""
    res = []
    in_block = False
    for idx, ln in enumerate(lines):
        before = in_block
        code, in_block = code_only(ln, in_block)
        res.append((idx, code, before))
    return res


def find_func(lines, name):
    """返回 (签名行, 左花括号行, 右花括号行)；找不到返回 None。"""
    sig = re.compile(r"^static\s+ssize_t\s+" + re.escape(name) + r"\s*\(")
    for i, ln in enumerate(lines):
        if not sig.match(ln):
            continue
        open_at = None
        for j in range(i, len(lines)):
            if "{" in lines[j]:
                open_at = j
                break
        if open_at is None:
            return None
        depth = 0
        for j in range(open_at, len(lines)):
            depth += lines[j].count("{") - lines[j].count("}")
            if depth <= 0:
                return i, open_at, j
        return None
    return None


def main():
    if len(sys.argv) < 2:
        print("[FAIL] usage: check_rwproc_locking.py <gg-root>", file=sys.stderr)
        return 2
    path = sys.argv[1].rstrip("\\/") + "/agentd/driver.c"
    try:
        with open(path, "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError as e:
        print(f"[FAIL] cannot read {path}: {e}", file=sys.stderr)
        return 2

    fails = []
    span = find_func(lines, FUNC)
    if not span:
        print(f"[FAIL] {FUNC} not found in driver.c", file=sys.stderr)
        return 1
    start, open_at, end = span
    codes = code_lines(lines)

    # 定位 lock / unlock 与 buf 引用（只看真实代码）
    lock_line = None
    unlock_lines = []
    buf_refs = []
    for idx, code, _ in codes:
        if LOCK in code:
            if lock_line is not None:
                fails.append(f"driver.c:{idx + 1}: multiple {LOCK} in {FUNC} (expect exactly one)")
            else:
                lock_line = idx
        if UNLOCK in code:
            unlock_lines.append(idx)
        if BUF in code:
            # 声明行不是"使用"，不参与临界区判断
            if not re.match(r"^static\s+uint8_t\s+" + BUF + r"\s*\[", code.strip()):
                buf_refs.append(idx)

    if lock_line is None:
        fails.append(f"{FUNC}: no {LOCK} — shared buffer is unprotected")
    if not unlock_lines:
        fails.append(f"{FUNC}: no {UNLOCK}")

    # A. 引用必须在函数体内（声明行本身豁免）
    for idx in buf_refs:
        if re.match(r"^static\s+uint8_t\s+" + BUF + r"\s*\[", lines[idx].strip()):
            continue
        if not (start <= idx <= end):
            fails.append(f"driver.c:{idx + 1}: {BUF} referenced outside {FUNC}: "
                         f"{lines[idx].strip()}")
    if fails:
        print("[FAIL] rwproc locking guardrail violations:")
        for f in fails:
            print("  - " + f)
        return 1

    # B. 引用必须在临界区区间内
    lo = lock_line
    hi = max(unlock_lines) if unlock_lines else -1
    for idx in buf_refs:
        if not (lo <= idx <= hi):
            fails.append(f"driver.c:{idx + 1}: {BUF} used outside the critical section "
                         f"(lock at {lo + 1}, last unlock at {hi + 1}): {lines[idx].strip()}")

    # D. 静态初始化
    if not re.search(r"static\s+pthread_mutex_t\s+rwproc_mtx\s*=\s*PTHREAD_MUTEX_INITIALIZER",
                     "\n".join(lines)):
        fails.append("rwproc_mtx must be a static pthread_mutex_t with PTHREAD_MUTEX_INITIALIZER")

    if fails:
        print("[FAIL] rwproc locking guardrail violations:")
        for f in fails:
            print("  - " + f)
        return 1
    print(f"[OK] rwproc locking guardrail passed ({len(unlock_lines)} unlock site(s), "
          f"{len(buf_refs)} {BUF} reference(s) inside {lo + 1}..{hi + 1})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
