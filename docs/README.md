# GG-MCP 文档索引

`docs/` 按性质分两类：`engineering/` 承载工程文档（架构/协议/验收/交接/优化记录），`research/` 承载逆向研究笔记。

> 2026-10-22 校准：`PROJECT_MAP.md`、`ARCHITECTURE.md`、`PROTOCOL.md` 已按 HEAD `1736072` 的代码实测更新
> （代码量、44 个工具、42 条 wire 命令枚举、rwproc33 单通道无降级、16 个单测程序 + 3 道护栏、真机 ITEST ALL PASS）。

> ⚠️ `research/` 下的逆向研究笔记**仅限本仓内部使用，不外发**。

## 工程文档（`engineering/`）

| 文件 | 用途 |
|---|---|
| [DESIGN.md](engineering/DESIGN.md) | **设计说明**：信任边界、调用通路、协议约定（ABI / 混淆协商 / AUTH 门 / 单帧上限）、并发与锁层级、引擎数据结构、会话不变量、安全分层、失败语义与限额表、改动 checklist。要动跨组件的东西先看这份。 |
| [PROJECT_MAP.md](engineering/PROJECT_MAP.md) | **项目总览**：进程边界、通信链路、各模块行数、44 个工具面、构建/测试能力与债务清单。查现状先看这份。 |
| [ACCEPTANCE.md](engineering/ACCEPTANCE.md) | KO 驱动方案验收清单，区分「本机可验证」与「需真机」两类项。 |
| [ARCHITECTURE.md](engineering/ARCHITECTURE.md) | 组件划分、数据流与驱动通道的架构说明。 |
| [PROTOCOL.md](engineering/PROTOCOL.md) | PC ↔ gg-agentd 的 wire 协议（20B 头 + JSON，含 AUTH 握手）与 rwproc33 驱动协议（33B 头 + read）笔记。 |
| [HANDOVER.md](engineering/HANDOVER.md) | 项目交接文档；正文是 08-16 历史记录，文首有 2026-10-15 的路径/命名更新。 |
| [CODE_REFACTOR.md](engineering/CODE_REFACTOR.md) | search.c 等模块的历史重构指南。 |
| [OPTIMIZATION_SUMMARY.md](engineering/OPTIMIZATION_SUMMARY.md) | 2026-08-22 优化工作总结；文首有 08-28 更正声明，**部分声明不实，以更正声明为准**。 |
| [CHECKPOINT_REVIEW_REPORT.md](engineering/CHECKPOINT_REVIEW_REPORT.md) | 2026-08-27 检查点审查报告；文首有历史快照声明，**勿当现状**。 |
| [DRIVER_MATRIX.md](engineering/DRIVER_MATRIX.md) | **驱动通道能力矩阵**：rwproc33 单型的通道与状态值判定依据、`ST_KO_HOOK` 假阳性的成因（三型已删，见 §6）、工具×驱动依赖、**§8 KO 版本谱系（哪个 .ko 能用）**。判断「驱动到底好不好」先看这份。 |
| [GG_UI.md](engineering/GG_UI.md) | **PC 侧搜索界面（imgui + Win32 + D3D11）**：`scripts\build_ui.bat` 构建，`build\pc\gg-ui.exe` 直接连 agentd。不重复实现协议 —— 链接 `Session::call_tool`，与 MCP 客户端同一条路径。含字体/版本两个坑的记录。 |
| [BACKLOG.md](engineering/BACKLOG.md) | **项目体检 / 待办清单**：按 P0/P1/P2 分级，每条带证据或实测数据；末尾附"明确不做"及其理由（CFI 还原、结果自动刷新、UI 内做 watch）。动手优化前先看这份，避免重复论证。 |
| [MAIN_C_SPLIT.md](engineering/MAIN_C_SPLIT.md) | **main.c 拆分方案（规划中）**：按行号切段、每段标注去向与所属 Phase、每步的验收方式。动手拆之前先读，避免和已拆的 `cmd_*.c` 重复。 |
| [CODE_REVIEW_2026-10-21.md](engineering/CODE_REVIEW_2026-10-21.md) | 2026-10-21 全量代码审查：通信层加固（wire 鉴权）、内存边界修复、静态分析与真机验证记录。 |
| [RWPROC33_ADAPTER.md](engineering/RWPROC33_ADAPTER.md) | rwProcMem33 驱动接口适配：命令枚举、请求结构、硬件断点与构建要点；§十一 跨内核复用 prebuilt、§十二 rwproc 全扫崩内核的实测证据。 |
| [HWBP_CRASH_HELP.md](engineering/HWBP_CRASH_HELP.md) | hwBreakpointProc_module 静默挂死排查记录（已解决：命令枚举错位），同类内核挂死可复用其排查法。 |
| [TOOLCHAIN.md](engineering/TOOLCHAIN.md) | 本机工具链实况（NDK r30 / capstone 5.0.1 / MinGW），跨平台构建 agentd 的可复制步骤。 |
| [DEVICE_STATUS_6.12.69.md](engineering/DEVICE_STATUS_6.12.69.md) | 内核 6.12.69 设备上的部署与运行状态报告（2026-08-22 快照）。 |
| [VERSION_MATCHING.md](engineering/VERSION_MATCHING.md) | 版本族（major.minor 同级放行）匹配规则的历史记录；**已整体作废**（GG 核心链路已删，KO 侧改为 `install_ko.sh` 精确版本匹配），文首有两次作废声明。 |
| [GG-GHOST.md](engineering/GG-GHOST.md) | GG-Ghost 内核虚拟触摸注入套件（`gg-ghost` input 设备 + proc 注入协议）完整文档。 |
| [AGENTD_HISTORY.md](engineering/AGENTD_HISTORY.md) | apk/ggtool 时代的旧版工作区指令归档，仅供追溯。 |
| [CHANGELOG_CLOSED.md](engineering/CHANGELOG_CLOSED.md) | 已关闭的历史条目（从 AGENTS.md 迁出）：docs 分目录、framework_v1 删除、capstone 就位、构建脚本修复、IDA 联动移除等。仅供追溯，不是现行约束。 |
| [MEMORY.md](engineering/MEMORY.md) | AI 会话持久记忆：环境事实、命令、踩坑结论的自动维护清单。 |
| [README.md](engineering/README.md) | 2026-09 前的旧版文档索引，已被本文件取代，仅作历史留档。 |

