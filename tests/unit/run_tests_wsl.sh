#!/bin/bash
# run_tests_wsl.sh — 在 WSL(Linux) 内跑「Windows 上无法验证」的单测
#
# 为什么需要单独一个入口：
#   test_loader.c 的 install_file 用 rename() 覆盖目标文件，POSIX 才覆盖
#   （Windows 的 rename 对已存在文件直接失败），所以「装桩/装 hook/还原」
#   这几步只能在 Linux 侧验证。Windows 的 run_tests.bat 会调本脚本补上这块。
#
# 用法（Windows 侧自动调用，也可手动）：
#   wsl -d Ubuntu -u root -- bash tests/unit/run_tests_wsl.sh
#
# 产物落在 build/wsl/（被 .gitignore 的 build/* 覆盖，不入库）。
set -e

cd "$(dirname "$0")/../.."
ROOT="$(pwd)"
OUT="$ROOT/build/wsl"
mkdir -p "$OUT"

CC="${CC:-gcc}"
CFLAGS="-O2 -Wall -Wextra"

echo "== WSL 侧单测（$($CC --version | head -1)）=="

build_and_run() {
    local name="$1"; shift
    echo "-- $name"
    # shellcheck disable=SC2086
    $CC $CFLAGS -I"$ROOT" -I"$ROOT/agentd" -I"$ROOT/shared" -I"$ROOT/third_party" \
        "$@" -o "$OUT/$name"
    "$OUT/$name"
}

build_and_run test_loader \
    tests/unit/test_loader.c agentd/loader.c agentd/version.c

echo "== WSL 侧单测全部通过 =="
