# AGENTS.md — GG-MCP 工作区指令（每个会话自动加载）

## 项目一句话

内核驱动式内存修改 MCP 服务：PC 上的 AI agent 通过 MCP 直接读写安卓游戏进程内存。
**只走 KO 内核驱动通道**（rwproc33）。GG 核心注入链路（`lib5_stub` + `libKernelGg.so`，
含 hook syscall 270）已于 2026-10-21 整条删除，命令码 28/29 只保留占位，调用会得到明确的
`BAD_REQUEST`。

## 项目结构

```
server/    PC 端 gg-ghost-mcp（C++17）：MCP 协议 + 会话 + wire 客户端
agentd/    设备端 gg-agentd（C, NDK arm64, root）：搜索/读写/冻结/监视 + 驱动对接
shared/    wire_protocol.h — PC/设备双端协议唯一事实源（20B 头 + JSON 负载；含 GGCMD_AUTH 握手）
ko/        内核模块源码快照（rwProcMem33 两个模块，184 源文件）——**改驱动必须同步这里**
tests/     unit（posix_stub 可 PC 跑）/ integration / regression 三层
scripts/   构建（build_pc.bat / build_ndk.bat / build_all.bat）、部署、测试、基准、KO 源码同步
configs/   gg-ghost.example.json（模板，入库）；gg-ghost.json 由 deploy 生成、**不入库**
tools/     辅助脚本（cfm_coord_read.py 等）
docs/      engineering/（活跃工程文档）+ reference/ + research/（逆向笔记，仅限本仓）
           + archive/（历史归档，不维护），索引见 docs/README.md
build/     构建产物与上游克隆（不入库）；build/rwProcMem33 是 KO 工作副本，含未推送提交
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
tests\unit\run_tests.bat          # 单元 + 6 道结构护栏（先跑这个）
scripts\test_all.bat              # 单元 + 集成 + 基准（缺前置会明确 SKIP）
scripts\benchmark.bat             # ⚠️ 仓内无基准源码，会直接报错退出

# 结构守护（拆分/改分发/改驱动对接后必跑；run_tests.bat 已自动执行）
python tests\unit\check_dispatch_consistency.py      # schema(52) ↔ session(52)
python tests\unit\check_kernel_only.py               # driver.c 不得回落用户态通道
python tests\unit\check_rwproc_locking.py            # rwproc_buf 拷贝必须在锁内
python tests\unit\check_hwbp_detail.py               # HIT_DETAIL 的 buf_size 非 0
python tests\unit\check_scalar_cmd.py                # 标量返回命令不许走 call_ex
python scripts\check_ko_src_sync.py                  # ko/ 快照 vs build/ 工作副本
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
adb 在 `D:\Kiln\platform-tools\adb.exe`（已入 PATH，`where adb` 可验）。2026-10-22 晚设备在位并已跑通 itest。
服务/窗口排查：`dumpsys window windows | grep ggmcp`；崩溃：`logcat -s AndroidRuntime:E`。

## 已知债索引（编号即历史 ID；2026-11-04 起按主题分册，原文逐字搬移）

主文件只留索引与硬约束，**条目全文在分册**：

| 分册 | 主题 | 条目 |
|---|---|---|
| `AGENTS.driver.md` | 驱动 / 内核模块 / hwbp / 隐蔽性 | 4, 20, 21, 22, 23, 24, 27, 28, 29, 31, 32, 33, 34, 43 |
| `AGENTS.agentd.md` | 设备端 agentd / 搜索引擎 / 会话 | 1, 5, 6, 7, 8, 9, 12, 13, 14, 16, 17, 19, 26, 30, 37, 38, 40, 41, 42, 44, 45, 46, 47, 49, 50 |
| `AGENTS.server.md` | PC server / wire 协议 / UI | 2, 10, 11, 15, 18, 25, 35, 36, 39, 48, 51 |

**按任务选分册（动代码前先读对应分册）**：
- 动 ko / 驱动读路径 / hwbp / 隐蔽性 → AGENTS.driver.md
- 动 agentd 搜索/refine/freeze/会话/handler → AGENTS.agentd.md
- 动 wire 协议 / PC server / UI → AGENTS.server.md
- 新增条目：续最大编号，写进对应分册，并更新本表与校准行。

## 真机纪律（原第 3 条，全项目唯一保留在主文件的条目）

3. **无真机连接时 `tests/integration/run_itest.ps1` 无法执行** → 设备端改动等于没有闭环，只能等设备接上后补验，
   期间不得把设备端改动当作「已验证」上报（2026-10-21）。
   2026-10-23 状态：设备 `134d2f8` 在位，但当日那轮改动（`search.c`/`process.c`/`main.c`/
   `watch.c`/`wire_auth.c`）**只过了编译，没上机** —— 别把"build_ndk 绿"当成"行为正确"。

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
旧版工作区指令（apk/ggtool 时代）归档在 `docs/archive/AGENTD_HISTORY.md`。

本文件最后校准：2026-11-08（server 册新增第 51 条 gg-ui 异步队列）。
上一条校准：2026-11-04（P1-15 拆分：50 条按主题入三个分册，主文件留索引+硬约束；分册内编号与原文不变）。
上一条校准：2026-11-04（第 50 条 freeze save/load + 第 49 条 gg_results_refresh；同日：第 47 条 region dump/restore + 第 48 条 wire AUTH 断连语义修复；更早：第 45 条 gg_pointer_path + 第 46 条搜索 module 参数）。
上一条校准：2026-11-03（第 43 条 anon_ring 审查 + 第 44 条 BACKLOG 5/6/12 收口）。
上一条校准：2026-10-23 19:55（HEAD `8bddaf5`）：项目结构补 `ko/` 与 `build/` 说明、
结构护栏补到 6 道、main.c 拆分收尾（683 行）、清掉已删除的 GG 核心注入链路表述、
itest 状态更新、第 14/16 条矛盾合并。同日新增 42。
