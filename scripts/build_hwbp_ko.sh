#!/bin/bash
# build_hwbp_ko.sh — 在 WSL(Ubuntu) 里编译 hwBreakpointProc 硬件断点模块
#
# 为什么必须用完整 GKI 源码树：
#   设备内核的 struct module 是 1600 字节（0x640）—— GKI 在末尾加了
#   scmversion、无条件 sig_ok、4 个 ANDROID_KABI_RESERVE，且这些字段受
#   GKI 特有的 CONFIG 影响，不是换个 module.h 就能补上的。
#   vanilla 6.12 是 1280，单换 GKI 头到 1344，**只有完整 GKI 树 + 设备真值
#   config 才对到 1600**。尺寸不符时 insmod 直接拒：
#   ".gnu.linkonce.this_module section size must match the kernel's built
#    struct module size at run time"
#
# 模块源码的三条缺陷（见 scripts/patches/hwbp-fix.patch）：
#   1) 命令枚举从 0 起始，driver.h 按 1 起始发 → cmd=1 落到 CLOSE_PROCESS
#      → put_pid((struct pid*)1) → 内核崩
#   2) 命中回调用 mutex + cvector(vmalloc)，跑在 perf_event 溢出上下文
#      → scheduling while atomic
#   3) 执行断点命中后不把断点挪到下一条指令 → PC 原地反复触发，进程死循环
#      （第 3 条已在回调里改为直接写 BVR 实现，不走会睡眠的 perf API）
#
# 前置（WSL Ubuntu）：
#   apt install -y clang lld llvm flex bison bc libelf-dev libdw-dev \
#                  build-essential kmod python3
#   GKI 源码：
#     curl -L -o /root/gki.tar.gz \
#       https://github.com/aosp-mirror/kernel_common/archive/refs/heads/android16-6.12.tar.gz
#     tar xzf /root/gki.tar.gz -C /root
#   （android.googlesource.com 不通时这条直连可用，约 240MB / 3 分钟）
#
# 用法：
#   bash scripts/build_hwbp_ko.sh [GKI源码树] [设备config]
#   默认 GKI 树 /root/kernel_common-android16-6.12
#   默认 config  /mnt/c/Windows/Temp/ggdeck_verify/device.config
#   设备 config 取法（PowerShell）：
#     adb exec-out "su -c 'cat /proc/config.gz'" > device_config.gz 然后解压
set -u

KSRC="${1:-/root/kernel_common-android16-6.12}"
DEVCONF="${2:-/mnt/c/Windows/Temp/ggdeck_verify/device.config}"
SRC=/root/hwbp-src
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO/build/ko_built/rwNova_6.12.69.ko"

SUBLEVEL_WANT=69
LOCALVERSION_WANT="-android16-6-4k"

[ -d "$KSRC" ] || { echo "FAIL: GKI 树不存在: $KSRC"; exit 1; }
[ -f "$DEVCONF" ] || { echo "FAIL: 设备 config 不存在: $DEVCONF"; exit 1; }

