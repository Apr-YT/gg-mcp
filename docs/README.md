# GG-MCP 文档索引

四层：`engineering/`（活跃工程文档）、`reference/`（外部技术参考）、`research/`（逆向笔记）、
`archive/`（历史归档，**不再维护**）。

> **现状数字以 `PROJECT_MAP.md` 为准**（2026-10-23，HEAD `8bddaf5`）：52 个工具 / 46 条 wire 命令枚举
> （有效 41）/ 15 个可执行单测 + 6 道结构护栏 / 两端均可本机编译。真机 itest 在 10-22 晚曾 ALL PASS，
> **10-23 这轮改动未上机**。
> 归档区里的数字是当时的，不当现状用。

> ⚠️ `research/` 下的逆向研究笔记**仅限本仓内部使用，不外发**。

## 活跃工程文档（`engineering/`）

| 文件 | 用途 |
|---|---|
| [PROJECT_MAP.md](engineering/PROJECT_MAP.md) | **现状唯一入口**：进程边界、通信链路、各模块行数、52 个工具面、构建/测试能力、当前债务。查现状先看这份。 |
| [DESIGN.md](engineering/DESIGN.md) | **设计说明**：信任边界、调用通路、协议约定（ABI / 混淆协商 / AUTH 门 / 单帧上限）、并发与锁层级、会话不变量、失败语义与限额表。动跨组件的东西先看这份。 |
| [PROTOCOL.md](engineering/PROTOCOL.md) | wire 协议（20B 头 + JSON，含 AUTH 握手）与 rwproc33 驱动协议（33B 头 + read）笔记。 |
| [ARCHITECTURE.md](engineering/ARCHITECTURE.md) | 组件划分、数据流与驱动通道的架构说明。 |
| [ACCEPTANCE.md](engineering/ACCEPTANCE.md) | KO 驱动方案验收清单，区分「本机可验证」与「需真机」。 |
| [DRIVER_MATRIX.md](engineering/DRIVER_MATRIX.md) | **驱动通道能力矩阵**：rwproc33 单型的通道与状态判定、工具×驱动依赖、**§8 KO 版本谱系（哪个 .ko 能用）**。判断「驱动到底好不好」先看这份。 |
| [RWPROC33_ADAPTER.md](engineering/RWPROC33_ADAPTER.md) | rwProcMem33 适配细节：命令枚举、请求结构、硬件断点与构建要点；§十一 跨内核复用 prebuilt、§十二 全扫崩内核的实测证据。 |
| [GG-GHOST.md](engineering/GG-GHOST.md) | GG-Ghost 内核虚拟触摸注入套件（`gg-ghost` input 设备 + proc 注入协议）。 |
| [GG_UI.md](engineering/GG_UI.md) | PC 侧搜索界面（imgui + Win32 + D3D11）：`scripts\build_ui.bat` 构建，链接 `Session::call_tool`，与 MCP 客户端同一条路径。 |
| [UE4_REFLECTION.md](engineering/UE4_REFLECTION.md) | UE4 反射体系（GNames / GUObjectArray / GWorld）的定位方法与偏移推导，`gg_aob_resolve` 的实战来源。 |
| [TOOLCHAIN.md](engineering/TOOLCHAIN.md) | 本机工具链实况（NDK r30 / capstone 5.0.1 / MinGW），跨平台构建 agentd 的可复制步骤。 |
| [BACKLOG.md](engineering/BACKLOG.md) | 待办清单，按 P0/P1/P2 分级，每条带证据；末尾附"明确不做"及理由。动手优化前先看。 |
| [SOURCE_REVIEW_20261023.md](engineering/SOURCE_REVIEW_20261023.md) | **源码全量梳理 + 修复记录**：锁矩阵、风险清单（P0–P2）、§九 逐条处置（已修/仅缓解/有意不改）。想知道"为什么这么改"看这份。 |
| [DOC_AUDIT_20261023.md](engineering/DOC_AUDIT_20261023.md) | **文档梳理记录**：分层与归档规则、查出的漂移、数字基线取法、维护约定。加文档或改代码后先看 §6。 |

