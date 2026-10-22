# PROJECT_MAP.md — gg-mcp 项目梳理

> 生成于 2026-10-21，起于 HEAD `6106717`，合并三条并行分支后校准至 `8449ffe`，工作区实测。
> 本文只描述**当前实际状态**，与 `ARCHITECTURE.md`（2026-08 定稿）冲突处以本文为准。

## 1. 一句话定位

PC 上的 MCP 服务器 + 安卓 root 端 agentd + 内核 KO 驱动，让 AI agent 通过标准 MCP 工具读写安卓游戏进程内存。**只走 KO 内核驱动通道**，不依赖 KPM。

## 2. 三个进程边界

| 层 | 产物 | 语言 | 运行位置 | 代码量 |
|---|---|---|---|---|
| PC 侧 | `gg-ghost-mcp.exe` | C++17 | Windows | 2315 行 / 12 文件 |
| 设备侧 | `gg-agentd` | C（NDK arm64） | Android root | 7122 行 / 27 文件 |
| 内核侧 | `build/ko/*.ko` | 预编译二进制 | Android kernel | 2 模块 × 2 内核版本 |
| 协议 | `wire_protocol.h` | 纯 C | 双端共用 | 162 行 / 1 文件 |

设备侧占全仓 60%，是真正的重心；PC 侧只是协议面与参数收敛层。

## 3. 通信链路

```
MCP client ──JSON-RPC──▶ gg-ghost-mcp ──20B 帧头 + JSON 负载──▶ adb forward tcp:34127
                              │                                        │
                              │                                        ▼
                              │                                   gg-agentd
                              │                                        │
                              │                    ┌───────────────────┼───────────────────┐
                              │                    ▼                   ▼                   ▼
                              │              KO hook 型            KO dev 型            QX 型
                              │        (hook process_vm_*)     (/dev + ioctl)   (/proc/pid/fd/3)
                              │                    │                   │                   │
                              │                    └───────────────────┴───────────────────┘
                              │                                        │
                              └────────── 响应 {"r":{}} / 错误 {"e":{}} ┘

降级链：KO 通道 → process_vm_readv → /proc/<pid>/mem（kernel_status 标记当前层）
```

## 4. 模块职责

### PC 侧 `server/`

| 文件 | 行数 | 职责 |
|---|---:|---|
| `session.cpp` | 823 | 43 个工具的 handler + 参数规范化 + save_to 落盘 |
| `device/wire.cpp` | 574 | 帧收发、失败语义、整体截止时间、adb forward 管理 |
| `mcp/schema.cpp` | 344 | 工具 schema（与 handler 的单一对齐点，护栏会校验） |
| `main.cpp` | 232 | 入口、配置加载与校验、stdio/HTTP 两种传输 |
| `net_guard.h` | 60 | HTTP 暴露面判定（监听地址是否回环、浏览器 Origin），纯 inline、可单测 |
| `annotations.cpp` | 182 | 标注库（纯本地，不经设备） |
| `mcp/protocol.cpp` | 149 | JSON-RPC 分层、错误码、批量请求 |
| `proxy-server.cpp` | 60 | 独立代理进程（与主链路无关） |

### 设备侧 `agentd/`

| 文件 | 行数 | 职责 |
|---|---:|---|
| `main.c` | 2097 | 命令分发 + 参数闸门（地址 / safe_mode / 握手）+ 事件广播 + 会话状态 |
| `driver.c` | 623 | 四型驱动通道统一抽象（hook / dev / qx / rwproc）+ 降级 |
| `search.c` | 824 | 精确/区间/模糊搜索，位图 + 快照 |
| `watch.c` | 414 | 硬件断点监视（CE 式"谁访问了这个地址"） |
| `process.c` | 367 | 进程枚举与 maps 解析 |
| `cmd_view.c` | 313 | 结构化内存查看 / 符号解析 / 指针链 |
| `elfsym.c` | 249 | ELF `.dynsym` 解析（符号名有界读取 + 字符串表上限） |
| `freeze.c` / `disasm.c` / `cmd_shell.c` / `version.c` | 453 | 冻结线程、Capstone 反汇编、shell 白名单、版本匹配 |

> 行数为 2026-10-22 `wc -l` 实测。此前版本的表已整体过时（`driver.c` 记的 1109 与实测差近一倍），
> 改代码后请顺手重测，别让它继续漂。

## 5. 工具面（43 个）

（分组清单见下表，各处数字以 `tests/unit/check_dispatch_consistency.py` 的输出为准 —— 它比对
`schema.cpp` 与 `session.cpp` 两份清单，任一新增/改名只改一边就会失败。）

