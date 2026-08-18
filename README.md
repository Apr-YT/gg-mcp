# GG-MCP · GameGuardian MCP Server（C++）

把 GameGuardian（GG修改器）的核心能力搬进 MCP（Model Context Protocol），让 **PC 上的 AI agent 直接通过 MCP 工具**搜索/修改安卓游戏内存。内存读写**只通过 KO 内核驱动通信**（外挂圈子方式：`insmod` 加载，不依赖 KPM/KPatch-Next），GG 核心采用**外挂圈子加载内核的方式**（ELF 桩 + LD_PRELOAD hook syscall 270 → KO 驱动），并支持**版本族容错**：同类型如 `6.12.xx`，补丁号不同均可加载。

## 架构总览

```
AI agent(PC) ──MCP(stdin / HTTP)──▶ gg-mcp-server (C++17, PC)
                                       │ 自定义二进制协议 (shared/wire_protocol.h)
                                       │ adb forward tcp:19100
                                       ▼
                          gg-agentd (C, 设备端 NDK arm64, root)
                              ├─ KO 加载: insmod + setenforce 0（版本族匹配 6.12.*）
                              ├─ hook 型 KO: 内核 hook process_vm_readv（零协议）
                              ├─ dev 型 KO:  /dev 节点 + ioctl 0x7e1a读/0x7e1b写
                              ├─ 降级链: KO → process_vm_readv → /proc/pid/mem
                              └─ GG 核心对接: lib5_stub(ELF桩) + libKernelGg.so(hook syscall 270)
```

## KO 驱动（只加载 KO，不要 KPM）

- **QX 型（推荐，你已对接）**：Aloha 开源 QX9-10（`qx-对接.h` 协议）。
  `deploy.ps1` 部署后 agentd 自动**发现隐藏驱动进程 → 重建 /dev 节点 → open → init_key**，
  无需 insmod、无需 .ko 文件。密钥在 `scripts/start_agentd.sh` 的 `QX_KEY` 里配置。
- **rwproc33 型（开源替代，无密钥门控）**：abcz316/rwProcMem33（`KO_TYPE=rwproc` +
  `RWPROC_MEM_KEY`/`RWPROC_BP_KEY`），proc read 33 字节头协议，内存读写 + perf_event
  硬件断点（命中带完整 x0-x30/sp/pc/pstate）。适配见 `docs/RWPROC33_ADAPTER.md`。
- **hook 型 KO**（内核 hook `process_vm_readv`）：`KO_TYPE=hook`，agentd 直接走
  `process_vm_readv`，自检显示 `state=ko-hook`
- **dev 型 KO**（/dev 节点 + ioctl 0x7e1a/0x7e1b 通用布局）：`KO_TYPE=dev` + `DEV_PATH`
- **版本族容错作用于 KO 选择**（hook/dev 型）：`ko/` 下多个同族 KO 时按 `6.12.*` 模式
  选第一个族匹配的加载，补丁号任意均可（qx/rwproc 型为自动发现/配置，不涉及）

## 快速开始

### 0. 构建

```bat
rem PC 端（需 MinGW g++）
scripts\build_pc.bat

rem 设备端（需 Android NDK，默认 D:\Kiln\android-ndk-r27d-windows；
rem 需 Capstone 5.x，默认 D:\Kiln\capstone\capstone-5.0.1，含交叉编译出的 libcapstone.a：
rem   cd /d D:\Kiln\capstone\capstone-5.0.1 && mingw32-make all CAPSTONE_STATIC=yes CAPSTONE_SHARED=no
rem     CAPSTONE_BUILD_CORE_ONLY=yes CAPSTONE_ARCHS="arm aarch64"
rem     CC=D:\Kiln\android-ndk-r27d-windows\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android28-clang.cmd
rem     AR=...\llvm-ar.exe RANLIB=...\llvm-ranlib.exe BUILDDIR=build-gg
rem   然后 copy build-gg\libcapstone.a 到 Capstone 根目录)
scripts\build_ndk.bat
```

产物：`build\pc\gg-mcp-server.exe`、`build\agentd\gg-agentd`、`build\native\libKernelGg.so`、`build\native\lib5_stub`。

### 1. 设备端准备（一次性）

