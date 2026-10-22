# PROJECT_MAP.md — gg-mcp 项目梳理

> 生成于 2026-10-21，2026-10-22 按 HEAD `1736072` 实测校准（行数、工具数、测试结果均为当场跑出）。
> 本文只描述**当前实际状态**，与 `ARCHITECTURE.md` 冲突处以本文为准。

## 1. 一句话定位

PC 上的 MCP 服务器 + 安卓 root 端 agentd + 内核 KO 驱动，让 AI agent 通过标准 MCP 工具读写安卓游戏进程内存。**只走 KO 内核驱动通道**，不依赖 KPM。

## 2. 三个进程边界

| 层 | 产物 | 语言 | 运行位置 | 代码量 |
|---|---|---|---|---|
| PC 侧 | `gg-ghost-mcp.exe` | C++17 | Windows | 2750 行 / 13 文件 |
| 设备侧 | `gg-agentd` | C（NDK arm64） | Android root | 7067 行 / 26 文件 |
| 内核侧 | `build/ko/*.ko` | 预编译二进制 | Android kernel | 2 模块 × 2 内核版本（6.12.23 / 6.12.69） |
| 协议 | `wire_protocol.h` | 纯 C | 双端共用 | 182 行 / 1 文件 |

设备侧约占全仓 71%，是真正的重心；PC 侧只是协议面与参数收敛层。

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
| `session.cpp` | 830 | 44 个工具的 handler + 参数规范化 + save_to 落盘 |
| `device/wire.cpp` | 600 | 帧收发、失败语义、整体截止时间、adb forward 管理 |
| `mcp/schema.cpp` | 352 | 工具 schema（与 handler 的单一对齐点，护栏会校验） |
| `main.cpp` | 232 | 入口、配置加载与校验、stdio/HTTP 两种传输 |
| `net_guard.h` | 60 | HTTP 暴露面判定（监听地址是否回环、浏览器 Origin），纯 inline、可单测 |
| `annotations.cpp` | 182 | 标注库（纯本地，不经设备） |
| `mcp/protocol.cpp` | 149 | JSON-RPC 分层、错误码、批量请求 |
| `proxy-server.cpp` | 60 | 独立代理进程（与主链路无关） |
| 头文件 5 个（`session.h` 110 / `wire.h` 76 / `annotations.h` 54 / `protocol.h` 23 / `schema.h` 22） | 285 | 接口声明 |

7 个 `.cpp` + 6 个头文件（含上表的 `net_guard.h`）= 2750 行 / 13 文件。

### 设备侧 `agentd/`

| 文件 | 行数 | 职责 |
|---|---:|---|
| `main.c` | 2398 | 命令分发 + 参数闸门（地址 / safe_mode / 握手）+ 事件广播 + 会话状态（含 starttime 校验） |
| `driver.c` | 623 | rwproc33 单通道实现（节点发现 / 33B 头读写 / GG-TOUCH / 硬件断点）+ 自检；**无降级**，失败即返回 -1 |
| `search.c` | 935 | 精确/区间/模糊搜索，位图 + 快照 + 模糊 undo 环形栈 |
| `watch.c` | 414 | 硬件断点监视（CE 式"谁访问了这个地址"） |
| `process.c` | 400 | 进程枚举、maps 解析、`process_starttime()` |
| `cmd_view.c` | 313 | 结构化内存查看 / 符号解析 / 指针链 |
| `elfsym.c` | 249 | ELF `.dynsym` 解析（符号名有界读取 + 字符串表上限） |
| `freeze.c` / `disasm.c` / `cmd_shell.c` / `version.c` | 556 | 冻结线程、Capstone 反汇编、shell 白名单（双超时）、版本匹配 |
| 14 个头文件 + `perf_simple.c` | 1179 | 接口声明与支撑层（`driver.h` 196 / `search.h` 138 / `perf_simple.h` 127 / `log_enhance.h` 108 / `watch.h` 104 / `last_resp.h` 56 …） |

对账：12 个 `.c`（上表 11 个 + `perf_simple.c`）+ 14 个头文件 = 26 文件 / 7067 行。

> 行数为 2026-10-22 `wc -l` 实测。此前版本的表已整体过时（`driver.c` 记的 1109 与实测差近一倍），
> 改代码后请顺手重测，别让它继续漂。

## 5. 工具面（44 个）

