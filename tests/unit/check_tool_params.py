#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gg-mcp 工具参数一致性护栏（schema ↔ handler 参数键）

职责：交叉校验「对外声明的参数名」与「handler 实际读取的参数名」，把两类漂移
当场暴露 —— 它们都不会报错，只会静默失效：

  ERR   handler 读了 args 里 schema 没声明的键
        → AI 永远传不出这个参数（它只按 schema 给值），功能静默降级成默认值。
          典型成因：改参数名时只改了 handler，或复制别的 handler 忘了改键名。

  WARN  schema 声明了 handler 从不读的键
        → AI 传了没有任何效果，还会以为自己控制了行为（比缺参数更误导）。
          典型成因：功能下线后忘了删 schema，或 handler 重构后改用别的键。

解析对象：
  server/mcp/schema.cpp   tool("<name>", <desc>, json{...}, json{...})
                          → 每个工具第三个参数的顶层键集合
  server/session.cpp      kTools[] 表 {<name>, <handler>, <write_op>}
                          → handler 函数体里 args 参数上的 ["k"] / .contains("k")
                            / .value("k" / .at("k") 键集合
  handler 内如果调用了本文件里的 helper（如 force_guard / put_addr_param），
  把 helper 在其 json 参数上读取的键并进来 —— 一层调用闭包，够用且不会跑到
  无关函数上。

只做静态文本解析：零编译、零运行风险，git clone 后即可跑。

用法：
  python check_tool_params.py [gg_repo_root]
默认 gg_repo_root 取本脚本上一级的上级（tests/unit -> gg 根）。
退出码：0 = 无 ERR（WARN 只打印不影响退出码），1 = 有 ERR 或解析失败。
"""
import os
import re
import sys

# ---------------------------------------------------------------- 工具函数


def norm_root(r):
    """兼容 Git Bash 传入的 /d/foo/bar。必须先转盘符再 normpath。"""
    r = re.sub(r"^/([a-zA-Z])/", lambda m: m.group(1).upper() + ":/", r)
    return os.path.normpath(r)


def find_matching(text, start, open_ch, close_ch):
    """text[start] 必须是 open_ch；返回配对 close_ch 的下标（跳过字符串字面量）。"""
    depth = 0
    i = start
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '"':
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    break
                i += 1
        elif ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def split_top(text, sep=","):
    """按顶层分隔符切分（忽略字符串 / 括号 / 花括号 / 方括号内部）。"""
    parts, buf, depth, i, n = [], [], 0, 0, len(text)
    while i < n:
        ch = text[i]
        if ch == '"':
            buf.append(ch)
            i += 1
            while i < n:
                buf.append(text[i])
                if text[i] == "\\":
                    if i + 1 < n:
                        buf.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == '"':
                    break
                i += 1
            i += 1
            continue
        if ch in "{([":
            depth += 1
        elif ch in "})]":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(buf))
            buf = []
        else:
            buf.append(ch)
        i += 1
    if "".join(buf).strip():
        parts.append("".join(buf))
    return parts


def strip_comments(text):
    """去掉 // 与 /* */ 注释（保留字符串字面量内容），避免注释里的键名被当真。"""
    out, i, n = [], 0, len(text)
    while i < n:
        ch = text[i]
        if ch == '"':
            out.append(ch)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == '"':
                    break
                i += 1
            i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


# ---------------------------------------------------------------- schema 侧

TOOL_CALL_RE = re.compile(r"\btool\(\s*\"(gg_[A-Za-z0-9_]+)\"")


def props_keys(props_text):
    """props 形如 json{ {"addr", prop(...)}, {"type", prop(...)} }
    只取顶层键：扫描花括号深度，depth==1 处出现 "<ident>" 后紧跟 , 即为键。"""
    start = props_text.find("{")
    if start < 0:
        return set()
    end = find_matching(props_text, start, "{", "}")
    if end < 0:
        return set()
    body = props_text[start + 1:end]
    keys = set()
    # props 是 json{{"a", prop()}, {"b", prop()}}：每个键在自身 {".."} 块内
    # depth==1 处；depth>=2 是嵌套对象属性的子键（如 {"kv", json{{"x",..}}}），
    # 那不是工具入参顶层键，不能算。
    depth = 0
    i, n = 0, len(body)
    while i < n:
        ch = body[i]
        if ch == '"':
            # 读一个字符串字面量；若 depth==0 且后面紧跟逗号，就是顶层键
            j = i + 1
            val = []
            while j < n:
                if body[j] == "\\" and j + 1 < n:
                    val.append(body[j:j + 2])
                    j += 2
                    continue
                if body[j] == '"':
                    break
                val.append(body[j])
                j += 1
            k = j + 1
            while k < n and body[k] in " \t\r\n":
                k += 1
            if depth == 1 and k < n and body[k] == ",":
                name = "".join(val)
                if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                    keys.add(name)
            i = j + 1
            continue
        if ch in "{([":
            depth += 1
        elif ch in "})]":
            depth -= 1
        i += 1
    return keys


def extract_schema(root):
    path = os.path.join(root, "server", "mcp", "schema.cpp")
    if not os.path.exists(path):
        raise FileNotFoundError("找不到 schema.cpp: %s" % path)
    text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
    tools = {}
    for m in TOOL_CALL_RE.finditer(text):
        name = m.group(1)
        lp = text.index("(", m.start())
        rp = find_matching(text, lp, "(", ")")
        if rp < 0:
            continue
        args = split_top(text[lp + 1:rp])
        # tool(name, desc, props, required)
        tools[name] = props_keys(args[2]) if len(args) >= 3 else set()
    return tools


# ---------------------------------------------------------------- session 侧

KTABLE_RE = re.compile(r"\{\s*\"(gg_[A-Za-z0-9_]+)\"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,")
FUNC_DEF_RE = re.compile(
    r"(?:static\s+)?(?:json|void|bool|int)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*\{")

# args 上取键的四种写法
ARG_KEY_RES = (
    re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\"([A-Za-z_][A-Za-z0-9_]*)\"\s*\]"),
    re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\.\s*contains\(\s*\"([A-Za-z_][A-Za-z0-9_]*)\"\s*\)"),
    re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\.\s*(?:value|at)\(\s*\"([A-Za-z_][A-Za-z0-9_]*)\""),
)

