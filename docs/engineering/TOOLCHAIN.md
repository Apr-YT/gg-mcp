# 设备端工具链（NDK r30 + Capstone 5.0.1）

本文记录 `scripts/build_ndk.bat` 所依赖的完整设备端构建链：工具链实际位置与版本、
Capstone 的获取与交叉编译步骤、批处理参数用法、常见报错处理。换一台机器可以照着走一遍。

目标产物（全部 aarch64，`android-28`）：

| 产物 | 路径 | 说明 |
|------|------|------|
| `gg-agentd` | `build/agentd/gg-agentd` | 设备端守护进程，动态 PIE 可执行文件 |
| `libKernelGg.so` | `build/native/libKernelGg.so` | syscall 270 hook 动态库 |
| `lib5_stub` | `build/native/lib5_stub` | 静态 PIE ELF 桩 |

---

## 1. 本机工具链现状

| 组件 | 路径 | 版本 |
|------|------|------|
| Android NDK | `D:/Kiln/android-ndk-r30` | r30，`Pkg.Revision = 30.0.16248370` |
| clang / clang++ | `<NDK>/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe` | Clang 21.0.0 |
| CMake | `D:/Kiln/mingw64/bin/cmake.exe` | 4.4.3 |
| Ninja | `D:/Kiln/mingw64/bin/ninja.exe` | 1.13.2 |
| Capstone | `D:/Kiln/capstone/capstone-5.0.1` | 5.0.1（源码自编，aarch64 静态库） |
| adb | `D:/Kiln/platform-tools/adb.exe` | 用于真机验证，构建本身不需要 |

> **关键差异**：NDK r30 的 `toolchains/llvm/prebuilt/windows-x86_64/bin/` 下**不再自带
> `ninja.exe` 和 `cmake.exe`**（r25 及更早版本带）。因此交叉编译 Capstone 时，
> 生成器用主机上的 Ninja（`D:/Kiln/mingw64/bin/ninja.exe`），并通过
> `CMAKE_MAKE_PROGRAM` 显式指过去；NDK 只提供 toolchain 文件与编译器。

目标三元组：`aarch64-linux-android28`（ABI `arm64-v8a`，API 28）。
对应的编译器包装脚本为 `<NDK>/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android28-clang.cmd`。

---

## 2. 获取 Capstone 5.0.1 源码

官方 tag 源码包（codeload 直连可用；国内不通时改用镜像）：

```bash
mkdir -p D:/Kiln/_download
curl -L --fail -o D:/Kiln/_download/capstone-5.0.1.tar.gz \
  "https://codeload.github.com/capstone-engine/capstone/tar.gz/refs/tags/5.0.1"
sha256sum D:/Kiln/_download/capstone-5.0.1.tar.gz
```

本次实际下载结果：

- 地址：`https://codeload.github.com/capstone-engine/capstone/tar.gz/refs/tags/5.0.1`
- 大小：7,654,195 字节
- SHA256：`2b9c66915923fdc42e0e32e2a9d7d83d3534a45bb235e163a70047951890c01a`
- 镜像：**未使用**，直连返回 200 且 4 秒内下完

镜像备选（把上面的 URL 替换为下面任意一条，其余不变）：

```text
https://gh-proxy.com/https://github.com/capstone-engine/capstone/archive/refs/tags/5.0.1.tar.gz
https://ghfast.top/https://github.com/capstone-engine/capstone/archive/refs/tags/5.0.1.tar.gz
```

>`github.com/.../archive/...` 会 302 到 `codeload.github.com`，走镜像时由镜像代为回源；
>下载后务必用 `sha256sum` 与上面的值比对（镜像返回的应是同一份 tar.gz，哈希一致）。

解压（注意：**zip 必须用系统 tar**，msys 自带 tar 不认 zip；`.tar.gz` 两者皆可，这里统一用系统 tar）：

```bash
mkdir -p D:/Kiln/_build/capstone
cd D:/Kiln/_build/capstone
C:/Windows/System32/tar.exe -xzf D:/Kiln/_download/capstone-5.0.1.tar.gz
# 得到 D:/Kiln/_build/capstone/capstone-5.0.1
```

---

## 3. 交叉编译 arm64 静态库

### 3.1 先确认需要哪些架构

`agentd/disasm.c` 只用到一个架构：

