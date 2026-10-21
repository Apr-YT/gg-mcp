#!/bin/bash
# build_hwbp_ko.sh — 在 WSL(Ubuntu) 里编译 hwBreakpointProc 模块
#
# 背景（2026-10-21）：afei 的 prebuilt 有两个致命缺陷，必须重编才能用硬件断点：
#   1) 命令码从 0 起始，agentd/driver.h 按 1 起始发送 → cmd=1 落到 CLOSE_PROCESS
#      → put_pid((struct pid*)1) → 内核崩
#   2) 命中回调用 mutex + cvector(vmalloc)，而它跑在 perf_event 溢出上下文
#      → scheduling while atomic
# 修复见 scripts/patches/hwbp-fix.patch（枚举对齐 1 起始 + raw_spinlock + 预分配缓冲）。
#
# ★ 关键前提：**必须用 GKI(android16-6.12) 的内核树**，不能用 vanilla。
#   vanilla 的 struct module 是 1280 字节，GKI 是 1344（多了 scmversion、
#   无条件的 sig_ok、4 个 ANDROID_KABI_RESERVE），加载时报
#   ".gnu.linkonce.this_module section size must match the kernel's built struct module size"。
#   本脚本没给内核树时会退化为 vanilla + GKI 头（1344），**仅够验证语法**，
#   加载仍会失败。
#
# 用法：
#   bash build_hwbp_ko.sh /path/to/gki-kernel-tree     # 推荐
#   bash build_hwbp_ko.sh                              # 退化模式（只验证能编译）
#
# 环境准备（一次性）：
#   apt install -y clang lld llvm flex bison bc libssl-dev libelf-dev
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
KSRC="${1:-}"
SRC=/root/hwbp-src
W=/mnt/c/Windows/Temp/ggdeck_verify
WORK=/root/hwbp-build

mkdir -p "$WORK"

# ---------- 1) 取模块源码 + 打补丁 ----------
echo "=== 1) 准备模块源码 ==="
rm -rf "$SRC"
mkdir -p "$SRC"
MODDIR="$REPO/build/rwProcMem33/hwBreakpointProcModule/hwBreakpointProc_module"
if [ -d "$MODDIR" ] && [ -f "$REPO/scripts/patches/hwbp-fix.patch" ]; then
    cp "$MODDIR"/*.c "$MODDIR"/*.h "$SRC"/
    # 若源码已是打过补丁的状态，patch 会失败，这里容忍
    (cd "$REPO/build/rwProcMem33" && git apply --reverse --check "$REPO/scripts/patches/hwbp-fix.patch" >/dev/null 2>&1) \
        && echo "源码已含补丁" \
        || (cd "$REPO/build/rwProcMem33" && git apply "$REPO/scripts/patches/hwbp-fix.patch" && \
            cp "$MODDIR"/*.c "$MODDIR"/*.h "$SRC"/ && echo "已应用补丁")
else
    echo "缺少模块源码或补丁，退出"; exit 1
fi

cat > "$SRC/Makefile" <<'MK'
obj-m += hwBreakpointProc_module.o
# clang 18 比 afei 用的版本严格：cvector.h 里两个非 static 函数触发
# -Wmissing-prototypes（内核把警告当错误），只在编译单元关掉。
ccflags-y += -Wno-missing-prototypes -Wno-unused-function
MK

# ---------- 2) 内核树 ----------
if [ -z "$KSRC" ]; then
    KSRC=/root/linux-6.12
    if [ ! -d "$KSRC" ]; then
        echo "=== 2) 下载 vanilla 6.12（清华镜像）==="
        (cd /root && curl -sL -o linux-6.12.tar.xz \
            https://mirrors.tuna.tsinghua.edu.cn/kernel/v6.x/linux-6.12.tar.xz && \
            tar xf linux-6.12.tar.xz)
    fi
    echo "!! 使用 vanilla 树：struct module=1280，与设备(1344)不符，编出来只能验证语法"

    echo "=== 3) 设备 config + arm64 配置 ==="
    # 设备 config 由 `adb exec-out "su -c 'cat /proc/config.gz'" | gunzip` 得到
    if [ -f "$W/device.config" ]; then
        cp "$W/device.config" "$KSRC/.config"
    else
        echo "缺少设备 config（先导出 /proc/config.gz 解压到 $W/device.config）"; exit 1
    fi
    cd "$KSRC"
    # 必须 ARCH=arm64，否则 olddefconfig 按 x86 解释设备 config 丢掉 CONFIG_ARM64
    make ARCH=arm64 CC=clang LLVM=1 olddefconfig 2>&1 | tail -2

    # vermagic 对齐设备真值：6.12.69-android16-6-4k
    #   6.12.69 ← Makefile 的 SUBLEVEL；-android16-6-4k ← CONFIG_LOCALVERSION
    sed -i 's/^SUBLEVEL = 0$/SUBLEVEL = 69/' Makefile
    sed -i 's|^CONFIG_LOCALVERSION=.*$|CONFIG_LOCALVERSION="-android16-6-4k"|' .config
    sed -i 's|^CONFIG_LOCALVERSION_AUTO=.*$|# CONFIG_LOCALVERSION_AUTO is not set|' .config
    make ARCH=arm64 CC=clang LLVM=1 syncconfig 2>&1 | tail -2
    make ARCH=arm64 CC=clang LLVM=1 modules_prepare -j"$(nproc)" 2>&1 | tail -3

    # GKI 的 module.h + android_kabi.h（让 struct module 变成 1344）
    if [ -f "$W/gki_module.h" ] && [ -f "$W/gki_android_kabi.h" ]; then
        cp "$KSRC/include/linux/module.h" "$KSRC/include/linux/module.h.vanilla" 2>/dev/null || true
        cp "$W/gki_module.h" "$KSRC/include/linux/module.h"
        cp "$W/gki_android_kabi.h" "$KSRC/include/linux/android_kabi.h"
        make ARCH=arm64 CC=clang LLVM=1 modules_prepare -j"$(nproc)" 2>&1 | tail -2
    fi
else
    echo "=== 2) 使用指定内核树: $KSRC ==="
    if [ ! -f "$KSRC/include/generated/utsrelease.h" ]; then
        echo "该树还没 prepare，先 modules_prepare ..."
        (cd "$KSRC" && make ARCH=arm64 CC=clang LLVM=1 modules_prepare -j"$(nproc)")
    fi
fi

# ---------- 4) 编译 ----------
echo
echo "=== 4) 编译模块 ==="
cd "$SRC"
make -C "$KSRC" ARCH=arm64 CC=clang LLVM=1 M="$SRC" KBUILD_MODPOST_WARN=1 modules 2>&1 \
    | grep -vE '^WARNING: modpost: "' | tail -12

echo
echo "=== 5) 产物 ==="
KO="$SRC/hwBreakpointProc_module.ko"
ls -l "$KO"
echo -n "vermagic: "; strings "$KO" | grep -m1 vermagic= | sed 's/vermagic=//'
echo -n "this_module 段: "; llvm-objdump -h "$KO" | awk '/linkonce.this_module/ {print $3; exit}'
echo
echo "设备期望的 vermagic: 6.12.69-android16-6-4k SMP preempt mod_unload modversions aarch64"
echo "（this_module 需与设备内核的 sizeof(struct module) 一致，否则 insmod 报 section size mismatch）"