# 动态键循环：for (const char* k : {"pc","fp"}) —— 循环变量在体内当键名用
FOR_LIST_RE = re.compile(r"for\s*\(\s*(?:const\s+)?char\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*\{([^}]*)\}")


def func_body(text, brace_pos):
    end = find_matching(text, brace_pos, "{", "}")
    return text[brace_pos + 1:end] if end > brace_pos else ""


def json_param_names(sig):
    """签名里所有 const json& 形态的参数名（这些参数承载 args）。"""
    names = set()
    for part in split_top(sig):
        if re.search(r"\bjson\s*&", part):
            m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", part.strip())
            if m:
                names.add(m.group(1))
    return names


def collect_keys(body, var_names):
    keys = set()
    for rx in ARG_KEY_RES:
        for var, key in rx.findall(body):
            if var in var_names:
                keys.add(key)
    # 动态键：for (const char* k : {"pc","fp","sp","lr"}) { ... a[k] ... }
    # 键名字面量不在 a["..."] 位置，静态正则抓不到，按循环变量名反查。
    for m in FOR_LIST_RE.finditer(body):
        var, items = m.group(1), m.group(2)
        lits = re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)"', items)
        if lits and re.search(r"\b" + re.escape(var) + r"\b", body):
            keys.update(lits)
    return keys


