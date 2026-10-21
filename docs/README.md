# GG-MCP 文档索引

`docs/` 按性质分两类：`engineering/` 承载工程文档（架构/协议/验收/交接/优化记录），`research/` 承载逆向研究笔记。

> ⚠️ `research/` 下的逆向研究笔记**仅限本仓内部使用，不外发**。

## 工程文档（`engineering/`）

| 文件 | 用途 |
|---|---|
| [PROJECT_MAP.md](engineering/PROJECT_MAP.md) | **项目总览**：进程边界、通信链路、各模块行数、45 个工具面、构建/测试能力与债务清单。查现状先看这份。 |
| [ACCEPTANCE.md](engineering/ACCEPTANCE.md) | KO 驱动方案验收清单，区分「本机可验证」与「需真机」两类项。 |
| [ARCHITECTURE.md](engineering/ARCHITECTURE.md) | 组件划分、数据流与驱动通道的架构说明。 |
| [PROTOCOL.md](engineering/PROTOCOL.md) | PC ↔ gg-agentd 的 wire 协议（20B 头 + JSON）与 KO 驱动 ioctl 协议笔记。 |
| [HANDOVER.md](engineering/HANDOVER.md) | 项目交接文档；正文是 08-16 历史记录，文首有 2026-10-15 的路径/命名更新。 |
| [CODE_REFACTOR.md](engineering/CODE_REFACTOR.md) | search.c 等模块的历史重构指南。 |
| [OPTIMIZATION_SUMMARY.md](engineering/OPTIMIZATION_SUMMARY.md) | 2026-08-22 优化工作总结；文首有 08-28 更正声明，**部分声明不实，以更正声明为准**。 |
| [CHECKPOINT_REVIEW_REPORT.md](engineering/CHECKPOINT_REVIEW_REPORT.md) | 2026-08-27 检查点审查报告；文首有历史快照声明，**勿当现状**。 |
| [RWPROC33_ADAPTER.md](engineering/RWPROC33_ADAPTER.md) | rwProcMem33 驱动接口适配：命令枚举、请求结构、硬件断点与构建要点。 |
| [HWBP_CRASH_HELP.md](engineering/HWBP_CRASH_HELP.md) | hwBreakpointProc_module 静默挂死排查记录（已解决：命令枚举错位），同类内核挂死可复用其排查法。 |
| [TOOLCHAIN.md](engineering/TOOLCHAIN.md) | 本机工具链实况（NDK r30 / capstone 5.0.1 / MinGW），跨平台构建 agentd 的可复制步骤。 |
| [DEVICE_STATUS_6.12.69.md](engineering/DEVICE_STATUS_6.12.69.md) | 内核 6.12.69 设备上的部署与运行状态报告（2026-08-22 快照）。 |
| [VERSION_MATCHING.md](engineering/VERSION_MATCHING.md) | 版本族（major.minor 同级放行）匹配规则，GG 核心与 KO 驱动两处生效。 |
| [GG-GHOST.md](engineering/GG-GHOST.md) | GG-Ghost 内核虚拟触摸注入套件（`gg-ghost` input 设备 + proc 注入协议）完整文档。 |
| [AGENTD_HISTORY.md](engineering/AGENTD_HISTORY.md) | apk/ggtool 时代的旧版工作区指令归档，仅供追溯。 |
| [MEMORY.md](engineering/MEMORY.md) | AI 会话持久记忆：环境事实、命令、踩坑结论的自动维护清单。 |
| [README.md](engineering/README.md) | 2026-09 前的旧版文档索引，已被本文件取代，仅作历史留档。 |

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