- Root（Magisk 或 KernelSU 均可，**无需** KPatch-Next）
- KO 驱动放入 `build\ko\`（本仓库不内置第三方驱动）
- 部署：`powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1`（自动 push + insmod + 启动 + adb forward）

### 2. 启动 PC 端 MCP 服务器

```bat
build\pc\gg-mcp-server.exe --config configs\ggmcp.json --transport http --port 8788
rem 或 stdio（供 Claude Desktop / Claude Code）：
build\pc\gg-mcp-server.exe --config configs\ggmcp.json --transport stdio
```

MCP 客户端配置见 `scripts\mcp_config.example.json`（stdio 与 http 两种）。

### 3. AI 使用示例

```
gg_kernel_status              → 验证 KO 通道（hook 型: state=ko-hook / dev 型: state=kernel-driver）
gg_list_processes             → 找到目标游戏进程
gg_attach {pid:1234}          → 附加
gg_search {value:"100", type:"dword", region:"anonymous"}
gg_get_results {limit:20}     → 查看命中地址
gg_set_results {values:[{"index":0,"value":"99999"}], type:"dword"}
gg_view_memory {addr:"0x...", range:8, element_type:"int32"} → 结构化查看（AI 理解结构体）
gg_freeze {addr:"0x...", value:"99999", type:"dword"}   → 冻结
gg_kernel_load_gg {pattern:"101.*", strict:true}       → 加载 GG 核心（版本族校验）
```

## 工具集（43 个）

进程：`gg_list_processes` `gg_attach` `gg_detach` `gg_target_info` `gg_module_list`
搜索：`gg_search` `gg_search_unknown_init` `gg_search_refine` `gg_search_range`
      `gg_result_count` `gg_get_results` `gg_clear_results`
读写：`gg_read` `gg_read_bytes` `gg_hex_dump` `gg_view_memory` `gg_write` `gg_set_results` `gg_freeze` `gg_unfreeze`
分析：`gg_module_base` `gg_calc_offset` `gg_pointer_scan` `gg_pointer_chain` `gg_resolve_symbol` `gg_pause` `gg_resume`
内核：`gg_kernel_status` `gg_kernel_load_gg` `gg_core_versions`
监视/反汇编：`gg_watch_set` `gg_watch_poll` `gg_watch_inject` `gg_watch_clear` `gg_disassemble`
标注：`gg_annotate_add` `gg_annotate_list` `gg_annotate_get` `gg_annotate_remove` `gg_annotate_context`
系统：`gg_status` `gg_shell` `gg_device_info`

## 版本族容错（核心需求）

> 只要是同类型的如 `6.12.xx`，后面 xx 不同都可以加载。

两处生效（模式可配置，默认按实际包调整）：
1. **GG 核心**：加载器从 GG 包取 versionName，与模式族匹配（major.minor 相同即同族）。
   本机分析你提供的 `宇航员修改器.zip`：**实际为 GameGuardian 101.1**（versionName `101.1`，
   伪装包名 `com.tencent.gamereva`，核心为 arm64 ELF 可执行文件）→ 默认模式 **`101.*`**。
2. **KO 驱动选择**：`ko/` 目录下 `.ko` 文件名中的版本号（如 `driverX_6.12.9.ko`）同样按模式族匹配选取（示例默认 `6.12.*`，按你的驱动实际改）。

详见 `docs/VERSION_MATCHING.md`。

## 目录

```
server/        PC 端 C++ MCP 服务器（mcp/ 协议层, tools/ 会话, device/ adb+wire）
agentd/        设备端 C 守护（driver(KO)/process/search/freeze/loader/version）
native/        GG 核心对接（hook.c syscall 270 拦截, lib5_stub.c ELF 桩）
shared/        wire_protocol.h（PC↔设备协议唯一事实源）
scripts/       构建/部署/KO 安装/MCP 配置样例
configs/       服务器配置
docs/          架构/协议/版本匹配文档
tests/         单测与集成测试
third_party/   json.hpp / httplib.h / cJSON（单头文件依赖）
```

## 文档

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — 架构与数据流
- [docs/PROTOCOL.md](docs/PROTOCOL.md) — KO 驱动协议与 wire 协议笔记
- [docs/VERSION_MATCHING.md](docs/VERSION_MATCHING.md) — 版本族匹配规则（GG 核心 + KO）

## 合规

仅供技术研究与学习。请勿用于联网游戏作弊（可能违反服务条款并被封号）或任何非法用途；使用者自行承担后果。`configs/ggmcp.json` 中 `safe_mode: true` 可启用只读会话（拒绝 write/freeze/shell）。
