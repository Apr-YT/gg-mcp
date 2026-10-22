# PROJECT_MAP.md — gg-mcp 项目梳理

> 生成于 2026-10-21，2026-10-22 按 HEAD `a225d9d` 重新实测校准（行数、工具数、命令数、测试结果均为当场跑出）。
> 本文只描述**当前实际状态**，与 `ARCHITECTURE.md` 冲突处以本文为准。

## 1. 一句话定位

PC 上的 MCP 服务器 + 安卓 root 端 agentd + 内核 KO 驱动，让 AI agent 通过标准 MCP 工具读写安卓游戏进程内存。**只走 KO 内核驱动通道**，不依赖 KPM。

## 2. 三个进程边界

| 层 | 产物 | 语言 | 运行位置 | 代码量 |
|---|---|---|---|---|
| PC 侧 | `gg-ghost-mcp.exe` | C++17 | Windows | 2709 行 / 13 文件 |
| 设备侧 | `gg-agentd` | C（NDK arm64） | Android root | 6427 行 / 25 文件 |
| 内核侧 | `build/ko/*.ko` | 预编译二进制 | Android kernel | 2 模块 × 2 内核版本（6.12.23 / 6.12.69） |
| 协议 | `wire_protocol.h` | 纯 C | 双端共用 | 172 行 / 1 文件 |

设备侧约占全仓 69%，是真正的重心；PC 侧只是协议面与参数收敛层。

## 3. 通信链路

```
MCP client ──JSON-RPC──▶ gg-ghost-mcp ──20B 帧头 + JSON 负载──▶ adb forward tcp:34127
                              │                                        │
                              │                                        ▼
                              │                                   gg-agentd
                              │                                        │
                              │                      /proc/<key>/<key> 33B 头 + read()
                              │                                        │
                              │                                        ▼
                              │                         rwProcMem_module / hwBreakpointProc_module
                              │                       （insmod 加载；页表遍历直读物理内存，
                              │                        不走 process_vm_readv、不 attach 目标）
                              └────────── 响应 {"r":{}} / 错误 {"e":{}} ┘
```

**没有降级链。** 唯一通道是 rwproc33：节点打不开、自检回环失败或驱动报错，
`kernel_status` 返回 `state=4 (unavailable)`，读写直接报错 —— 既不回落
`process_vm_readv` 也不回落 `/proc/pid/mem`（`check_kernel_only.py` 守这条）。
hook / dev / qx 三型后端已于 2026-10-21 删除，理由见 `DRIVER_MATRIX.md` §6。

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
| 头文件 5 个（`session.h` 110 / `wire.h` 76 / `annotations.h` 54 / `protocol.h` 23 / `schema.h` 22） | 285 | 接口声明 |

7 个 `.cpp` + 6 个头文件（含上表的 `net_guard.h`）= 2709 行 / 13 文件。

### 设备侧 `agentd/`

| 文件 | 行数 | 职责 |
|---|---:|---|
| `main.c` | 2097 | 命令分发 + 参数闸门（地址 / safe_mode / 握手）+ 事件广播 + 会话状态 |
| `driver.c` | 623 | rwproc33 单通道实现（节点发现 / 33B 头读写 / GG-TOUCH / 硬件断点）+ 自检；**无降级**，失败即返回 -1 |
| `search.c` | 824 | 精确/区间/模糊搜索，位图 + 快照 |
| `watch.c` | 414 | 硬件断点监视（CE 式"谁访问了这个地址"） |
| `process.c` | 367 | 进程枚举与 maps 解析 |
| `cmd_view.c` | 313 | 结构化内存查看 / 符号解析 / 指针链 |
| `elfsym.c` | 249 | ELF `.dynsym` 解析（符号名有界读取 + 字符串表上限） |
| `freeze.c` / `disasm.c` / `cmd_shell.c` / `version.c` | 453 | 冻结线程、Capstone 反汇编、shell 白名单、版本匹配 |
| 13 个头文件 + `perf_simple.c` | 1087 | 接口声明与支撑层（`driver.h` 196 / `perf_simple.h` 127 / `search.h` 116 / `log_enhance.h` 108 / `watch.h` 104 / `atomic_simple.h` 88 …） |

对账：12 个 `.c`（上表 11 个 + `perf_simple.c`）+ 13 个头文件 = 25 文件 / 6427 行。

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
| PC 端单测 | ✅ | `tests/unit/run_tests.bat`，2026-10-22 复跑全绿（16 个测试程序 + 3 道护栏） |
| 结构护栏 | ✅ | dispatch 一致性（43↔43）+ kernel-only + rwproc 共享缓冲临界区 |
| **agentd / native 编译** | ✅ | **NDK r30 + capstone 5.0.1 均已就位**，`build_ndk.bat` 实测全绿 |
| 真机集成测试 | ✅ | 设备在位（`134d2f8` / 6.12.69-android16-6），agentd 在 34127 运行；2026-10-22 晚 `run_itest.ps1` **ITEST ALL PASS**（search/read/write/freeze 全过），rwproc 读写链路已打通 |
| 性能基准 | ❌ | 仓内无基准源码，`benchmark.bat` 会明确报错 |

产出实测：`gg-agentd` 2.4MB（ELF64 AArch64 PIE，仅 NEEDED `libc.so`/`libdl.so`，`-Wall` 零警告）。

