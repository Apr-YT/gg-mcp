#!/usr/bin/env python3
"""check_scalar_cmd.py — 护栏：返回标量值的命令不能走 rwproc_call_ex

【为什么需要这条】
rwproc_call_ex 把 read()/ioctl() 的返回值当成"内核回写了多少字节"：

    size_t n = (r > 0) ? (size_t)r : out_cap;
    if (n > out_cap) -> "result overflow" 并返回 -1

绝大多数命令确实如此（r = 32 位头 + payload 的长度）。但有几条命令的返回值
是**标量本身**：CMD_ANON_GET_FD 直接返回 fd 号。fd=12 而 out_cap=4 时，
上面的检查会误报 overflow，调用方看到的是一句 "rwproc result overflow: got 12,
cap 4"，完全指不到真正原因（本周排查 hwbp 匿名通道时就撞在类似现象上）。

正确用法：这类命令改用 rwproc_call()（out=NULL，返回值就是标量），
或 rwproc_call_ex(..., out=NULL, out_cap=0)。

【判据】
扫描 agentd/driver.c 的 rwproc_call_ex 调用点，看实参里是否出现
SCALAR_CMDS 中的命令。出现即失败。

【怎么扩展】
新增"返回值是标量"的命令时，把宏名加进 SCALAR_CMDS 即可 —— 护栏会自动
检查所有已有调用点，将来有人图省事用 call_ex 就会被挡住。

用法: python tests/unit/check_scalar_cmd.py <repo_root>
"""
import os
import re
import sys

# 返回值是标量（不是回写长度）的命令。
# 加新命令时同步更新 agentd/driver.c 里 rwproc_call_ex 上方的约定注释。
SCALAR_CMDS = [
    "RWPROC_CMD_ANON_GET_FD",
    "RWBP_CMD_ANON_GET_FD",
]


def find_calls(src, name):
    """返回 name( ... ) 的实参文本列表（跳过定义行）。"""
    out = []
    for m in re.finditer(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*\(", src):
        # 只跳过**函数定义**（"static ssize_t rwproc_call_ex("）。
        # 不能见 "ssize_t" 就跳：调用点常写成 "ssize_t r = rwproc_call_ex(...)"，
        # 那样会把一半调用点漏掉（第一版就是这么错的，7 个只数到 2 个）。
        line_start = src.rfind("\n", 0, m.start()) + 1
        head = src[line_start:m.start()].strip()
        if head.startswith("static") or head == "ssize_t":
            continue
        # 从 '(' 起配对到对应的 ')'
        i = m.end() - 1
        depth = 0
        end = None
        while i < len(src):
            ch = src[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    end = i
                    break
            i += 1
        if end is None:
            continue
        out.append((m.start(), src[m.start():end + 1]))
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    path = os.path.join(root, "agentd", "driver.c")
    if not os.path.exists(path):
        print("[scalar-cmd-check] SKIP: %s 不存在" % path)
        return 0

    src = open(path, encoding="utf-8", errors="replace").read()
    calls = find_calls(src, "rwproc_call_ex")
    if not calls:
        print("[scalar-cmd-check] WARN: 没找到任何 rwproc_call_ex 调用点，判据可能失效")
        return 0

    bad = []
    for pos, text in calls:
        for cmd in SCALAR_CMDS:
            if re.search(r"(?<![A-Za-z0-9_])" + cmd + r"(?![A-Za-z0-9_])", text):
                line = src.count("\n", 0, pos) + 1
                bad.append((line, cmd, " ".join(text.split())[:110]))

    if bad:
        print("[scalar-cmd-check] FAIL: 标量返回值命令不能走 rwproc_call_ex")
        print("  它把返回值当\"回写字节数\"做溢出检查，fd 号大一点就误报 overflow。")
        print("  改用 rwproc_call(...)（out=NULL，返回值就是标量）。")
        for line, cmd, snippet in bad:
            print("  - driver.c:%d  %s\n      %s" % (line, cmd, snippet))
        return 1

    print("[scalar-cmd-check] OK (%d 个 rwproc_call_ex 调用点，无一使用标量命令)"
          % len(calls))
    return 0


if __name__ == "__main__":
    sys.exit(main())