（分组清单见下表，各处数字以 `tests/unit/check_dispatch_consistency.py` 的输出为准 —— 它比对
`schema.cpp` 与 `session.cpp` 两份清单，任一新增/改名只改一边就会失败。）

| 分组 | 数量 | 工具 |
|---|---:|---|
| 进程/会话 | 5 | list_processes, attach, detach, target_info, module_list |
| 搜索 | 8 | search, search_unknown_init, search_refine, search_range, search_undo, result_count, get_results, clear_results |
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

44 个工具中，6 个是纯 PC 本地实现（标注库 5 个 + `status`），其余下发到 agentd；差额还来自
多工具共用同一命令（`read_bytes` / `hex_dump` 共用 `GGCMD_READ_BYTES`，`watch_*` 共用 `GGCMD_WATCH`，
`search_undo` 复用 `GGCMD_SEARCH` 的 `op=undo`）。所以 wire 命令数（42）小于工具数。

## 6. 构建与验证能力（本机实测）

| 能力 | 状态 | 说明 |
|---|---|---|
| PC 端编译 | ✅ | MinGW g++ 16.2.0，`scripts/build_pc.bat` |
| PC 端单测 | ✅ | `tests/unit/run_tests.bat`，2026-10-22 复跑全绿（16 个测试程序 + 3 道护栏） |
| 结构护栏 | ✅ | dispatch 一致性（44↔44）+ kernel-only + rwproc 共享缓冲临界区 |
| **agentd / native 编译** | ✅ | **NDK r30 + capstone 5.0.1 均已就位**，`build_ndk.bat` 实测全绿 |
| 真机集成测试 | ✅ | 设备在位（`134d2f8` / 6.12.69-android16-6），agentd 在 34127 运行；2026-10-22 晚 `run_itest.ps1` **ITEST ALL PASS**（search/read/write/freeze 全过），rwproc 读写链路已打通 |
| 性能基准 | ❌ | 仓内无基准源码，`benchmark.bat` 会明确报错 |

产出实测：`gg-agentd` 2.4MB（ELF64 AArch64 PIE，仅 NEEDED `libc.so`/`libdl.so`，`-Wall` 零警告）。

**三端均可本机编译**（2026-10-21 实测），**真机链路 2026-10-22 晚已打通**：rwproc33 的四条崩溃根因修完后，
`com.android.systemui`（341MB / 5940 VMA）全量匿名搜索 4.8s 完成、`truncated=false`、设备零 Oops ——
§十二 会让设备重启的那个操作现在正常收尾。

⚠️ 一个反复踩的坑：`run_itest.ps1` 用的是**自带 itest 二进制**（链 `driver.c`），它绿了不代表
设备上跑的 `agentd` 是新版。本轮就因此误判过一次（数字 `value` 报 `bad value`，实为设备还是旧 `gdm`）。
**判设备端行为前先 `deploy.bat` 部署新二进制。**

## 7. 测试覆盖

40 个文件 / 4051 行（含 `posix_stub` 头与 runner 脚本），四层：

- **unit**（PC 可跑，16 个可执行程序）：`test_wire`（头布局 static_assert）、`test_wire_io`（真 socket 假 agentd，验失败语义）、`test_wire_posix_syntax`（POSIX 分支 `-fsyntax-only`，不出可执行）、`test_session`（枚举校验 + 地址规范化）、`test_net_guard`（回环判定 / Origin 白名单）、`test_protocol`（JSON-RPC batch）、`test_results_page`（分页不越界）、`test_last_resp`（cmd_end 截断与 UTF-8 三种相位）、`test_search`、`test_aob`（跨块 / 故障页）、`test_parallel`（分片线程合并）、`test_process_maps`、`test_freeze`、`test_watch`（命中游标去重）、`test_elfsym`（畸形 ELF）、`test_version`、`test_shell_security`
- **结构护栏**（python，`run_tests.bat` 自动执行）：`check_dispatch_consistency.py`（schema↔session 43↔43）、`check_kernel_only.py`（driver.c 无用户态回退）、`check_rwproc_locking.py`（`rwproc_buf` 拷贝在临界区内）
- **integration**：`run_itest.ps1` + `itest.c`（需真机 + root）
- **regression**：`test_api.bat`、`test_stdio_smoke.py`（stdio 端到端冒烟）