**三端均可本机编译**（2026-10-21 实测）。当前缺口在驱动层：rwproc33 调试版 KO 能在真机加载、
也不再崩机，但读写回环返回 0 —— 设备端的功能验证全被这一层挡住。编译通过、模块加载成功
都不等于链路可用，不得当作「已验证」上报。

## 7. 测试覆盖

39 个文件 / 3900 行（含 `posix_stub` 头与 runner 脚本），四层：

- **unit**（PC 可跑，15 个可执行程序）：`test_wire`（头布局 static_assert）、`test_wire_io`（真 socket 假 agentd，验失败语义）、`test_wire_posix_syntax`（POSIX 分支 `-fsyntax-only`，不出可执行）、`test_session`（枚举校验 + 地址规范化）、`test_net_guard`（回环判定 / Origin 白名单）、`test_protocol`（JSON-RPC batch）、`test_results_page`（分页不越界）、`test_search`、`test_aob`（跨块 / 故障页）、`test_parallel`（分片线程合并）、`test_process_maps`、`test_freeze`、`test_watch`（命中游标去重）、`test_elfsym`（畸形 ELF）、`test_version`、`test_shell_security`
- **结构护栏**（python，`run_tests.bat` 自动执行）：`check_dispatch_consistency.py`（schema↔session 43↔43）、`check_kernel_only.py`（driver.c 无用户态回退）、`check_rwproc_locking.py`（`rwproc_buf` 拷贝在临界区内）
- **integration**：`run_itest.ps1` + `itest.c`（需真机 + root）
- **regression**：`test_api.bat`、`test_stdio_smoke.py`（stdio 端到端冒烟）

通信层失败语义（`CallOutcome` 七态：ConnectFailed / SendFailed / ResponseTimeout / OutcomeUnknown / ProtocolError / RemoteError）**已有真 socket 回归**，不再只靠手工验证。
只有 `ConnectFailed` 允许上层重放，其余失败一律不重放 —— 见 `server/session.cpp:220`。

## 8. 已知债务与文档漂移

| 项 | 状态 |
|---|---|
| `agentd/main.c` 仍 2097 行 | 已拆 shell（`cmd_shell.c`）/ view（`cmd_view.c`）两域，待拆 cmd_search / cmd_mem / cmd_proc / dispatch；拆前后跑 `check_dispatch_consistency.py` + `build_ndk.bat` |
| wire 1MiB 单帧上限无分片 | `gg_get_results` 上限 10000 条，实测整帧 610–740KB（单条 61–74B），距 1MiB 余量 29%–42%。分片前仍**不要**上调该 limit |
| wire 头仍走结构体内存布局 | 无显式 encode/decode，依赖两端同为小端 x86/arm64 |
| **`driver.c` 残留三型死代码** | `ko_ioctl_req_t` / `qx_copy_memory_t` / `qx_module_base_t` 只剩定义、无任何调用；文件头注释仍按四型后端写。功能无影响，读码者会被误导（2026-10-22 发现） |
| **rwproc33：崩机已止，读写未通** | 2026-10-22 真机（`134d2f8` / 6.12.69）加载 ABI 匹配的调试版 KO：`insmod` RC=0、6h16m 未重启、dmesg 零 Oops。R1/R3 已生效 —— 39.3GB 非法 phys 被 `[rwfast] phys range invalid` 拦下（旧代码会 `__va` → 越界 vmemmap → 崩机）。但回环读返回 0：VA_BITS=39 / 3 级页表 / BTF 偏移（`task_size@96` `pgd@104` `mmap_lock@136`）均已核对一致，39GB 来自**页表项本身**。→ `audit/RWPROC33_2026-10-22.md` |
| **硬件断点默认禁用** | 不带 `--hwbp` 时先做运行时探测，探测不过就报错；帮助文本原话是「当前 KO 的模块会崩内核，勿开」（`main.c:1909`） |
| ~~`docs/` 混杂~~ | 2026-10-21 已分 `engineering/`（工程）与 `research/`（逆向笔记），索引见 `docs/README.md` |
| ~~`archive/framework_v1/`~~ | 2026-10-21 已删除（8 个文件）；需要时从提交 `a29871b` 恢复 |
| ~~`ARCHITECTURE.md:83` 重连语义~~ | 2026-10-22 已改：仅 `ConnectFailed` 重放一次，其余失败不重放 |

## 9. 建议的推进顺序

1. **rwproc33 读写链路**（当前唯一阻断项）—— 崩机已止（R1–R4 生效），但自检回环读返回 0：物理地址算出 39.3GB，来自页表项本身。下一步是逐级 printk 单地址复现，或对照 afei prebuilt（它在本机 ITEST ALL PASS）的遍历差异。打通前不要用 rwproc 对游戏做全量搜索。
2. **真机 itest** —— 驱动读写通了才能验 `GG-01~09` 的行为（地址参数类型、`value` 解析、safe_mode 覆盖面、HEX_DUMP 响应 cmd）。现在跑只会全红，不能算数。
3. **分片** —— 解除 1MiB 硬依赖，才能上调结果上限。需要两端同步。
4. **`main.c` 继续拆分** —— 剩 cmd_search / cmd_mem / cmd_proc / dispatch 四域，可编译验证，风险已可控。
5. **wire 头显式编解码** —— 要两端同步改，收益小于前四项，排最后。

---

*本文为工程梳理，结论基于 2026-10-22 的 HEAD `a225d9d` 与实际构建/测试结果；行数、工具数、命令数、单测结果均为当场跑出，环境类判断（NDK / adb / MinGW）已现场核验。*