## 参考资料（`reference/`）

| 文件 | 用途 |
|---|---|
| [UE4_ESP_TECH.md](reference/UE4_ESP_TECH.md) | UE4 ESP 技术参考（来自 Apr-YT/esp-android）：GWorld 链路、实体遍历与坐标读取。 |

## 逆向研究笔记（`research/`）

> ⚠️ 仅限本仓内部，不外发。

| 文件 | 用途 |
|---|---|
| [CF-ESP代码框架.md](research/CF-ESP代码框架.md) | CF ESP 数据面框架：dump.cs 结构 + 运行时偏移 + W2S/对象遍历。 |
| [CF-Pandora-Lua.md](research/CF-Pandora-Lua.md) | CF Pandora SDK Lua 逆向：`_G` 全局表定位与 `pua_State` 结构。 |
| [CF反作弊检测机制分析.md](research/CF反作弊检测机制分析.md) | 腾讯 ACE 检测架构 / 采集面 / 封号机制逆向全景。 |
| [三角洲ACE云下发检测文件分析.md](research/三角洲ACE云下发检测文件分析.md) | 三角洲启动时 ACE 云端下发检测文件的解包与逐项分析。 |
| [三角洲行动手游ACE分析.md](research/三角洲行动手游ACE分析.md) | 三角洲手游 ACE 集成深度、坐标 shellcode 加密与 rwproc33 无痕解密。 |
| [FPS-ESP完整技术路线.md](research/FPS-ESP完整技术路线.md) | FPS ESP 从数据面（坐标/矩阵/实体列表）到渲染层的完整路线。 |
| [FPS矩阵绘制算法原理-全合集详解.md](research/FPS矩阵绘制算法原理-全合集详解.md) | 矩阵绘制算法教程合集整理（11 集）。 |
| [Unity Camera ICall 逆向.md](research/Unity%20Camera%20ICall%20逆向.md) | 由 `libunity.so` 的 ICall 注册表定位相机矩阵的通用方法论。 |
| [加密类型识别方法.md](research/加密类型识别方法.md) | 判定内存数据被哪种方式加密的决策树，坐标加密的配套篇。 |
| [坐标加密对抗方法.md](research/坐标加密对抗方法.md) | 坐标加密的 7 种形式、识别特征与破解链。 |

## 归档（`archive/`）

**不维护，只追溯用。** 里面的工具数 / 行数 / 测试结果是当时的结论，不要拿来对现状。

| 组 | 文件 |
|---|---|
| 审计记录 | `audit/` 下 6 篇：2026-10-21 基线/缺陷清单/总结（`BASELINE`、`FINDINGS`、`FINAL_REPORT`、`EXTERNAL_AUDIT_TRIAGE`）、2026-10-22 联合审计（`UNIFIED_2026-10-22`）与 rwproc33 崩内核根因（`RWPROC33_2026-10-22`） |
| 已完成的方案与重构记录 | `MAIN_C_SPLIT`（main.c 拆分，已完成）、`CODE_REFACTOR`（重构指南）、`MERGE_PLAN`（双线合并计划） |
| 审查与调研 | `CODE_REVIEW_2026-10-21`、`CHECKPOINT_REVIEW_REPORT`、`PEER_REVIEW_2026-10-23`、`COMMUNITY_TOOLS_SURVEY` |
| 实战与历史 | `PRACTICE_HILLCLIMB`（靶场实战）、`DEVICE_STATUS_6.12.69`、`HWBP_CRASH_HELP`、`HANDOVER`、`AGENTD_HISTORY`、`CHANGELOG_CLOSED`、`OPTIMIZATION_SUMMARY`、`VERSION_MATCHING`（已作废）、`MEMORY`、`OLD_INDEX`（旧索引） |

归类原则见 `engineering/DOC_AUDIT_20261023.md` §2：**能指导当前决策的进 `engineering/`，
记录"当时怎么想 / 怎么做"的进 `archive/`**。