# ---------- 1) 模块源码 ----------
echo "=== 1) 准备模块源码 ==="
MODDIR="$REPO/build/rwProcMem33/hwBreakpointProcModule/hwBreakpointProc_module"
[ -d "$MODDIR" ] || { echo "FAIL: 找不到模块源码 $MODDIR"; exit 1; }
rm -rf "$SRC"; mkdir -p "$SRC"
cp "$MODDIR"/*.c "$MODDIR"/*.h "$SRC"/ || { echo "FAIL: 复制源码失败"; exit 1; }
cat > "$SRC/Makefile" <<'MK'
obj-m += rwNova.o
# 模块名用 rwNova，源文件名保持原样（改名只影响加载名与产物名）
rwNova-y := hwBreakpointProc_module.o
# clang 18 比 afei 用的版本严格：cvector.h 里两个非 static 函数会触发
# -Wmissing-prototypes（内核把警告当错误），只在编译单元关掉。
ccflags-y += -Wno-missing-prototypes -Wno-unused-function
MK
echo "  源文件: $(ls "$SRC" | tr '\n' ' ')"

# ---------- 2) 配置内核树 ----------
echo "=== 2) 配置内核树（设备真值 config）==="
cd "$KSRC" || exit 1
cp "$DEVCONF" .config

# vermagic 必须与设备一致。设备 uname -r 带 git hash 与 ABO 编号
# （6.12.69-android16-6-g43b5ab7e247d-abogki561813157-4k），但内核的
# vermagic 比较只认到 -android16-6-4k —— 实测 afei 那份能正常加载的模块
# 就是这个串，说明比对该后缀之外的差异是放宽的。
# 这里用 SUBLEVEL + LOCALVERSION 直接产出真值，不做二进制改串
# （新串更长，塞不进原字符串空间）。
sed -i "s/^SUBLEVEL = .*/SUBLEVEL = $SUBLEVEL_WANT/" Makefile
sed -i 's/^EXTRAVERSION =.*/EXTRAVERSION =/' Makefile
if grep -q '^CONFIG_LOCALVERSION=' .config; then
    sed -i "s|^CONFIG_LOCALVERSION=.*|CONFIG_LOCALVERSION=\"$LOCALVERSION_WANT\"|" .config
else
    echo "CONFIG_LOCALVERSION=\"$LOCALVERSION_WANT\"" >> .config
fi

# GKI 的 CONFIG_UNUSED_KSYMS_WHITELIST 指向 AOSP 发布的 prebuilt
# abi_symbollist.raw（不在源码树里），modpost 还额外要一份
# protected_module_names_list。两者都只影响"哪些符号被导出"，
# 不改变 struct module 布局、也不影响 module_layout 的 CRC，所以关掉/留空。
sed -i 's/^CONFIG_TRIM_UNUSED_KSYMS=y/# CONFIG_TRIM_UNUSED_KSYMS is not set/' .config
sed -i 's/^CONFIG_UNUSED_KSYMS_WHITELIST=.*/# CONFIG_UNUSED_KSYMS_WHITELIST is not set/' .config
: > "$KSRC/protected_module_names_list"

grep -E "^VERSION =|^PATCHLEVEL =|^SUBLEVEL =" Makefile | head -3 | sed 's/^/  /'

make ARCH=arm64 LLVM=1 olddefconfig 2>&1 | tail -3
echo "  kernel.release: $(cat include/config/kernel.release 2>/dev/null)"

for k in CONFIG_ARM64 CONFIG_MODULES CONFIG_MODVERSIONS CONFIG_EXTENDED_MODVERSIONS CONFIG_CFI_CLANG; do
    grep -E "^$k=|^# $k is not set" .config | head -1 | sed 's/^/  /'
done

# ---------- 3) modpost 空版本表兜底 ----------
# 内核树没编过 vmlinux 就没有 Module.symvers，模块引用的内核符号全是
# undefined，于是 __version_ext_names 为空列表，上游 modpost 会生成
# `static const char x[] = ;` —— C 语法错误。补一个空串兜底。
if grep -q '_any_ext_name' scripts/mod/modpost.c; then
    echo "=== 3) modpost 空版本表补丁已在 ==="
else
    echo "=== 3) 给 modpost 打空版本表兜底 ==="
    python3 - "$KSRC" <<'PY'