def extract_session(root):
    path = os.path.join(root, "server", "session.cpp")
    if not os.path.exists(path):
        raise FileNotFoundError("找不到 session.cpp: %s" % path)
    text = strip_comments(open(path, encoding="utf-8", errors="replace").read())

    table = {}
    for name, fn in KTABLE_RE.findall(text):
        table.setdefault(name, fn)

    # 函数名 -> (json 参数名集合, 直接读取的键集合, 函数体)
    funcs = {}
    for m in FUNC_DEF_RE.finditer(text):
        fname, sig = m.group(1), m.group(2)
        body = func_body(text, m.end() - 1)
        vars_ = json_param_names(sig)
        funcs[fname] = (vars_, collect_keys(body, vars_), body)

    # 一层调用闭包：handler 里出现的已知函数，把其在本文件 json 参数上读的键并进来
    def keys_of(fname, seen=None):
        if fname not in funcs:
            return set()
        seen = seen or set()
        if fname in seen:
            return set()
        seen.add(fname)
        vars_, direct, body = funcs[fname]
        keys = set(direct)
        for callee in re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", body):
            if callee in funcs and callee != fname:
                keys |= keys_of(callee, seen)
        return keys

    return table, {n: keys_of(n) for n in funcs}


# ---------------------------------------------------------------- 比对

# 已知的「schema 声明但 handler 有意不读」白名单：(工具名, 键) -> 理由。
# 键写 "*" 表示该工具全部键都放过（只用于「整包透传」这类 handler）。
# 加白名单必须写清理由 —— 它是「这条为什么不修」的唯一记录。
WARN_ALLOW = {
    ("gg_annotate_add", "*"):
        "整包委派 AnnotationStore::add()：它遍历 args 的所有键写进标注库，"
        "schema 里的字段只是给 AI 看的文档（自由格式标注），handler 不逐个读取",
}

# 已知的「handler 读但 schema 未声明」白名单：理论上不该有，留着记录历史例外。
# 注意 force：schema 已按 AGENTS 27 永久摘除 force 参数（rwproc33 is_force_read
# 在本机内核 panic），handler 仍读它**是故意的** —— 旧客户端/直连脚本传了必须
# 明确报错，静默忽略会让调用方以为强制读生效了。
ERR_ALLOW = {
    ("gg_read", "force"): "force_guard 故意拒绝（参数已永久下线，仅保留报错路径）",
    ("gg_read_bytes", "force"): "同上",
    ("gg_hex_dump", "force"): "同上",
    ("gg_view_memory", "force"): "同上",
}


def main(argv):
    root = norm_root(argv[1]) if len(argv) > 1 else norm_root(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    schema = extract_schema(root)
    table, func_keys = extract_session(root)

    if not schema:
        print("[tool-params] 解析 schema.cpp 得到 0 个工具 → 解析逻辑失效，视为失败")
        return 1
    if not table:
        print("[tool-params] 解析 session.cpp 得到 0 个工具 → 解析逻辑失效，视为失败")
        return 1

    errs, warns = [], []
    for name in sorted(schema):
        fn = table.get(name)
        if fn is None:
            errs.append("%s: schema 有声明，kTools 表中无此工具" % name)
            continue
        declared = schema[name]
        read = func_keys.get(fn, set())
        for k in sorted(read - declared):
            if (name, k) in ERR_ALLOW:
                continue
            errs.append("%s: handler(%s) 读取 \"%s\"，但 schema 未声明 → AI 传不出该参数"
                        % (name, fn, k))
        for k in sorted(declared - read):
            if (name, k) in WARN_ALLOW or (name, "*") in WARN_ALLOW:
                continue
            warns.append("%s: schema 声明 \"%s\"，handler(%s) 从不读取 → 传了无效"
                         % (name, k, fn))

    for w in warns:
        print("[warn] " + w)
    for e in errs:
        print("[err ] " + e)

    print("[tool-params] 工具 %d 个（schema %d / kTools %d），ERR %d / WARN %d"
          % (len(set(schema) & set(table)), len(schema), len(table), len(errs), len(warns)))
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
