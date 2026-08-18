#!/usr/bin/env bash
# build_rwproc_ko.sh — 在 Linux VM 里重建 rwProcMem33 两个模块（rwProcMem + hwBreakpoint）
#
# 背景: 模块必须在与设备内核同源的 Linux 构建环境里编译（本机 Windows checkout 无内核构建产物）。
# 设备: REDMI myron, 内核 6.12.69-android16-6-4k（GKI android16-6.12, SUBLEVEL=69,
#       CONFIG_CFI_ICALL_NORMALIZE_INTEGERS=y 已开启 —— 见 docs/RWPROC33_ADAPTER.md §十）
#
# 用法:
#   KERN_SRC=/path/to/android16-6.12 \
#   DEV_KO_DIR=/path/to/device_vendor_kos \
#   bash scripts/build_rwproc_ko.sh
#
# 环境变量:
#   KERN_SRC    内核源码树（已 make gki_defconfig + 生成 vmlinux/Module.symvers + auto.conf）
#   DEV_KO_DIR  设备厂商模块目录（*.ko, 用于 patch_ko_versions.py 建符号→CRC 映射;
#               Windows 侧样例在 build/devmod/all/）
#   NDK         可选: Android NDK 根目录（默认自动探测 PATH 里的 clang）
#   OUT_DIR     可选: 输出目录（默认 build/ko）
#
# 产物:
#   $OUT_DIR/rwProcMem_module.ko  $OUT_DIR/hwBreakpointProc_module.ko（已 CRC 修补）
#
# 注意: 若在 Windows checkout 上运行, 先处理 CRLF（sed -i 's/\r$//' 本脚本与全部 .c/.h）,
#       并补齐大小写撞名文件（见 RWPROC33_ADAPTER.md §十 构建节）。
set -euo pipefail

: "${KERN_SRC:?需要 KERN_SRC=<内核源码树路径>}"
: "${DEV_KO_DIR:?需要 DEV_KO_DIR=<设备厂商模块目录>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/rwProcMem33"
HWBP_DIR="$SRC/hwBreakpointProcModule/hwBreakpointProc_module"
RWMEM_DIR="$SRC/rwProcMem33Module/rwProcMem_module"
OUT_DIR="${OUT_DIR:-$ROOT/build/ko}"
mkdir -p "$OUT_DIR"

# ---- 工具链探测 ----
if [ -n "${NDK:-}" ]; then
  CLANG="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
  export PATH="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
else
  CLANG="${CLANG:-clang}"
fi
command -v "$CLANG" >/dev/null || { echo "[ERR] 找不到 clang: $CLANG（装 NDK 或 clang-18, 或设 NDK=...）"; exit 1; }
echo "[build] clang: $CLANG"
"$CLANG" --version | head -1

# LLVM 工具（llvm-ar/llvm-nm/ld.lld）须在 PATH
for t in llvm-ar llvm-nm ld.lld; do command -v "$t" >/dev/null || { echo "[ERR] 缺 $t（LLVM 工具链不完整）"; exit 1; }; done

build_mod() { # $1=模块目录 $2=模块名
  local dir="$1" name="$2"
  echo "== 构建 $name =="
  # 防 CRLF（Windows checkout 常见）
  find "$dir" -name '*.c' -o -name '*.h' | xargs sed -i 's/\r$//' 2>/dev/null || true
  make -C "$KERN_SRC" M="$dir" ARCH=arm64 LLVM=1 CC="$CLANG" \
       KBUILD_MODPOST_WARN=1 modules -j"$(nproc)"
  cp "$dir/$name.ko" "$OUT_DIR/$name.ko"
  echo "  -> $OUT_DIR/$name.ko"
}

build_mod "$RWMEM_DIR" "rwProcMem_module"
build_mod "$HWBP_DIR"  "hwBreakpointProc_module"

echo "== CRC 修补（对齐设备 Xiaomi 内核的 MODVERSIONS）=="
python3 "$ROOT/scripts/patch_ko_versions.py" "$DEV_KO_DIR" \
       "$OUT_DIR/hwBreakpointProc_module.ko" \
       "$OUT_DIR/rwProcMem_module.ko"

echo "== 完成 =="
ls -l "$OUT_DIR"/*.ko
echo "下一步: 把两个 .ko push 到设备 /data/adb/GG-Ghost/ko/ 后执行 install_ko.sh"