import io, sys
p = sys.argv[1] + '/scripts/mod/modpost.c'
s = io.open(p, encoding='utf-8').read()
old = (
    '\tlist_for_each_entry(s, &mod->unresolved_symbols, list) {\n'
    '\t\tif (!s->module)\n'
    '\t\t\tcontinue;\n'
    '\t\tif (!s->crc_valid)\n'
    '\t\t\t/*\n'
    '\t\t\t * We already warned on this when producing the crc\n'
    '\t\t\t * table.\n'
    '\t\t\t * We need to skip its name too, as the indexes in\n'
    '\t\t\t * both tables need to align.\n'
    '\t\t\t */\n'
    '\t\t\tcontinue;\n'
    '\t\tbuf_printf(b, "\\t\\"%s\\\\0\\"\\n", s->name);\n'
    '\t}\n'
    '\tbuf_printf(b, ";\\n");\n'
)
new = (
    '\t{\n'
    '\t\tbool _any_ext_name = false;\n'
    '\t\tlist_for_each_entry(s, &mod->unresolved_symbols, list) {\n'
    '\t\t\tif (!s->module)\n'
    '\t\t\t\tcontinue;\n'
    '\t\t\tif (!s->crc_valid)\n'
    '\t\t\t\tcontinue;\n'
    '\t\t\tbuf_printf(b, "\\t\\"%s\\\\0\\"\\n", s->name);\n'
    '\t\t\t_any_ext_name = true;\n'
    '\t\t}\n'
    '\t\tif (!_any_ext_name)\n'
    '\t\t\tbuf_printf(b, "\\t\\"\\"\\n");\n'
    '\t}\n'
    '\tbuf_printf(b, ";\\n");\n'
)
if old not in s:
    print('  modpost 补丁点未匹配，跳过（若编译报 `= ;` 再手工处理）')
    sys.exit(0)
io.open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
print('  已打补丁')
PY
fi

# ---------- 4) modules_prepare ----------
echo "=== 4) modules_prepare ==="
make ARCH=arm64 LLVM=1 modules_prepare -j"$(nproc)" 2>&1 | tail -4

# ---------- 5) 编译模块 ----------
# KBUILD_MODPOST_WARN=1：没有 Module.symvers，符号必然被报 undefined。
# 这不影响加载 —— 内核按名字解析未定义符号，版本表为空时只警告不拒绝
# （已于 2026-10-21 在 6.12.69 真机实测 insmod 成功、断点可用）。
# 要拿到真正的 CRC 表则必须完整编译内核树（耗时且需精确版本匹配）。
echo "=== 5) 编译模块 ==="
make ARCH=arm64 LLVM=1 KBUILD_MODPOST_WARN=1 M="$SRC" modules -j"$(nproc)" 2>&1 \
    | grep -E "error|CC \[M\]|LD \[M\]|MODPOST" | head -8

MOD="$SRC/rwNova.ko"
[ -f "$MOD" ] || { echo "FAIL: 未产出 .ko"; exit 1; }

# ---------- 6) 校验并导出 ----------
echo "=== 6) 产物校验 ==="
mkdir -p "$REPO/build/ko_built"
cp "$MOD" "$OUT"
ls -l "$OUT" | awk '{print "  "$5" bytes  "$9}'
echo "  vermagic: $(strings "$MOD" | grep -m1 vermagic)"
echo "  this_module 段（设备真值应为 640 = 1600 字节）:"
llvm-objdump -h "$MOD" | grep -E "\.gnu\.linkonce\.this_module" | sed 's/^/    /'
echo "  版本段（为空属正常，见第 5 步说明）:"
llvm-objdump -h "$MOD" | grep -E "__versions|__version_ext" | sed 's/^/    /'

SIZE=$(llvm-objdump -h "$MOD" | awk '/\.gnu\.linkonce\.this_module/ {print $3; exit}')
if [ "$SIZE" != "00000640" ]; then
    echo ""
    echo "  警告: this_module 段为 0x$SIZE，设备要求 0x640（1600 字节）。"
    echo "        多半是 GKI 树不完整或没用设备真值 config，insmod 会被拒。"
    exit 1
fi

echo ""
echo "完成: $OUT"
echo "部署: deploy.ps1 会自动用它覆盖 build/ko/ 里的 afei prebuilt"
