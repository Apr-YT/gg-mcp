"""修补 modpost 在 __version_ext_names 为空时生成的非法初始化。

现象：rwProcMem_module.mod.c 里出现

    static const char ____version_ext_names[]
    __used __section("__version_ext_names") =
    ;

`= ;` 不是合法的 C 初始化，clang 报 "expected expression"。
上游 modpost 在版本表为空（没有 Module.symvers）时会生成这种代码，
RWPROC33_ADAPTER.md §10.4.1 也记过这个缺陷。

用法：python fix_modc.py <xxx.mod.c>
"""

import sys

BAD = '__version_ext_names") =\n;'
GOOD = '__version_ext_names") = "";'


def main() -> int:
    if len(sys.argv) != 2:
        print("用法: python fix_modc.py <xxx.mod.c>")
        return 2
    path = sys.argv[1]
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
        src = f.read()
    if BAD not in src:
        print("未找到待修补模式（可能不是那个缺陷，或已被修补）")
        return 1
    src = src.replace(BAD, GOOD)
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(src)
    print("已修补:", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
