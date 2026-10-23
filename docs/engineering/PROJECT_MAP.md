# PROJECT_MAP.md — gg-mcp 项目梳理

> 2026-10-23 按 HEAD `8bddaf5` 重测重写。行数为 `wc -l` 实测，工具/命令数为护栏输出。
> 本文只描述**当前实际状态**，与 `ARCHITECTURE.md` 冲突处以本文为准；历史快照类文档（`audit/`、
> `OPTIMIZATION_SUMMARY.md`、`CHECKPOINT_REVIEW_REPORT.md`）里的数字是当时的，不要拿它们对现状。

## 1. 一句话定位

PC 上的 MCP 服务器 + 安卓 root 端 agentd + 内核 KO 驱动，让 AI agent 通过标准 MCP 工具
读写安卓游戏进程内存。**只走 KO 内核驱动通道**，不依赖 KPM。

## 2. 进程边界与代码量

| 层 | 产物 | 语言 | 运行位置 | 代码量 |
|---|---|---|---|---|
| PC 侧 | `gg-ghost-mcp.exe` | C++17 | Windows | 3,515 行 / 14 文件 |
| 设备侧 | `gg-agentd` | C（NDK arm64） | Android root | 8,246 行 / 29 文件 |
| 协议 | `shared/wire_protocol.h` | 纯 C | 双端共用 | 225 行 / 1 文件 |
| 内核侧 | `build/ko/*.ko` | 预编译二进制 | Android kernel | 2 模块 × 2 内核版本（6.12.23 / 6.12.69） |
| 内核源码 | `ko/rwProcMem33/**` | C | 仓库快照 | 184 个源文件（与 `build/rwProcMem33/` 同步，护栏校验） |

设备侧约占 69%，是真正的重心；PC 侧只是协议面与参数收敛层。

## 3. 通信链路