## 审计记录（`audit/`）

按时间倒序。每条结论都带「代码可证 / 推断 / 未验证」的标注，引用前先看该条的状态。

| 文件 | 用途 |
|---|---|
| [UNIFIED_2026-10-22.md](audit/UNIFIED_2026-10-22.md) | 2026-10-22 gg-mcp 与 Ai-root 联合审计：20 项修复（地址参数类型、Windows `SO_ERROR`、HTTP 暴露面、确认哈希单射、管道 ACL 等）、未修高危清单与验证状态。**注意：Ai-root 不是本项目的第二个组件**，结论只对 Ai-root 自身成立。 |
| [RWPROC33_2026-10-22.md](audit/RWPROC33_2026-10-22.md) | 2026-10-22 rwproc33 驱动全量搜索崩内核的根因定位（四条，含代码证据）、已改的 4 个文件、两版源码的功能互补关系、两端接口契约核对。 |
| [FINAL_REPORT.md](audit/FINAL_REPORT.md) | 2026-10-21 全量审查总结。 |
| [FINDINGS.md](audit/FINDINGS.md) | 2026-10-21 缺陷清单（逐条编号）。 |
| [BASELINE.md](audit/BASELINE.md) | 2026-10-21 审查基线（环境、命令、初始状态）。 |
| [EXTERNAL_AUDIT_TRIAGE.md](audit/EXTERNAL_AUDIT_TRIAGE.md) | 外部审查报告的逐条处置记录。 |

## 逆向研究笔记（`research/`）

> ⚠️ 仅限本仓内部，不外发。

| 文件 | 用途 |
|---|---|
| [CF-ESP代码框架.md](research/CF-ESP代码框架.md) | CF ESP 数据面框架：dump.cs 结构 + 运行时偏移 + W2S/对象遍历。 |
| [CF-Pandora-Lua.md](research/CF-Pandora-Lua.md) | CF Pandora SDK Lua 逆向：`_G` 全局表定位与 `pua_State` 结构。 |
| [CF反作弊检测机制分析.md](research/CF反作弊检测机制分析.md) | 腾讯 ACE 检测架构/采集面/封号机制逆向全景。 |
| [三角洲ACE云下发检测文件分析.md](research/三角洲ACE云下发检测文件分析.md) | 三角洲启动时 ACE 云端下发检测文件的解包与逐项分析。 |
| [三角洲行动手游ACE分析.md](research/三角洲行动手游ACE分析.md) | 三角洲手游 ACE 集成深度、坐标 shellcode 加密与 rwproc33 无痕解密。 |
| [FPS-ESP完整技术路线.md](research/FPS-ESP完整技术路线.md) | FPS ESP 从数据面（坐标/矩阵/实体列表）到渲染层的完整技术路线。 |
| [FPS矩阵绘制算法原理-全合集详解.md](research/FPS矩阵绘制算法原理-全合集详解.md) | 矩阵绘制算法教程合集整理（11 集，4 小时余）。 |
| [Unity Camera ICall 逆向.md](research/Unity%20Camera%20ICall%20逆向.md) | 由 `libunity.so` 的 ICall 注册表定位相机矩阵的通用方法论。 |
| [加密类型识别方法.md](research/加密类型识别方法.md) | 判定内存数据被哪种方式加密的决策树方法论，坐标加密的配套篇。 |
| [坐标加密对抗方法.md](research/坐标加密对抗方法.md) | 坐标加密的 7 种形式、识别特征与解密破解链，ESP 对抗面专项。 |