| 分组 | 数量 | 工具 |
|---|---:|---|
| 进程/会话 | 5 | list_processes, attach, detach, target_info, module_list |
| 搜索 | 7 | search, search_unknown_init, search_refine, search_range, result_count, get_results, clear_results |
| 读写 | 6 | read, read_bytes, hex_dump, view_memory, write, set_results |
| 冻结 | 2 | freeze, unfreeze |
| 地址解析 | 4 | module_base, calc_offset, pointer_scan, pointer_chain |
| 符号/反汇编 | 2 | resolve_symbol, disassemble |
| 执行控制 | 2 | pause, resume |
| 内核/驱动 | 1 | kernel_status |
| 硬件断点 | 4 | watch_set, watch_poll, watch_inject, watch_clear |
| 设备/系统 | 3 | status, shell, device_info |
| 标注（PC 本地） | 5 | annotate_add, list, get, remove, context |
| 虚拟触摸 | 2 | ghost_tap, ghost_swipe |

43 个工具中，6 个是纯 PC 本地实现（标注库 5 个 + `status`），其余下发到 agentd；差额还来自
多工具共用同一命令（`read_bytes` / `hex_dump` 共用 `GGCMD_READ_BYTES`，`watch_*` 共用 `GGCMD_WATCH`）。

## 6. 构建与验证能力（本机实测）

| 能力 | 状态 | 说明 |
|---|---|---|
| PC 端编译 | ✅ | MinGW g++ 16.2.0，`scripts/build_pc.bat` |
| PC 端单测 | ✅ | `tests/unit/run_tests.bat`，全绿（含真 socket 失败语义回归） |
| 结构护栏 | ✅ | dispatch 一致性（45↔45）+ kernel-only |
| **agentd / native 编译** | ✅ | **NDK r30 + capstone 5.0.1 均已就位**，`build_ndk.bat` 实测全绿 |
| 真机集成测试 | ⚠️ | adb 在（`D:\Kiln\platform-tools`），当前**无设备连接**，跑不了 `run_itest.ps1` |
| 性能基准 | ❌ | 仓内无基准源码，`benchmark.bat` 会明确报错 |

产出实测：`gg-agentd` 2.4MB（ELF64 AArch64 PIE，仅 NEEDED `libc.so`/`libdl.so`，`-Wall` 零警告）。

**三端均可本机编译**（2026-10-21 实测）。真正的闭环缺口只剩**真机 itest**：
没有设备连接时，设备端改动只能编译验证，不能功能验证 —— 不得当作「已验证」上报。

## 7. 测试覆盖

31 个文件 / 2434 行，三层：

- **unit**（PC 可跑）：`test_wire`（头布局 + QX 结构 static_assert）、`test_wire_io`（真 socket 假 agentd，验失败语义）、`test_session`（枚举校验）、`test_search`、`test_aob`、`test_freeze`、`test_watch`、`test_elfsym`、`test_process_maps`、`test_version`、`test_ko_downgrade`、`test_shell_security`、`test_parallel`
- **integration**：`run_itest.ps1` + itest/ltest/qtest（需真机 + root）
- **regression**：`test_api.bat`

通信层失败语义（ResponseTimeout / OutcomeUnknown / ProtocolError）**已有真 socket 回归**，不再只靠手工验证。

## 8. 已知债务与文档漂移

| 项 | 状态 |
|---|---|
| `agentd/main.c` 仍 1846 行 | 已拆 shell / view 两域，待拆 cmd_search / cmd_mem / cmd_proc / dispatch；拆前后跑 `check_dispatch_consistency.py` + `build_ndk.bat` |
| wire 1MiB 单帧上限无分片 | `gg_get_results` 上限 10000 条，实测整帧 610–740KB（单条 61–74B），距 1MiB 余量 29%–42%。分片前仍**不要**上调该 limit |
| wire 头仍走结构体内存布局 | 无显式 encode/decode，依赖两端同为小端 x86/arm64 |
| ~~`docs/` 混杂~~ | 2026-10-21 已分 `engineering/`（工程）与 `research/`（逆向笔记），索引见 `docs/README.md` |
| `archive/framework_v1/` | 全量框架未接线，要么启用要么删除 |
| `docs/ARCHITECTURE.md:83` | 写着「adb 断连：工具返回 NO_DEVICE，自动重连一次」——**已过时**，当前只在 `ConnectFailed` 时重连 |

## 9. 建议的推进顺序

1. **wire 头显式编解码** —— 需 PC 与 agentd 同步改；两端现在都能编译，改完过真机 itest 即可闭环。
2. **分片** —— 解除 1MiB 硬依赖，才能上调结果上限。同样需要两端同步。
3. **`main.c` 继续拆分** —— 剩 cmd_search / cmd_mem / cmd_proc / dispatch 四域，可编译验证，风险已可控。
4. **真机回归** —— 上面三项都需要设备接上后跑 `run_itest.ps1` 才算完成。
5. **文档治理** —— `ARCHITECTURE.md` 漂移（:83 的重连语义）、`docs/` 已分目录，成本最低，随时可做。

---

*本文为工程梳理，所有结论基于 2026-10-21 的 HEAD 与实际构建/测试结果；环境类判断（NDK/adb/MinGW）已现场核验。*