通信层失败语义（`CallOutcome` 七态：ConnectFailed / SendFailed / ResponseTimeout / OutcomeUnknown / ProtocolError / RemoteError）**已有真 socket 回归**，不再只靠手工验证。
只有 `ConnectFailed` 允许上层重放，其余失败一律不重放 —— 见 `server/session.cpp:220`。

## 8. 已知债务与文档漂移

| 项 | 状态 |
|---|---|
| `agentd/main.c` 仍 2398 行 | 已拆 shell（`cmd_shell.c`）/ view（`cmd_view.c`）两域，待拆 cmd_search / cmd_mem / cmd_proc / dispatch；拆前后跑 `check_dispatch_consistency.py` + `build_ndk.bat` |
| wire 分片 | 已实现（`GGWF_MORE`，2026-10-22）：单帧 1MiB、重组 `GGWM_MAX_REASSEMBLED` 8MiB，AUTH 需带 `frag:1` 协商。`gg_get_results` 10000 条约 510KB，**上调 limit 前先看 8MiB 重组上限**（超限帧会被 `read_frame` 拒） |
| wire 头仍走结构体内存布局 | 无显式 encode/decode，依赖两端同为小端 x86/arm64 |
| **`driver.c` 残留三型死代码** | `ko_ioctl_req_t` / `qx_copy_memory_t` / `qx_module_base_t` 只剩定义、无任何调用；文件头注释仍按四型后端写。功能无影响，读码者会被误导（2026-10-22 发现） |
| **硬件断点默认禁用** | 不带 `--hwbp` 时先做运行时探测，探测不过就报错；帮助文本原话是「当前 KO 的模块会崩内核，勿开」（`main.c`）。要用得先用 `scripts/build_hwbp_ko.sh` 重编模块 |
| **精确搜索没有 undo** | `gg_search_undo` 只回退模糊搜索（位图环形栈 3 级，不回退数据快照）；精确搜索要另存地址数组副本，尚未实现，调用时明确报错 |
| **模糊搜索的两个使用前提** | ① `init` 要在数据稳定的时刻（菜单/暂停），目标活跃时 init 会把期间变化全算进去，收敛不到；② `refine` 的 op 必须同时认字符串与数字（直连 wire 时字符串的 `valueint` 恒 0，会让候选整体清空） |
| ~~rwproc33 全量搜索崩内核~~ | 2026-10-22 晚真机验证通过：四条根因修完 → `ITEST ALL PASS`，`com.android.systemui`（341MB / 5940 VMA）全量匿名搜索 4.8s、零 Oops。关键是**不能用 `si.totalram` 当物理地址上界**（合法 pfn 可达 39.3GB，RAM 只有 11.6GB），合法性只由 `virt_addr_valid()` 判 |
| ~~GG-U1/U2/U3/U4/U6~~ | 已修并真机验证：shell 双超时、会话 (pid, starttime) 校验、事件广播锁外发送、临时密钥、cmd_end 截断标记。详见 `AGENTS.md` 第 10–15 条 |
| ~~`docs/` 混杂~~ | 2026-10-21 已分 `engineering/`（工程）与 `research/`（逆向笔记），索引见 `docs/README.md` |
| ~~`archive/framework_v1/`~~ | 2026-10-21 已删除（8 个文件）；需要时从提交 `a29871b` 恢复 |
| ~~`ARCHITECTURE.md:83` 重连语义~~ | 2026-10-22 已改：仅 `ConnectFailed` 重放一次，其余失败不重放 |

## 9. 建议的推进顺序

1. **wire 分片** —— 现在超限能被明确拒绝，但 `get_results limit=10000` 已占 510KB，不先分片就没法上调结果上限。要两端同步改。
2. **精确搜索 undo** —— 模糊搜索能回退，精确搜索误操作只能重搜；需要保存地址数组副本。
3. **`main.c` 继续拆分** —— 剩 cmd_search / cmd_mem / cmd_proc / dispatch 四域，可编译验证，风险已可控。
4. **wire 头显式编解码** —— 要两端同步改，收益小于前三项。
5. **靶场回归** —— 登山赛车实战已抓出 4 个缺陷（refine 不可用、undo、attach 失败清会话等），继续在真实用例上跑比扩功能更能暴露问题。

---

*本文为工程梳理，结论基于 2026-10-22 的 HEAD `1736072` 与实际构建/测试结果；行数、工具数、单测结果均为当场跑出，环境类判断（NDK / adb / MinGW）已现场核验。*
