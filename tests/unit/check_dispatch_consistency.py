#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gg-mcp 分发一致性护栏（工程改动纪律核心）

职责：自动交叉校验 MCP 工具清单的两大事实源，确保「漏挂 / 错挂」立刻暴露：
  1. server/mcp/schema.cpp  的 build() —— 对外暴露的 43 工具权威清单
  2. server/session.cpp     的 call_tool() —— 实际分发逻辑（if name == "..." 链）

规则：
  - schema 声明的每个工具名，必须在 session 有对应分发分支（防「对外宣发了但调用即 unknown」）
  - session 分发的每个工具名，必须在 schema 声明（防「孤儿分支 / 拼写漂移」）

设计原则（符合「不影响其他功能」纪律）：
  - 纯静态解析，零编译、零功能风险，可直接进 CI / run_tests.bat
  - 不依赖任何构建产物，git clone 后即可跑
  - 解析宽松：容忍 `gg_a || gg_b` 合并分支、`gg_status` 等本地处理分支

用法：
  python check_dispatch_consistency.py [gg_repo_root]
默认 gg_repo_root 取本脚本上一级的上级（tests/unit -> gg 根）。
"""
import os
import re
import sys

TOOL_RE = re.compile(r'tool\(\s*"([^"]+)"')          # schema.cpp: tool("gg_xxx",
# session.cpp 分发名字提取（双模式并集）：
#  旧 if 链:  if (name == "gg_xxx")            -> NAME_RE
#  表驱动:    {"gg_xxx", tool_xxx, false},     -> TBL_RE（M1 重构后形态）
NAME_RE = re.compile(r'name\s*==\s*"([^"]+)"')        # session.cpp: if (name == "gg_xxx"
TBL_RE  = re.compile(r'\{\s*"(gg_[a-z_]+)"\s*,')      # session.cpp: {"gg_xxx", fn, ...}
OR_RE   = re.compile(r'name\s*==\s*"([^"]+)"\s*\|\|')  # session.cpp: gg_a || gg_b


def norm_root(r: str) -> str:
    """归一化 repo 根路径：兼容 Git Bash/WSL 传入的 /d/foo/bar -> D:/foo/bar。

    注意：必须在 os.path.normpath 之前做盘符转换，否则 Windows 版 normpath
    会把前导 '/' 当作「当前盘符根」误归一成 \\d\\foo。
    """
    r = re.sub(r"^/([a-zA-Z])/", lambda m: m.group(1).upper() + ":/", r)
    return os.path.normpath(r)



def extract_schema_tools(root: str) -> set:
    path = os.path.join(root, "server", "mcp", "schema.cpp")
    if not os.path.exists(path):
        raise FileNotFoundError(f"找不到 schema.cpp: {path}")
    text = open(path, encoding="utf-8", errors="replace").read()
    # 只在 build() 函数体里取，避免误抓注释里出现的 tool( 字样
    # build() 以 static const std::vector<ToolDef>& build() 起，到 return defs; 止
    m = re.search(r"build\(\)\s*\{(.*?)\n\}", text, re.S)
    body = m.group(1) if m else text
    return set(TOOL_RE.findall(body))


def extract_session_dispatch(root: str) -> set:
    path = os.path.join(root, "server", "session.cpp")
    if not os.path.exists(path):
        raise FileNotFoundError(f"找不到 session.cpp: {path}")
    text = open(path, encoding="utf-8", errors="replace").read()
    # 定位 call_tool 函数体
    m = re.search(r"call_tool\(.*?\)\s*\{(.*?)\n\}\s*\n\}", text, re.S)
    body = m.group(1) if m else text
    names = set(NAME_RE.findall(body))
    # 补全 `gg_a || gg_b` 合并分支的首个名字（NAME_RE 已抓到 gg_a，这里抓 gg_b）
    for mm in re.finditer(r'name\s*==\s*"([^"]+)"\s*\|\|\s*name\s*==\s*"([^"]+)"', body):
        names.add(mm.group(2))
    # 表驱动（M1）：kTools[] 数组在匿名 namespace（call_tool 外），单独提取数组体
    km = re.search(r"kTools\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if km:
        names |= set(TBL_RE.findall(km.group(1)))
    # 过滤掉非 gg_ 的（理论不会，保险）
    return {n for n in names if n.startswith("gg_")}


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    root = norm_root(root)
    print(f"[dispatch-check] gg root = {root}")

    try:
        schema_tools = extract_schema_tools(root)
        session_dispatch = extract_session_dispatch(root)
    except FileNotFoundError as e:
        print(f"[dispatch-check] SKIP: {e}")
        return 0  # 环境缺失不阻断（CI 中若需强制可改 exit 1）

    print(f"[dispatch-check] schema 工具数 = {len(schema_tools)}")
    print(f"[dispatch-check] session 分发数 = {len(session_dispatch)}")

    missing = sorted(schema_tools - session_dispatch)   # schema 有但 session 没分发 → 调用即 unknown
    orphan  = sorted(session_dispatch - schema_tools)   # session 分发但 schema 没声明 → 孤儿/拼写漂移

    ok = True
    if missing:
        ok = False
        print("\n[FAIL] schema 声明但未在 call_tool 分发的工具（会被当 unknown tool）：")
        for n in missing:
            print(f"    - {n}")
    if orphan:
        ok = False
        print("\n[FAIL] call_tool 分发但 schema 未声明的工具（孤儿分支/拼写漂移）：")
        for n in orphan:
            print(f"    - {n}")

    if ok:
        print(f"[dispatch-check] OK：{len(schema_tools)} 个工具 schema↔session 完全一致")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
