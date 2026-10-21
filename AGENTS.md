# AGENTS.md — GG-MCP 工作区指令（每个会话自动加载）

## 项目一句话

内核驱动式内存修改 MCP 服务：PC 上的 AI agent 通过 MCP 直接读写安卓游戏进程内存。
**只走 KO 内核驱动通道**（QX / rwproc33 / hook / dev 四型），GG 核心采用外挂圈子加载（ELF 桩 + hook syscall 270）。

## 项目结构

```
server/    PC 端 gg-ghost-mcp（C++17）：MCP 协议 + 会话 + wire 客户端
agentd/    设备端 gg-agentd（C, NDK arm64, root）：搜索/读写/冻结/监视 + 驱动对接
native/    内核侧：hook.c（hook 型 KO）、lib5_stub.c（GG 核心加载桩）
shared/    wire_protocol.h — PC/设备双端协议唯一事实源（20B 头 + JSON 负载）
tests/     unit（posix_stub 可 PC 跑）/ integration / regression 三层
scripts/   构建（build_pc.bat / build_ndk.bat / build_all.bat）、部署、测试、基准
configs/   gg-ghost.json（serial/端口/KO 类型）
tools/     辅助脚本（cfm_coord_read.py 等）
docs/      engineering/（工程文档）+ research/（逆向研究笔记，仅限本仓），索引见 docs/README.md
```

## 构建与验证

```powershell
# PC server（MinGW g++，-O3 -flto；本机实测 16.2.0）
scripts\build_pc.bat              # 或 scripts\build_all.bat [-Target pc|android|all]

# 设备端（需 NDK r30 + capstone 5.0.1）
scripts\build_ndk.bat

# 运行
build\pc\gg-ghost-mcp.exe --config configs\gg-ghost.json

# 测试
tests\unit\run_tests.bat          # 单元 + 两道结构护栏（先跑这个）
scripts\test_all.bat              # 单元 + 集成 + 基准（缺前置会明确 SKIP）
scripts\benchmark.bat             # ⚠️ 仓内无基准源码，会直接报错退出

# 结构守护（拆分/改分发后必跑；run_tests.bat 已自动执行）
python tests\unit\check_dispatch_consistency.py
python tests\unit\check_kernel_only.py
```

> **本机工具链现状（2026-10-21 复测）**
> - MinGW g++ **16.2.0**（`D:\Kiln\mingw64`，已入 PATH）；Python 3.13 可用于两道结构护栏。
> - NDK 为 **r30**（`D:\Kiln\android-ndk-r30`，`Pkg.Revision = 30.0.16248370`）；
>   `toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android28-clang.cmd` 存在。
> - **capstone 确实缺失**：`D:\Kiln\capstone` 整个目录不存在，而 `agentd/disasm.c` 链接需要 `libcapstone.a`
>   → **agentd / native 无法本地编译，也就无法本地验证**。PC 端、全部单元测试、结构护栏均可正常跑。
> - ⚠️ 要改 agentd（含拆 main.c）**先补 capstone**（NDK 已就位，不再是瓶颈）；否则属于盲改，改动必须过真机 itest 才能算完成。
> - `run_tests.bat` 的护栏探测已修（此前是死代码）：找不到 python 会**直接 `exit /b 1`**，
>   不再静默跳过报假绿。确需跳过时显式设 `GG_SKIP_GUARDRAILS=1`；
>   或用 `GG_PYTHON=<python.exe 全路径>` 指定解释器。

设备：Redmi myron（serial `134d2f8`，KernelSU root，内核 6.12.69，SELinux Permissive）。
adb 在 `D:\Kiln\platform-tools\adb.exe`（已入 PATH，`where adb` 可验；当前无设备连接）。
服务/窗口排查：`dumpsys window windows | grep ggmcp`；崩溃：`logcat -s AndroidRuntime:E`。

## 已知债（2026-10-15 复核，2026-10-21 校准）

1. **main.c 巨型文件**：已拆 shell 域（`cmd_shell.c`）、view/符号/指针链域（`cmd_view.c`），
   现 1846 行（2026-10-21 `wc -l` 实测）；待拆 `cmd_search` / `cmd_mem` / `cmd_proc` / dispatch。拆前后必跑 `check_dispatch_consistency.py`。
   ⚠️ 本机无 capstone，拆分无法本地编译验证 —— 见上。
2. wire 协议：1MiB 上限无分片（`GGWM_MAX_PAYLOAD`）；XOR seed 固定 `0xA5`（`shared/wire_protocol.h`）；serial 硬编码在 config。
   - 实测余量：`gg_get_results` 上限 10000 条（`agentd/main.c:968` 硬钳），每条 `addr`/`value`/`value_hex` 三个字符串，
     单条序列化 61–74B → 整帧 JSON 约 **610–740KB**，距 1MiB 剩约 29%–42%（2026-10-21 按序列化格式实测）。
     → **补分片之前不要上调 limit**，否则单帧会被 `read_frame`（`server/device/wire.cpp:286`）直接判非法。
