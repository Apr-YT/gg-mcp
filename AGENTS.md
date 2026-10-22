# AGENTS.md — GG-MCP 工作区指令（每个会话自动加载）

## 项目一句话

内核驱动式内存修改 MCP 服务：PC 上的 AI agent 通过 MCP 直接读写安卓游戏进程内存。
**只走 KO 内核驱动通道**（QX / rwproc33 / hook / dev 四型），GG 核心采用外挂圈子加载（ELF 桩 + hook syscall 270）。

## 项目结构

```
server/    PC 端 gg-ghost-mcp（C++17）：MCP 协议 + 会话 + wire 客户端
agentd/    设备端 gg-agentd（C, NDK arm64, root）：搜索/读写/冻结/监视 + 驱动对接
shared/    wire_protocol.h — PC/设备双端协议唯一事实源（20B 头 + JSON 负载；含 GGCMD_AUTH 握手）
tests/     unit（posix_stub 可 PC 跑）/ integration / regression 三层
scripts/   构建（build_pc.bat / build_ndk.bat / build_all.bat）、部署、测试、基准
configs/   gg-ghost.example.json（模板，入库）；gg-ghost.json 由 deploy 生成、**不入库**
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
> - **capstone 5.0.1 已就位**：`D:\Kiln\capstone\capstone-5.0.1\libcapstone.a`（17:44 装入）。
> - **agentd 现已可本地完整编译**。实测 `scripts\build_ndk.bat` 全绿，产出
>   `build\agentd\gg-agentd`（2.4MB，ELF64 **AArch64 PIE**，仅 NEEDED `libc.so`/`libdl.so`，`-Wall` 零警告）。
> - ⚠️ **编译通过 ≠ 功能可用**：设备端改动仍必须过真机 itest（`deploy.bat` → `run_itest.ps1`）才算完成。
> - `run_tests.bat` 的护栏探测已修（此前是死代码）：找不到 python 会**直接 `exit /b 1`**，
>   不再静默跳过报假绿。确需跳过时显式设 `GG_SKIP_GUARDRAILS=1`；
>   或用 `GG_PYTHON=<python.exe 全路径>` 指定解释器。
> - **WSL2 已装（2026-10-21）**：`Ubuntu 24.04.5`（WSL 3.0.3 / 内核 6.18.54.1），
>   磁盘在 `D:\WSL\Ubuntu`，swap 在 `D:\WSL\swap.vhdx`，apt 走清华源，gcc 13.3 + make 4.3。
>   用途：跑 Windows 上无法验证的 POSIX 行为（如 `install_file` 的 rename 覆盖语义）。
>   ⚠️ **原写的「`run_tests.bat` 末尾自动调用 `tests/unit/run_tests_wsl.sh`」不成立**：
>   该脚本从未落地（`git ls-files | grep wsl` 为空），`run_tests.bat` 里也没有任何 WSL 调用。
>   要用 WSL 跑测试得手动 `wsl -d Ubuntu -- bash <脚本>`。
>   直接编译 `/mnt/d/...` 可用但 9p 较慢，重负载建议把源码放 WSL 内部的 `/home`。

设备：Redmi myron（serial `134d2f8`，KernelSU root，内核 6.12.69，SELinux Permissive）。
adb 在 `D:\Kiln\platform-tools\adb.exe`（已入 PATH，`where adb` 可验；当前无设备连接）。
服务/窗口排查：`dumpsys window windows | grep ggmcp`；崩溃：`logcat -s AndroidRuntime:E`。

## 已知债（2026-10-15 复核，2026-10-21 校准，2026-10-22 增补）

1. **main.c 巨型文件**：已拆 shell 域（`cmd_shell.c`）、view/符号/指针链域（`cmd_view.c`），
   现 2097 行（2026-10-22 `wc -l` 实测，因地址参数加固增加约 250 行）；待拆 `cmd_search` / `cmd_mem` / `cmd_proc` / dispatch。拆前后必跑 `check_dispatch_consistency.py`，
   并跑 `scripts\build_ndk.bat` 确认可编译（NDK+capstone 已就位，不再是盲改）。
2. wire 协议：1MiB 上限无分片（`GGWM_MAX_PAYLOAD`）；XOR seed 固定 `0xA5`（`shared/wire_protocol.h`）；serial 硬编码在 config。
   - 实测余量：`gg_get_results` 上限 10000 条（`agentd/main.c:1166` 硬钳），每条 `addr`/`value`/`value_hex` 三个字符串，
     单条序列化 61–74B → 整帧 JSON 约 **610–740KB**，距 1MiB 剩约 29%–42%（2026-10-21 按序列化格式实测）。
     → **补分片之前不要上调 limit**，否则单帧会被 `read_frame`（`server/device/wire.cpp:286`）直接判非法。
3. ~~`docs/` 工程文档与逆向研究笔记混放，待分目录~~ —— 2026-10-21 已分为 `docs/engineering/` / `docs/research/`，
   索引见 `docs/README.md`（研究笔记仅限本仓，不外发）。
4. ~~`archive/framework_v1/` 全量版框架（perf/atomic/error/mempool/conn_pool/io_batch）未接线~~ —— 2026-10-21 已删除（8 个文件）。
   理由：未接线，且与现役 `*_simple` 是同一职责的两套实现，留着必然被误用。需要时从提交 `a29871b` 的 `archive/framework_v1/` 恢复。
5. ~~**capstone 5.0.1 的 aarch64 静态库缺失**~~ —— 2026-10-21 17:44 已装入并实测 `build_ndk.bat` 全绿（本节保留为历史记录）。原判断：
   agentd / native 无法本地编译（`D:\Kiln\capstone\capstone-5.0.1\libcapstone.a`
   当时不存在，`scripts/build_ndk.bat` 直接 `exit /b 1`）。**该阻塞项已解除。**
6. **无真机连接时 `tests/integration/run_itest.ps1` 无法执行** → 设备端改动等于没有闭环，只能等设备接上后补验，
   期间不得把设备端改动当作「已验证」上报（2026-10-21）。
7. **rwproc33 全量搜索崩内核已定位**（2026-10-22）：根因在驱动侧，不在 agentd。
   ① `phy_mem_auto_offset.h:45` 用 `__pa(si.totalram)` 算物理地址上限（字节数当内核 VA）→ 范围检查失效；
   ② `get_proc_phy_addr` 不判 pmd 大页，THP 的 pmd 被 `pte_offset_kernel` 当页表指针；
   ③ 本地打开 `CONFIG_ENABLE_FAST_PHYMEM` 的 `__va()` 后无 `virt_addr_valid` 兜底；
   ④ 页表遍历在 `mmap_lock` 外。四处在 `build/rwProcMem33` 已改。
   **2026-10-22 晚真机验证通过**（myron / 6.12.69-android16-6）：GKI 树（设备 config +
   clang-18，SUBLEVEL 改 69、LOCALVERSION 补 `-android16-6-4k`）编出的 KO `this_module=1600`、
   vermagic 逐字匹配；`driver_self_test: channel OK`；`run_itest.ps1` **ITEST ALL PASS**；
   **com.android.systemui（341MB / 5940 VMA）全量匿名搜索 4.8s 完成、`truncated=false`、设备零 Oops**
   —— §十二 会让设备重启的那个操作现在正常收尾。
   关键修正：**不能用 `si.totalram` 当物理地址上界**（真机实测合法 pfn 可达 0x926afc=39.3GB，
   RAM 只有 11.6GB，PA_BITS=48），合法性只由 `virt_addr_valid()` 判。
   地址/值参数的两种形式（`"0x..."` 字符串与 JSON 数字）在真机逐项一致 —— 但**前提是先部署新
   `gdm`**：`run_itest.ps1` 用的是自带 itest 二进制（链接 `driver.c`），它绿了不代表设备上
   跑的 agentd 是新版。本轮就踩过：数字 `value` 报 `bad value`，实为设备还是 00:17 的旧 gdm。
   详见 `docs/audit/RWPROC33_2026-10-22.md`「真机验证结论」节。
8. **本轮（2026-10-22）已修但设备端未验**：地址参数两端类型约定不一致（数字地址让 `gg_watch_inject`
   `strtoull(NULL)` 段错误、`gg_write {value:100}` 误报缺参）、`parse_value_str` 缺 endptr、HEX_DUMP 响应 cmd 错位、
   safe_mode 漏 `ghost_tap/swipe`、elfsym 字符串表越界；server 端 Windows connect 缺 `SO_ERROR`（把「设备没连上」报成
   「令牌鉴权失败」）、HTTP 暴露面判定、JSON-RPC batch。测试程序 13 → 15 个（新增 `test_protocol` / `test_net_guard`）。
   `run_tests.bat` 全绿只代表 PC 侧逻辑正确，设备端行为仍需接真机。

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
- **wire 鉴权**（2026-10-21 起）：agentd 未认证的连接只放行 `PING`/`AUTH`，其余一律 `GGERR_PERM`。
  密钥在设备端 `/data/adb/GG-Ghost/wire.key`（`0600`，首启自动生成 32 字节随机 hex），
  通过 `--wire-key-file` 传入；PC 端 `configs/gg-ghost.json` 的 `wire_key` 必须与之一致，
  `deploy.ps1` 会自动拉取并写回配置。**wire_key 与 KO 密钥同级别，不入库、不入对话、不贴日志。**
  因此 `configs/gg-ghost.json` **不进版本库**（已列入 `.gitignore`）：它是部署生成物，
  入库的只有模板 `configs/gg-ghost.example.json`（`wire_key` 为空），首次部署由
  `deploy.ps1` 从模板复制后再写入真值。曾经因为跟踪了它，21 个提交把真实 key 带进过历史。
- 敏感研究笔记（ACE/反作弊对抗）只放 `docs/research/`，不外发；驱动密钥、QX_KEY 等不入库不入对话
- 设备端改动必须在真机过 itest（scripts/deploy.bat 部署后跑 tests/integration/run_itest.ps1）
- 特征字符串纪律：agentd 二进制 strings 不得出现 GG-Ghost / gg-touch / GG_KO 等特征（历史提交 88d83cc、21b246d 有验证方法）
- `.bat` 一律 ASCII-only 注释：cmd 按 ANSI（GBK）解析批处理，UTF-8 中文会吞掉 `^` 续行并破坏脚本

## 历史

apk/ 悬浮窗 APK 与 ggtool/ UI Demo 线已于 2026-08-27 下线（commit bcdcf1f，可从 git 历史恢复）。
旧版工作区指令（apk/ggtool 时代）归档在 `docs/engineering/AGENTD_HISTORY.md`。

本文件最后校准：2026-10-21（工具链、main.c 行数、wire 余量均按本机实测）。