```c
cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle);
```

即 **仅 AArch64**（`CS_ARCH_ARM64` 是 AArch64 的别名）。没有用到 ARM32/MIPS/x86 等，
因此只开 AArch64 支持，编译量与库体积都最小。

> **坑**：Capstone 5.0.1 的 CMake 选项名是 `CAPSTONE_ARM64_SUPPORT`，**不是**
> `CAPSTONE_AARCH64_SUPPORT`。后者不会报错——CMake 只提示 unused variable 并忽略，
> 配合 `CAPSTONE_ARCHITECTURE_DEFAULT=OFF` 会生成一个**不含任何架构**的
> `libcapstone.a`：链接照样通过（`CS_ARCH_ARM64` 只是枚举值），但运行时 `cs_open()`
> 返回 `CS_ERR_ARCH`，反汇编功能静默失效。

### 3.2 配置

```bash
NDK=D:/Kiln/android-ndk-r30
SRC=D:/Kiln/_build/capstone/capstone-5.0.1
BUILD=D:/Kiln/_build/capstone/build-arm64

D:/Kiln/mingw64/bin/cmake.exe -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="D:/Kiln/mingw64/bin/ninja.exe" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DCAPSTONE_BUILD_TESTS=OFF \
  -DCAPSTONE_BUILD_CSTOOL=OFF \
  -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF \
  -DCAPSTONE_ARM64_SUPPORT=ON \
  -DCMAKE_INSTALL_PREFIX="D:/Kiln/capstone/capstone-5.0.1"
```

配置成功的标志之一是输出里出现：

```text
-- The C compiler identification is Clang 21.0.0
Enabling CAPSTONE_ARM64_SUPPORT
-- Configuring done
```

**只看到 `Enabling CAPSTONE_ARM64_SUPPORT` 这一行架构才说明生效**；如果一行都没有，
说明架构选项名写错了。

### 3.3 编译 + 安装

```bash
D:/Kiln/mingw64/bin/cmake.exe --build "$BUILD"          # 13 个编译单元，约 2 秒
D:/Kiln/mingw64/bin/cmake.exe --install "$BUILD"
```

本机实测：配置 1.0 s，编译 1.6 s，产出 `libcapstone.a` 约 4.6 MB。

### 3.4 目录布局（脚本期望的形状）

`cmake --install` 会把静态库放到 `lib/` 子目录，而 `build_ndk.bat` 检查的是
`%CAPSTONE%\libcapstone.a`（安装根目录下）。所以安装后需要再复制一份到根：

```bash
cp D:/Kiln/capstone/capstone-5.0.1/lib/libcapstone.a \
   D:/Kiln/capstone/capstone-5.0.1/libcapstone.a
```

最终结构：

```text
D:/Kiln/capstone/capstone-5.0.1/
  include/capstone/capstone.h      # 头文件（--include 指向 include/）
  libcapstone.a                    # 构建脚本检查的位置
  lib/libcapstone.a                # cmake --install 的原始输出（保留亦可）
  lib/cmake/capstone/...           # 可选，供 CMake 项目 find_package 用
```

验证静态库确实是 aarch64（不是主机版冒充）：

```bash
BIN=D:/Kiln/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin
"$BIN/llvm-readelf.exe" -h D:/Kiln/capstone/capstone-5.0.1/libcapstone.a | grep -E "Class:|Machine:"
#   Class:   ELF64
#   Machine: AArch64
"$BIN/llvm-nm.exe" --defined-only D:/Kiln/capstone/capstone-5.0.1/libcapstone.a | grep AArch64_get_insn_id
#   0000000000000000 T AArch64_get_insn_id
```

---

## 4. `scripts/build_ndk.bat` 用法

```text
usage: build_ndk.bat [NDK dir] [Capstone dir]
  NDK default:      D:\Kiln\android-ndk-r30
  Capstone default: D:\Kiln\capstone\capstone-5.0.1
```

两个参数都可省略；省略时使用上面的默认路径。换机器或用多版本 NDK 时按位置传参：

```bat
scripts\build_ndk.bat D:\Kiln\android-ndk-r30 D:\Kiln\capstone\capstone-5.0.1
```

从 worktree 根目录直接跑（cmd 下）：

```bat
scripts\build_ndk.bat
```