3. ~~`docs/` 工程文档与逆向研究笔记混放，待分目录~~ —— 2026-10-21 已分为 `docs/engineering/` / `docs/research/`，
   索引见 `docs/README.md`（研究笔记仅限本仓，不外发）。
4. `archive/framework_v1/` 全量版框架（perf/atomic/error/mempool/conn_pool/io_batch）未接线，后续要么启用要么删除。
5. **capstone 5.0.1 的 aarch64 静态库缺失** → agentd / native 无法本地编译（`D:\Kiln\capstone\capstone-5.0.1\libcapstone.a`
   不存在，`scripts/build_ndk.bat` 直接 `exit /b 1`）。已由 `chore/toolchain-capstone` 分支处理中（2026-10-21）。
6. **无真机连接时 `tests/integration/run_itest.ps1` 无法执行** → 设备端改动等于没有闭环，只能等设备接上后补验，
   期间不得把设备端改动当作「已验证」上报（2026-10-21）。

### 已清（2026-10-15，本机实测）

- `run_tests.bat` 两道结构护栏曾是**死代码**（`if exist "%PYEXE%"` 对 PATH 命令名恒判假 → 静默跳过 + 报假绿，
  本地与 CI 同样中招）；已改为完整路径探测 + 校验确为 python 3 + 缺失时 `exit /b 1`。三条路径（正常/缺失/降级）均已实测。
- `build_all.bat` 汇总检查 `gg-mcp-server.exe`（实际 `gg-ghost-mcp.exe`）→ 恒报 `PC: MISSING`；
  且 `-Target` 参数被文档承诺却从未实现。已修正，并让"请求的产物缺失"导致构建失败。
- `test_all.bat` 在 `tests\unit` 下扫 `test_*.exe`（产物实际在 `build\pc\`）→ 循环体空转却打印 `[OK]`；
  且 `cd` 链相对错位。已改为按 `%ROOT%` 绝对路径、失败即退出。
- `benchmark.bat` 引用的 `tests\benchmark\memory_read.c` **全仓不存在**（无任何基准源码）→ 已改为如实报错。
- `run_itest.ps1` 的 `$Root` 少退一级（指向 `<repo>\tests\tests\...`）→ 已修正；adb 增加回退路径。
- 文档/脚本中残存的 `gg-mcp-server.exe` 全部对齐为 `gg-ghost-mcp.exe`（改名发生在提交 `b953bc5`「命名统一」）。
- **IDA Pro MCP 联动整体移除**（2026-10-15，按用户要求）：删 `server/ida_client.{h,cpp}` 与 `tests/unit/test_ida_client.cpp`；
  移除 `configs/gg-ghost.json` 的 `ida` 段、`build_pc.bat` / CI 的编译项、`main.cpp` 的 ida 配置解析、`session.{h,cpp}` 的
  `IdaConfig` / `ida_resolve`、`schema.cpp` 工具描述里的联动说明。`gg_resolve_symbol` 现为**纯本地静态解析**
  （模块 / 加载基址 / 模块内偏移 + ELF .dynsym 最近导出符号），响应**不再含 `ida` 字段**。旧实现可从 git 历史恢复。

### 已清（2026-08-28）

CI 三 job 重写（v4 + Windows 单测 + NDK 构建）；lint.bat 重写落地；双套模块收敛（simple 版留用，全量版入 archive）；
`run_tests.bat` 修复（ASCII/链接顺序/编码）并全绿；`build_ndk` 补 driver.c + 删 shell_util.c 幽灵引用；`deploy.bat` 产物对齐。

详细评审见 `docs/engineering/OPTIMIZATION_SUMMARY.md`（08-22 记录，文首有 08-28 更正声明，**该文档部分声明不实，以更正声明为准**）。

## 约定

- 提交信息：`<域>: <动词开头的一句话>`（域 = agentd / server / wire / scripts / docs / tests / chore）
- 敏感研究笔记（ACE/反作弊对抗）只放 `docs/research/`，不外发；驱动密钥、QX_KEY 等不入库不入对话
- 设备端改动必须在真机过 itest（scripts/deploy.bat 部署后跑 tests/integration/run_itest.ps1）
- 特征字符串纪律：agentd 二进制 strings 不得出现 GG-Ghost / gg-touch / GG_KO 等特征（历史提交 88d83cc、21b246d 有验证方法）
- `.bat` 一律 ASCII-only 注释：cmd 按 ANSI（GBK）解析批处理，UTF-8 中文会吞掉 `^` 续行并破坏脚本

## 历史

apk/ 悬浮窗 APK 与 ggtool/ UI Demo 线已于 2026-08-27 下线（commit bcdcf1f，可从 git 历史恢复）。
旧版工作区指令（apk/ggtool 时代）归档在 `docs/engineering/AGENTD_HISTORY.md`。

本文件最后校准：2026-10-21（工具链、main.c 行数、wire 余量均按本机实测）。