```
MCP client ──JSON-RPC──▶ gg-ghost-mcp ──20B 帧头 + JSON 负载──▶ adb forward tcp:34127
                              │                                        │
                              │                                        ▼
                              │                                   gg-agentd
                              │                                        │
                              │                      /proc/<key>/<key> 33B 头 + read()
                              │                      （首次 AUTH 后升级为匿名 inode fd + ioctl）
                              │                                        ▼
                              │                         rwNova33（内存）/ rwNova（断点）
                              │                       insmod 加载；页表遍历直读物理内存，
                              │                        不走 process_vm_readv、不 attach 目标
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
| `session.cpp` | 1245 | 全部工具 handler（表驱动 `kTools[]`）+ 参数规范化 + save_to 落盘 |
| `device/wire.cpp` | 639 | 帧收发、失败语义七态、整体截止时间、分片重组、adb forward 管理 |
| `mcp/schema.cpp` | 482 | 工具 schema（与 handler 的单一对齐点，护栏校验） |
| `main.cpp` | 230 | 入口、配置加载与校验、stdio/HTTP 双传输 |
| `annotations.cpp` | 182 | 标注库（纯本地，不经设备） |
| `mcp/protocol.cpp` | 149 | JSON-RPC 分层、错误码、批量请求 |
| `proxy-server.cpp` | 60 | 独立代理进程（与主链路无关） |
| 头文件 7 个 | 568 | `shared_ctx.h` 177 / `session.h` 115 / `device/wire.h` 77 / `net_guard.h` 60 / `annotations.h` 54 / `protocol.h` 23 / `schema.h` 22 |

`net_guard.h` 与 `shared_ctx.h` 是 header-only，前者可单测（`test_net_guard`）。

### 设备侧 `agentd/`

| 文件 | 行数 | 职责 |
|---|---:|---|
| `search.c` | 1448 | 精确/区间/AoB/模糊搜索，位图 + 快照 + 两套 undo 栈 + 过滤/直方图/指针扫描 |
| `driver.c` | 784 | rwproc33 单通道（节点发现 / 33B 头读写 / 匿名 fd / GG-TOUCH / 硬件断点）+ 自检；**无降级** |
| `main.c` | 683 | dispatch（AUTH 门 / 地址闸门 / safe_mode 闸门 / 快速通道）+ 连接生命周期 |
| `cmd_analyze.c` | 581 | disasm / watch 四合一 / aob_resolve / module_base / calc_offset / pointer_scan |
| `cmd_search.c` | 472 | 搜索域 handler + 结果集读写 + 过滤 |
| `process.c` | 422 | 进程枚举、maps 解析与 4 槽缓存、`process_starttime()` |
| `watch.c` | 416 | 硬件断点监视（驱动 perf_event 路径 + ptrace 兜底）+ 命中 enrich |
| `cmd_view.c` | 313 | 结构化内存查看 / 符号解析 / 指针链 |
| `cmd_proc.c` | 299 | 会话域（attach/detach/session_guard） |
| `cmd_mem.c` | 251 | 读写 / 冻结 / 暂停恢复 |
| `elfsym.c` | 249 | ELF `.dynsym`/`.symtab` 最近符号（有界读取 + 字符串表上限） |
| `wire_io.c` | 248 | 出站帧 + 分片 + 全局发送锁 + frag 注册表 |
| `screen.c` | 230 | screencap raw → ROI 灰度降采样（不做 OCR） |
| `wire_auth.c` | 167 | 常量时间比较、临时密钥落盘、AUTH handler |
| `freeze.c` | 162 | 冻结注册表 + 100ms 全量重写循环 + pid 复用守卫 |
| `cmd_shell.c` | 157 | 白名单 argv 直执行（fork+execvp，双超时） |
| `version.c` | 111 | 版本族匹配、KO 文件名版本提取 |
| `cmd_status.c` | 93 | kernel_status / device_info（无锁快速通道） |
| `disasm.c` | 85 | Capstone ARM64 封装 + 注释启发式 |
| `cmd_touch.c` | 68 | 内核虚拟触摸（tap/swipe） |

头文件 10 个（1,007 行）：`engine_internal.h` 237 / `search.h` 202 / `driver.h` 199 /
`watch.h` 104 / `process.h` 80 / `freeze.h` 52 / `elfsym.h` 40 / `shell_util.h` 37 /
`version.h` 35 / `disasm.h` 21。

> `engine_internal.h` 是 main.c 拆分后的**唯一跨文件声明源**。新增 handler 一律在这里声明，
> 不要在 `cmd_*.c` 里重复前置声明（拆分期留下过重复，与头文件不一致时编译器不报错）。

## 5. 工具面（52 个）

数字以 `tests/unit/check_dispatch_consistency.py` 输出为准 —— 它比对 `schema.cpp` 与
`session.cpp` 两份清单，任一新增/改名只改一边就会失败。

| 分组 | 数量 | 工具 |
|---|---:|---|
| 进程/会话 | 5 | list_processes, attach, detach, target_info, module_list |
| 搜索 | 8 | search, search_unknown_init, search_refine, search_undo, search_range, result_count, get_results, clear_results |
| 筛选/画面 | 3 | filter_results, screen_diff, screen_watch_refine |
| 读写 | 6 | read, read_bytes, hex_dump, view_memory, write, set_results |
| 冻结 | 3 | freeze, unfreeze, freeze_results |
| 地址解析 | 6 | module_base, calc_offset, aob_resolve, pointer_scan, pointer_chain, resolve_symbol |
| 反汇编 | 1 | disassemble |
| 执行控制 | 2 | pause, resume |
| 内核/驱动 | 2 | self_test, kernel_status |
| 设备/系统 | 3 | status, shell, device_info |
| 硬件断点 | 4 | watch_set, watch_poll, watch_inject, watch_clear |
| 标注（PC 本地） | 5 | annotate_add, list, get, remove, context |
| 虚拟触摸 | 2 | ghost_tap, ghost_swipe |
| UI 联动（本地） | 2 | context, ui_command |

52 个工具里 7 个纯 PC 本地实现（标注 5 + `status` + `ui_command`/`context` 读本地文件），
其余下发到 agentd。wire 枚举共 46 条，其中 5 条是占位/已删（`LOAD_GG`、`CORE_VERSIONS`、
`SUBSCRIBE_EVENTS`、`UNSUBSCRIBE_EVENTS`、`RELAY_EVENT`），有效命令 41 条 —— 命令数少于
工具数是因为多工具共用命令（`read_bytes`/`hex_dump` 共用 `READ_BYTES`，`watch_*` 共用
`WATCH`，`search_undo` 复用 `SEARCH_REFINE` 的 `op=undo`）。

## 6. 构建与验证能力（2026-10-23 实测）

| 能力 | 状态 | 说明 |
|---|---|---|
| PC 端编译 | ✅ | MinGW g++ 16.2.0，`scripts/build_pc.bat` |
| agentd 编译 | ✅ | NDK r30 + capstone 5.0.1，`build_ndk.bat`，**`-Wall` 零告警** |
| PC 端单测 | ✅ | `run_tests.bat` 全绿：15 个可执行程序 + 1 个 `-fsyntax-only` 语法检查 |
| 结构护栏 | ✅ | 6 道自动执行（见 §7），任一失败 `run_tests.bat` 直接退出 |
| 真机集成测试 | ⏳ | `run_itest.ps1`；2026-10-22 晚曾 `ITEST ALL PASS`，**10-23 这轮改动未跑** |
| 性能基准 | ❌ | 仓内无基准源码，`benchmark.bat` 会明确报错退出 |
| MCP 冒烟 | ✅ | stdio 起真进程 → `tools/list` 返回 52 项，`gg_self_test` 在列 |

产出：`gg-agentd` 2.4MB（ELF64 AArch64 PIE，仅 NEEDED `libc.so`/`libdl.so`）。

⚠️ 判设备端行为前先 `deploy.bat`：`run_itest.ps1` 用的是**自带 itest 二进制**（链 `driver.c`），
它绿了不代表设备上跑的 agentd 是新版。本轮之前就因此误判过一次。

## 7. 测试覆盖

- **unit**（PC 可跑，15 个可执行）：`test_wire`（头布局）、`test_wire_io`（真 socket 假 agentd，
  验失败语义）、`test_session`（枚举校验 / 地址规范化 / force 拒绝）、`test_net_guard`、
  `test_protocol`（JSON-RPC batch）、`test_results_page`（分页不越界）、`test_search`、
  `test_aob`（跨块 / 故障页）、`test_parallel`（分片线程合并）、`test_process_maps`、
  `test_freeze`、`test_watch`（命中游标去重）、`test_elfsym`（畸形 ELF）、`test_version`、
  `test_shell_security`。另有 `test_wire_posix_syntax` 只做 `-fsyntax-only`（本地构建走
  `_WIN32`，POSIX 超时分支编不到）。
- **结构护栏**（6 道，`run_tests.bat` 自动跑）：`check_dispatch_consistency.py`（schema↔session）、
  `check_kernel_only.py`（driver.c 无用户态回退）、`check_rwproc_locking.py`（`rwproc_buf`
  拷贝在临界区内）、`check_hwbp_detail.py`（`GET_HWBP_HIT_DETAIL` 的 `buf_size` 非 0）、
  `check_scalar_cmd.py`（标量返回命令不许走 `call_ex`）、`scripts/check_ko_src_sync.py`
  （`ko/` 快照与 `build/rwProcMem33/` 不漂移；工作副本不存在时按通过处理）。
- **integration**：`run_itest.ps1` + `itest.c`（需真机 + root）。
- **regression**：`test_api.bat`、`test_stdio_smoke.py`。

`CallOutcome` 七态（ConnectFailed / SendFailed / ResponseTimeout / OutcomeUnknown /
ProtocolError / RemoteError / Ok）有真 socket 回归；只有 `ConnectFailed` 允许重放一次。

未被任何脚本引用的遗留：`tests/wtest.cpp`、`tests/bptest.c`、`tests/watchtest.c`。

## 8. 已知债务与限制（当前）

| 项 | 状态 |
|---|---|
| 真机 itest 未跑 | 2026-10-23 的改动（`search.c`/`process.c`/`main.c`/`watch.c`/`wire_auth.c`）只有编译证据，未上机 |
| 单帧 1MiB / 重组 8MiB | 硬上限。`get_results` 10000 条约 610–740KB，余量不大；上调 limit 前先看 `GGWM_MAX_REASSEMBLED` |
| wire 负载混淆 | 滚动 XOR，seed 固定 `0xA5`。**是混淆不是加密** —— JSON 明文前缀就是已知明文 |
| `force=1` 永久禁用 | `is_force_read` 在本机内核会 panic。schema 已不声明该参数，传了直接 BAD_REQUEST |
| 冻结容量 | 256 槽，100ms 全量重写（≈2560 次驱动写/秒）；与搜索共用 `rwproc_mtx`，满载互相拖慢 |
| 多客户端共享结果集 | 设备端 `g_eng` 是单例，PC 端用会话锁串行化整次工具调用；`gg_screen_watch_refine` 阻塞会连带排队（上限已收到 10s） |
| KO 源码双份 | `ko/` 是快照、`build/rwProcMem33/` 是工作副本（含未推送提交）。漂移由护栏发现，改完须 `sync_ko_src.py` |
| pagemap 驻留筛选 | 只在真机生效；Windows 单测跑的是逐页探测的 fallback 分支 |
| 硬件断点 | 默认先探测模块命令码基准（`GET_NUM_BRPS==6` 才放行），afei prebuilt 返回 4 会被拒 |
| wire 头结构体内存布局 | 无显式 encode/decode，依赖两端同为小端 |
| 32 位目标 | 不支持，attach 直接 `GGERR_ARCH_UNSUPPORTED` |

## 9. 建议的推进顺序

1. **真机 itest 闭环** —— 设备在位即可跑，是当前唯一没有证据链的环节。
2. **精确搜索 undo 的实战验证** —— 已实现（地址数组环形栈），但只在单测里覆盖过。
3. **靶场回归** —— 登山赛车实战已抓出 4 个缺陷（refine 不可用、undo、attach 失败清会话等），
   继续在真实用例上跑比扩功能更能暴露问题。
4. **wire 头显式编解码** —— 要两端同步改，收益小于前三项。

---

*结论基于 2026-10-23 HEAD `8bddaf5` 的实际构建与测试结果；行数、工具数、护栏输出均为当场跑出。
历史快照类文档的数字不要当现状。*