Git Bash 下等价写法（`//c` 防止 msys 把 `/c` 当路径转换）：

```bash
cmd //c "scripts\build_ndk.bat"
```

### 4.1 脚本做了什么

1. 用 `aarch64-linux-android28-clang.cmd` 把 `agentd/*.c` + `third_party/cJSON.c` 编成
   `build/agentd/gg-agentd`（`-O3 -flto`，链接 `libcapstone.a`，`-pie`）。
2. `native/hook/hook.c` → `build/native/libKernelGg.so`（`-shared -fPIC`）。
3. `native/stub/lib5_stub.c` → `build/native/lib5_stub`（`-static-pie`）。

任一阶段失败会打印 `[FAIL]` 并以非 0 退出，不会留半成品成功提示。

### 4.2 验收

```bash
BIN=D:/Kiln/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin
"$BIN/llvm-readelf.exe" -h build/agentd/gg-agentd | grep -E "Class:|Type:|Machine:"
#   Class:   ELF64
#   Type:    DYN (Shared object file)     <- PIE 可执行文件
#   Machine: AArch64
"$BIN/llvm-readelf.exe" -l build/agentd/gg-agentd | grep -A1 INTERP
#   [Requesting program interpreter: /system/bin/linker64]
"$BIN/llvm-readelf.exe" -d build/agentd/gg-agentd | grep FLAGS_1
#   0x... (FLAGS_1)  NOW PIE
```

真机部署另需 `adb devices` 看到设备；无设备时只能验证「编译产得出」。

---

## 5. 常见报错与处理

### `[FAIL] D:\Kiln\capstone\capstone-5.0.1\libcapstone.a not found.`

库没编或只装到了 `lib/` 子目录。按 §3.3–3.4 编译并复制到安装根目录。

### 配置阶段没有任何 `Enabling CAPSTONE_xxx_SUPPORT` 输出

架构选项名写错（例如用了不存在的 `CAPSTONE_AARCH64_SUPPORT`）。改回
`-DCAPSTONE_ARM64_SUPPORT=ON`，删除 `CMakeCache.txt` 后重新配置。

### 编译通过、运行时 `disasm` 返回空

同上：库里没有 AArch64 后端。用 §3.4 的 `llvm-nm | grep AArch64_get_insn_id` 确认。

### `[FAIL] NDK clang not found at ...`

NDK 路径不对/版本不同。检查 `%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android28-clang.cmd`
是否存在；r27d 之类旧默认路径在本机并不存在，传入实际路径即可。

### 批处理里的 `^` 续行莫名失效 / 只编了第一个源文件

`build_ndk.bat` 里有 `^` 续行。**cmd 按 GBK 解析批处理，UTF-8 中文注释会让续行符被吞掉**，
后续行被当成独立命令。因此 `.bat` 内部只允许 ASCII；注释一律写 `rem` 英文。
编辑后自查：

```bash
LC_ALL=C grep -nP "[^\x00-\x7F]" scripts/build_ndk.bat   # 应为空
```

### `cmake: command not found` / `ninja: command not found`

NDK r30 不自带 cmake/ninja。把 `D:/Kiln/mingw64/bin` 加入 PATH，或像 §3.2 那样写全路径。

### Git Bash 里 `-DCMAKE_TOOLCHAIN_FILE=/d/...` 之类路径失效

msys 会把 `/tmp/...`、`/d/...` 改写成本地路径。统一用 `D:/...` 正斜杠形式，
批处理内部用 `D:\...` 反斜杠。

### `file: command not found`

Git Bash 未提供 `file`。用 NDK 的 `llvm-readelf.exe` 看 ELF 头，或直接看扩展名与
`readelf -h` 输出（见 §4.2）。

---

## 6. 换机器清单

1. 安装 NDK r30 到任意目录，记下路径（下文 `<NDK>`）。
2. 安装 CMake ≥ 3.15 与 Ninja，加入 PATH。
3. 按 §2 下载 capstone 5.0.1 并校验 SHA256。
4. 按 §3.2–3.3 交叉编译，按 §3.4 摆放目录。
5. 跑 `scripts\build_ndk.bat <NDK 目录>`（Capstone 在非默认位置时传第二个参数）。
6. 按 §4.2 校验三个产物的 ELF 头。
