# GG-MCP 源码梳理报告

- 生成时间：2026-10-23 19:0x（GMT+8），19:30 按修复结果更新
- 方法：**只读源码**（`server/` + `agentd/` + `shared/` + `tests/` + `scripts/`），未依赖 `docs/` 下任何文档
- 实测动作：`scripts/build_pc.bat`、`scripts/build_ndk.bat`、`tests/unit/run_tests.bat`、stdio 冒烟、`adb devices`、`git log/status`
- 代码规模：**11,986 行**（server 全量含 `device/`、`mcp/`；agentd；shared。不含 tests 与 third_party）
- 文末 §九 是修复记录：哪些已修、哪些只做缓解、哪些有意不改及理由

---

## 一、这是什么

一句话：**PC 上的 AI（MCP 客户端）通过 `gg-ghost-mcp` → adb forward → 设备端 `gg-agentd` → 内核 LKM（rwproc33）这条链路，直接读写 Android 目标进程内存。**

它不是一个"游戏助手"，而是一套完整的**内存逆向工作台**：搜索/收敛/定位/反汇编/硬件断点/指针链/符号解析/冻结/虚拟触摸/屏幕差分，全部以 MCP 工具形式暴露给 AI。

### 1.1 关键数字

| 项 | 数值 | 出处 |
|---|---|---|
| MCP 工具数 | **52** | `server/mcp/schema.cpp` |
| 分发表项数 | **52**（集合与 schema 完全一致） | `server/session.cpp` `kTools[]` |
| wire 命令数 | **46**（`GGCMD_MAX = 47`） | `shared/wire_protocol.h` |
| 其中占位/已删 | 5（`LOAD_GG`/`CORE_VERSIONS`/`SUBSCRIBE`/`UNSUBSCRIBE`/`RELAY`） | 同上 |
| 单元测试源 | 17 个 `.c/.cpp` + 5 个 Python 结构护栏 | `tests/unit/` |
| 搜索并行度 | 4 线程，按 region 均分 | `agentd/search.c` |
| 单帧上限 / 重组上限 | 1 MiB / 8 MiB | `wire_protocol.h` |
| 模糊快照上限 | 256 MB | `search.h` |
| 结果集上限 | 100,000（可配） | `GG_RESULT_CAP_DEFAULT` |
| 冻结表容量 | 256 | `GG_FREEZE_MAX` |

---

## 二、整体架构

```
┌──────────────────────────────────────────────────────────────┐
│ MCP 客户端（Claude Code / Cline / 自研）                      │
│   stdio 或 HTTP POST /mcp（bearer + Origin 校验）             │
└───────────────────────┬──────────────────────────────────────┘
                        │ JSON-RPC 2.0
┌───────────────────────▼──────────────────────────────────────┐
│ server/  gg-ghost-mcp.exe   (~3.5k 行 C++17, MinGW)          │
│  main.cpp      双传输 + 配置校验                               │
│  mcp/protocol   initialize/tools.list/tools.call/批量         │
│  mcp/schema     52 工具 JSON Schema（对外唯一事实源）           │
│  session.cpp    51 handler（表驱动）+ 参数规范化 + safe_mode    │
│  device/wire    AgentClient：帧编解码 + 重连重放 + 分片重组     │
│  annotations    本地标注库（JSON 持久化，跨会话记忆）           │
│  shared_ctx.h   与 gg-ui 的文件 IPC（context/cmd/result）       │
└───────────────────────┬──────────────────────────────────────┘
                        │ TCP 127.0.0.1:34127（adb forward）
                        │ 20B 头 + JSON，XOR 混淆，AUTH 门
┌───────────────────────▼──────────────────────────────────────┐
│ agentd/  gg-agentd  (~8.6k 行 C, NDK aarch64 PIE, root)      │
│  main.c        dispatch / 握手门 / safe_mode 闸门 / 连接生命周期 │
│  wire_io.c     send_ok/send_err + 分片 + 全局发送锁            │
│  wire_auth.c   常量时间比较 + 临时密钥落盘                      │
│  cmd_proc      session_guard(pid 复用自愈) / attach / maps     │
│  cmd_search    搜索域 handler                                  │
│  cmd_mem       读写/冻结/暂停                                  │
│  cmd_view      view_memory / resolve_symbol / pointer_chain     │
│  cmd_analyze   disasm / watch(四合一) / aob_resolve / ptr_scan  │
│  cmd_status    快速通道（无锁）状态查询                         │
│  cmd_shell     白名单 argv 直执行（fork+execvp，不经 sh）        │
│  cmd_touch     内核虚拟触摸                                     │
│  screen.c      screencap raw → ROI 灰度差分                    │
│  search.c      搜索引擎（精确/区间/AoB/模糊/过滤/直方图/指针）   │
│  driver.c      rwproc33 通道（33B 头 read/ioctl + 匿名 fd）      │
│  watch.c       硬件断点（驱动 perf_event + ptrace 兜底）        │
│  freeze.c      100ms 全量重写循环 + pid 复用守卫                │
│  process.c     /proc 解析 + maps 会话缓存(5s)                  │
│  elfsym.c      ELF64 .dynsym/.symtab 最近符号                   │
└───────────────────────┬──────────────────────────────────────┘
                        │ read/ioctl 33B 头协议
┌───────────────────────▼──────────────────────────────────────┐
│ 内核 LKM：rwNova33（内存，CMD 1..16）+ rwNova（断点，CMD 1..15）│
│  源码在 build/rwProcMem33/（未纳入 git，见风险 R3）              │
└──────────────────────────────────────────────────────────────┘
```

**关键设计取向**（贯穿全部源码注释）：

1. **只走内核通道**。`driver_read/write` 驱动失败即报错，**不回落** `process_vm_readv` / `/proc/pid/mem`。`check_kernel_only.py` 是这条纪律的自动护栏。
2. **显式失败优于静默降级**。参数非法一律 `GGERR_BAD_REQUEST`，不"猜一个默认值继续"（`norm_type` 返回 -1、`put_type` 返回 false 都是这个原则）。
3. **协议 ABI 不可回收**。删掉的命令保留枚举值（28/29、37/38/39），新命令一律追加在 `GGCMD_AUTH` 之后。

---

## 三、通信协议（`shared/wire_protocol.h`，223 行）

### 3.1 帧格式

```c
typedef struct {          // 20 字节，PC(C++) 与设备(C) 共用同一头文件
    uint32_t magic;       // 0x47474D43 "GGMC"
    uint16_t ver;         // 1
    uint16_t cmd;         // GGCMD_*
    uint32_t seq;         // 请求/响应配对
    uint32_t flags;       // REQ/RESP/ERR/EVENT/ENC/MORE
    uint32_t payload_len; // ≤ 1 MiB
} gg_wire_header_t;
```

负载约定：请求 `{"p":{...}}`、成功 `{"r":{...}}`、错误 `{"e":{"code","message"}}`。

### 3.2 四个机制

| 机制 | 实现位置 | 要点 |
|---|---|---|
| **AUTH 握手** | `agentd/wire_auth.c` | 未认证连接只放行 `PING`/`AUTH`；`ct_equal` 常量时间比较；无 key 时生成临时 key 落盘 `/data/adb/GG-Ghost/wire.key.ephemeral`(0600) 并复用；`/dev/urandom` 不可读则拒绝启动（fail-closed） |
| **XOR 混淆** | `gg_xor_payload` | 滚动 XOR，seed 固定 `0xA5`；响应恒带 `GGWF_ENC`，请求**自适应**（收到 ENC 响应后才加密，兼容旧 agentd）；`enc_on_` 连接级归零 |
| **大响应分片** | `wire_io.c` / `wire.cpp` | AUTH 带 `"frag":1` 才分片；同 seq 复用；**必须逐片独立 XOR**；整组在同一把 `g_send_mtx` 内发完；超限且不支持分片 → 明确回 `GGERR_CAP` 而非静默不发 |
| **事件流** | 已整体删除 | `SUBSCRIBE`/`UNSUBSCRIBE`/`RELAY` 现在回 `"command removed: events subsystem was deleted"`；`session.cpp` 的 `relay_annotate` 是空实现 |

### 3.3 命令表与工具映射（52 工具 → wire）

| MCP 工具 | wire 命令 | 设备端 handler |
|---|---|---|
| gg_list_processes / gg_attach / gg_detach / gg_target_info / gg_module_list | LIST_PROCESSES / ATTACH / DETACH / TARGET_INFO / MODULE_LIST | `cmd_proc.c` |
| gg_search / gg_search_range / gg_search_unknown_init / gg_search_refine / gg_search_undo / gg_filter_results / gg_result_count / gg_get_results / gg_clear_results / gg_set_results | SEARCH / SEARCH_RANGE / UNKNOWN_INIT / REFINE / REFINE(op=undo) / FILTER_RESULTS / RESULT_COUNT / GET_RESULTS / CLEAR_RESULTS / SET_RESULTS | `cmd_search.c` |
| gg_screen_diff / gg_screen_watch_refine | SCREENSHOT_ROI（+ REFINE） | `screen.c` + `cmd_search.c` |
| gg_read / gg_read_bytes / gg_hex_dump / gg_write / gg_freeze / gg_unfreeze / gg_freeze_results / gg_pause / gg_resume | READ / READ_BYTES / HEX_DUMP / WRITE / FREEZE / UNFREEZE / FREEZE_RESULTS / PAUSE / RESUME | `cmd_mem.c` |
| gg_view_memory / gg_resolve_symbol / gg_pointer_chain | VIEW_MEMORY / RESOLVE_SYMBOL / POINTER_CHAIN | `cmd_view.c` |
| gg_module_base / gg_calc_offset / gg_aob_resolve / gg_pointer_scan / gg_disassemble | MODULE_BASE / CALC_OFFSET / AOB_RESOLVE / POINTER_SCAN / DISASSEMBLE | `cmd_analyze.c` |
| gg_watch_set / poll / inject / clear | WATCH（`op` 字段四合一） | `cmd_analyze.c` `h_watch` |
| gg_self_test | SELF_TEST | `cmd_proc.c`（此前唯一无 MCP 入口的 wire 命令） |
| gg_kernel_status / gg_device_info | KERNEL_STATUS / DEVICE_INFO | `cmd_status.c`（**无锁快速通道**） |
| gg_status | KERNEL_STATUS（PC 端本地包装） | `session.cpp::local_status` |
| gg_shell | SHELL | `cmd_shell.c` |
| gg_ghost_tap / gg_ghost_swipe | GHOST_TAP / GHOST_SWIPE | `cmd_touch.c` |
| gg_annotate_* (5 个) | **不走 wire** | `server/annotations.cpp` 本地 |
| gg_context / gg_ui_command | 本地 + RESULT_COUNT | `server/shared_ctx.h` 文件 IPC |

**未暴露给 MCP 的 wire 命令**：`GGCMD_SELF_TEST`（只能直连 wire 触发，PC 端 0 处引用）。

---

## 四、PC 端实现要点（`server/`）

### 4.1 入口与传输（`main.cpp`，230 行）

- 配置两遍解析：先扫 `--config`，读文件，再用命令行覆盖 → 参数优先。
- 配置校验原则：**文件缺失可用默认值继续；字段存在但不合法直接失败**（`http_port`/`agent_port` 范围、`max_save_bytes > 0`）。
- HTTP 模式两条硬防线（`net_guard.h`，可单测）：
  - `host_is_loopback()` 用 `inet_pton` 判定，覆盖 `::`、`[::]`、网卡地址 —— 非回环且无 token → **拒绝启动**。
  - `origin_allowed()` 拒 `Origin: null`，且前缀比较后必须紧跟 `:` 或 `/`（挡 `http://127.0.0.1.evil.com`）。
- `set_payload_max_length(8MiB)`：httplib 默认 `SIZE_MAX`，不设就是 OOM 面。

### 4.2 协议层（`mcp/protocol.cpp`，149 行）

- 错误分层：`-32700` 非 JSON / `-32600` 缺字段 / `-32601` 方法不存在 / `-32602` 参数非法 / `-32603` 内部异常（**不回传堆栈与路径**）。
- 支持 **JSON-RPC 批量数组**：逐条处理、通知不出现在响应里、整批通知不回复。早期整批拒绝会让并发合并成一个 batch 的客户端挂死。

### 4.3 工具分发（`session.cpp`，1230 行 —— 全仓最大 PC 文件）

- **表驱动**：`kTools[] = {name, fn, write_op}`。`write_op` 是 `safe_mode` 拦截的**单一数据源**（本地拦一次，设备端再拦一次 = 双保险）。
- **参数规范化层**（这是这个文件最有价值的部分）：
  - `parse_addr`：拒负号（`strtoull("-1")` → `0xFFFF...` 且不置 ERANGE）；统一转 `"0x..."` 字符串下行，避免 JSON double 在 >2^53 时丢精度。
  - `parse_len`：显式值必须 1..max，缺省才用默认 —— 拒绝"传 0 或字符串被静默改成默认"。
  - `parse_flag01`：只认 0/1/bool/"0"/"1"。
  - `norm_type/region/op`：缺省走默认，**显式非法返回 -1 由调用方报错**。
- `device_call` 的**重放语义**：只在 `CallOutcome::ConnectFailed`（一个字节都没发出去）时重连重放一次；`SendFailed`/`ResponseTimeout`/`OutcomeUnknown` 一律不重放，防止 `gg_write` 执行两次。
- 并发：`call_tool` 全程持 `mtx_`。设备端本来就单连接串行，这把锁不额外增加代价，但**同时意味着 HTTP 并发会被完全串行化**（见风险 R6）。
- `save_to` 落盘：路径收敛在 `export_dir` 内（拒绝对路径/盘符/`..`/控制字符），先写 `.part`、全段成功才 rename，失败给 `complete=false` + `missing[]` 区间。

### 4.4 wire 客户端（`device/wire.cpp`，639 行）

平台差异处理得相当扎实（都是踩过坑的痕迹）：

| 坑 | 处理 |
|---|---|
| Winsock `SO_RCVTIMEO` 是 DWORD 毫秒，不是 timeval | 传 timeval 只取前 4 字节 → 实际 300ms，慢搜索必超时 |
| Winsock `select` 可写 = 连上 **或** 被拒 | 必须复核 `SO_ERROR`，否则失败被判成功、adb forward 自愈分支走不到 |
| POSIX 重复 `connect` → `EISCONN` | 分平台各调一次 |
| `SIGPIPE` 杀进程 | `MSG_NOSIGNAL` |
| 整体截止时间 vs 单次 recv | `gg_remain_ms(deadline)` 动态压 socket 超时，防"对端持续吐杂散帧"拖死请求 |

### 4.5 另外两个本地子系统

- **标注库**（`annotations.cpp`）：原子写（`.tmp`→rename）、损坏文件先备份 `.corrupt` 再空库启动。`context_text()` 生成给 AI 的纯文本注入块。
- **UI 联动**（`shared_ctx.h`）：与 `gg-ui` 通过 `build/gg-shared/{context,cmd,cmd_result}.json` 三文件通信，`ts` 3 秒判活，200ms 轮询。命令限定"看/定位"类，**不含 write/freeze**（改内存必须走 `gg_write` 那条有审计的路径）。

---

## 五、设备端实现要点（`agentd/`）

### 5.1 dispatch 的三道闸门（`main.c`，667 行）

顺序是刻意的，不能换：

```
payload 解析 → ① AUTH 门（未认证只放行 PING/AUTH）
            → ② 地址闸门（addr/base/pc/fp/sp/lr 统一 parse_u64_strict，
                             JSON 数字就地规范化为 "0x..." 字符串）
            → ③ safe_mode 闸门（单一破坏性命令清单）
            → ④ 快速通道分流（PING/KERNEL_STATUS/LIST_PROCESSES/DEVICE_INFO 不持锁）
            → ⑤ lock(g_eng.mtx) → session_guard() → switch
```

- **② 为什么必须"就地改写"而不只是"校验通过"**：下游 handler 一律按 `->valuestring` 取地址，而 cJSON 数字节点的 `valuestring` 恒 `NULL`。放行不转换 = 把 NULL 交给 `strtoull`（`gg_watch_inject` 曾因此段错误）。
- **③ 清单含 `GHOST_TAP/SWIPE`**：虚拟触摸能替用户点掉授权弹窗，属"改变外部状态"，早期漏列导致 `--safe` 自称只读却仍能注入输入。
- **④ 快速通道的意义**：大进程搜索会持锁数十秒，若探活也要排队，外部看到的是"假死"。

### 5.2 会话自愈（`cmd_proc.c`）

- 身份 = `(pid, process_starttime)`。判据只用 starttime，因为 comm 可改、uid 可换、maps 可雷同。
- `session_guard()` 每请求在锁内执行；不一致即 `session_drop()`（停 watch、停 freeze、清结果集、失效 maps 缓存、清 session_*）。
- `attach` 失败**也必须清旧会话** —— 否则调用方以为切了目标，实际读写还落在上一个进程（实测过）。
- `freeze_loop` 每轮（100ms）独立再查一次 starttime，不只依赖 dispatch。

### 5.3 锁与共享缓冲矩阵（改代码时最需要背下来的表）

| 资源 | 保护者 | 谁会并发碰它 | 失控后果 |
|---|---|---|---|
| `g_eng`（会话/结果集/引擎） | `g_eng.mtx`（dispatch 全串行） | 只有 handler | 结果集/会话错乱 |
| `rwproc_buf`（进程级 33B+256KB 静态缓冲） | `rwproc_mtx` | ① 搜索 worker 线程池 ② freeze 线程 ③ 主 handler | 读到别的地址的数据、句柄错位（**删锁 = 数据破坏**） |
| 出站帧 | `g_send_mtx`（全局串行） | 多连接线程 | header/payload 交错，接收端按长度读必错位 |
| 冻结表 | `freeze.mtx` | freeze 线程 + handler | 表项撕裂 |
| 屏幕差分基线 | `screen_mtx_`（PC 端） | 独立于 `mtx_` | 差分与工具调用互相等 |
| `search_refine` / `pointer_scan` 的 `static chunk` | 隐式依赖 `g_eng.mtx` | 无显式锁 | 一旦 handler 移出锁立刻互相覆盖 |
| `process.c` maps 缓存 | `g_maps_mtx` + TTL 5s | 多 handler | 抖动（非正确性问题） |

### 5.4 搜索引擎（`search.c`，1424 行 —— 全仓最大文件）

三种模式共用一个 worker（`search_worker_t`，`mode` 0/1/2），4 线程按 region 均分，各 worker 本地结果数组 → join 后合并。

**精确搜索**：`memchr` 首字节预筛 + `carry` 跨块（块尾起始的匹配不会被漏）。  
**区间搜索**：整数类型走 `u64` 比较（避免 `read_as_double` 在 >2^53 丢精度）；**默认按自然对齐扫描**（逐字节会让一个 8 字节值命中 8 次，结果虚高 8 倍）。  
**AoB**：支持 `??` / `8?` / `?B` 三种通配（mask 逐位），无通配走 `memcmp`。

**性能核心 —— pagemap 驻留筛选**（2026-10-23 的关键优化）：

- 病灶：`rw-p` 堆里大量页"映射了但从未 touch"，驱动读失败，`gg_fault_advance` 每次只前进一页 → 一次全量搜索 25 万次 syscall，平均每次只读到 0.21 页。
- 解法：`/proc/<pid>/pagemap` 的 present 位（bit63），一次 `pread` 8 字节/页、最多 64 页（256KB），把"逐页探测"变成"按驻留段整块读"。
- 为什么不用"连续失败就大步跳过"：会过冲混合 region 末端的可读页，**违背"两端可读都找到"**。pagemap 不猜，只跳确定 absent 的页，因此不漏。
- 打开失败（`pmfd < 0`）退回逐页探测；Windows 单测跑的就是这条 fallback 分支 —— 是"测 fallback"，不是"假实现骗测试"。

**模糊搜索**：位图（1 bit/元素）+ 全量快照（上限 256MB）。refine 逐元素比较并**就地更新快照**（合并双读，每轮只读一遍全量内存）。两个关键取舍写在注释里：

- 读失败的段落**必须清位图**（否则这些元素永远是候选，`remaining` 卡死；真机实测卡在 5300 万）。
- **短读不能清**（rwproc 遇不可驻留页短读返回，下一轮从 `off+got` 还能读到；清掉就是误杀可读元素，`unchanged` 直接归零）。

**撤销栈**：模糊存位图副本（256MB 区 ≈ 8.4MB/级）、精确存 `addrs+vals`（≤1.6MB/级），深度都是 3，两套独立。

**结果集内过滤 / 直方图**：`search_filter_exact` 按**当前值**原地压缩（可与 refine 交替使用）；`results_value_histogram` 输出 top-8 + `unique_exact` 标志（桶溢出替换后唯一值数只是下限）。读不到的条目直接剔除/不计入，避免"0 出现两万次"这种假结论进 AI 上下文。

**指针扫描**：目标去重排序（上限 2 万）→ 可写区扫描 → 精确用 `bsearch`、`lookback` 模式用二分找最近邻；`aligned` 默认开。同样接入了 pagemap 跳过（2026-10-23 补，此前漏了这条路径：微信 585MB 扫 2852 个目标耗 11.7s，90% 在读不存在的页）。

### 5.5 内核通道（`driver.c`，784 行）

- 协议：`read(fd, buf, 33 + data_size)`，33B packed 头 `{cmd, p1, p2, p3, buf_size}`；结果回写在 `buf+33`。**编译期 4 条 `_Static_assert` 锁死结构尺寸**（33 / 312 / 1053 / 288）—— 错位会毁掉整条通道。
- **匿名 fd 升级**：`CMD_ANON_GET_FD(14)` 在用户态发起，拿到 `anon_inode` fd 后命令改走 `ioctl(fd, 33, buf)`，**并关掉 proc fd**（proc fd 在 `/proc/<pid>/fd` 里会暴露节点路径）。两个模块各有自己的 anon fd，必须按 fd 归属取同一个（曾取错导致 `GET_NUM_BRPS` 返回 -14）。
- **节点类型探测**：两个模块都是 `/proc/<key>/<key>` 形状，只能靠"只有一方认识且对另一方无害"的命令区分 —— 用 `GET_NUM_BRPS(3)`：断点模块返回正数（实测 6），内存模块 cmd3 是 `READ_PROCESS_MEMORY` 空句柄 → 负数。**绝不能拿 cmd4/cmd5 试探**（那是写内存 / `put_pid`）。
- **self-test 不做硬门禁**：真机上"驱动读自身进程"会返回成功但填 0（跨进程读正常）。拿它判死整条通道，等于让一个自检缺陷废掉全部功能。当前口径：读到期望值 = OK；读成功值不对 = 按可用处理但留 WARN；读失败/打不开句柄 = 不可用。
- `force=1` **一律拒绝**（`driver_read_ex` 直接报错）：该内核上 `is_force_read` 会 panic（实测连续两次崩内核重启）。

### 5.6 硬件断点（`cmd_analyze.c` + `watch.c`）

- **四合一 handler**：`op = set|clear|poll|inject`。
- **命令码基准探测**（一次性，全局缓存 `g_hwbp_probe`）：自编译模块 `GET_NUM_BRPS` 返回 **6** 才放行；afei 的 0 起始 prebuilt 返回 **4**（那条其实是 `GET_NUM_WRPS`），发 `INST` 会 `put_pid((struct pid*)1)` 崩内核。失败信息分两种文案（节点没开 vs 旧模块），避免把排查方向带偏。
- **按线程注册**：驱动侧没有 `INST_ALL(13)` 实现，因此用户态枚举 `/proc/<pid>/task`，每线程走 `OPEN → INST → CLOSE`；perf_event 是 per-task 的，任务切换自动换装。注册后**立刻释放进程句柄**，否则每断点泄漏一个内核 pid 引用。
- **命中消费游标**：模块命中累计不清零且只能从向量头取（上限 1024），每 bp 一个 `rwproc_hit_consumed[]` 游标，只上报新增；`cap` 截断的剩余下次补。
- **enrich**：每条命中自动补 ① pc 所属模块 + 模块内偏移 ② FP 链回溯（≤5 层）③ pc±16 条预反汇编（ARM64 定长 4B）。
- **ptrace 兜底路径**：`NT_ARM_HW_WATCH` 设 `DBGWCR`（E|LSC=write|BAS=0xf），后台线程 `waitpid(__WALL)` 收 `TRAP_HWBKPT`。停止时先 `DETACH` 全部 tid 再 join（否则 `waitpid` 不返回 → 死锁）。

### 5.7 其它子系统

| 模块 | 关键实现 |
|---|---|
| `screen.c` | `screencap` raw 流（不用 `-p`：PNG 编码 1.02s / raw 0.18s）；只回传 ROI 灰度（BT.601 整数近似）；ROI 过大自动抬 `scale`，保证 `sw*sh ≤ 8192`；非 fmt=1 直接拒绝（像素步长会错） |
| `freeze.c` | 100ms 全量重写；`active/last_err` 逐条标记；`ESRCH` 自动停条目；启动前 `thread_started` 防孤儿线程 |
| `cmd_shell.c` | `fork + execvp` 直执行 argv（不经 `sh`，免疫 `;` `|` `$()`）；白名单 `ps/ls/cat/dumpsys/getprop/id`；读与收尸**双超时**（10s），超时 SIGKILL 并附部分输出 |
| `cmd_view.c` | `view_memory` 按 int32/float32/pointer 解码并标注目标是否映射/所属区域；`pointer_chain` 逐级解引用 + 环检测 + 标注 `at_base` |
| `elfsym.c` | 纯本地 ELF64 解析，`.dynsym` 优先退 `.symtab`；**字符串表 32MiB 上限**、符号名用有界 `strlen` —— 防"伪造 ELF 让 root 进程越界读并把堆内容回吐到 JSON" |
| `process.c` | `/proc/<pid>/stat` **从最后一个 `)` 起数第 20 个 token** 取 starttime（comm 可含空格与括号）；`process_is_64bit` 扫描全部 maps（首行常是低地址 dalvik 映射，只看首行必误判 32 位） |
| `cmd_status.c` | 无锁快速通道，整型原子读、字符串快照容忍撕裂 |
| `main.c` 伪装 | `hide_process()` 清 argv[1..] 与 environ，进程名 `dspbridge`；参数值先 `snprintf` 到独立 buffer 再清 argv（否则 dtype/key 变空串） |

---

## 六、实测结果（本次会话真实执行）

| 项 | 结果 |
|---|---|
| `scripts/build_pc.bat` | **OK** — 仅有 LTO 串行编译提示（`lto-wrapper: using serial compilation of 11 LTRANS jobs`），无编译错误 |
| `tests/unit/run_tests.bat` | **全绿** — 14 个可执行测试 + 5 个结构护栏全部通过 |
| 工具一致性护栏 | `schema 工具数 = 52` / `session 分发数 = 52` / **完全一致** |
| `check_kernel_only` / `check_rwproc_locking` / `check_hwbp_detail` / `check_scalar_cmd` | 全部 `[OK]` |
| `adb devices` | `134d2f8  device`（设备在位） |
| `git status` | 干净；HEAD = `894627c feat(screen): 屏幕区域灰度差分抓取 + 结果集内过滤` |
| `tests/integration/run_itest.ps1` | **未执行**（本次只做源码梳理，未连设备跑 itest） |

> 编译告警（`-Wall` 可见，非错误）：`agentd/watch.c:159` `snprintf` 目标 256 字节、输出可能 31–286 字节 → 可能截断 `last_error`。不影响正确性，但会让最长的那条错误信息缺尾巴。

---

## 七、风险与债务清单

### P0（影响结论有效性 / 能力缺失）

| ID | 问题 | 依据 | 建议 |
|---|---|---|---|
| R1 | **设备端结论未经真机闭环**。本次所有 agentd 侧判断都来自源码，itest 未跑 | 未执行 `run_itest.ps1` | 设备已在位（`134d2f8`），改动后必须 `deploy.bat` → `run_itest.ps1` 才算完成 |
| R2 | `force=1` 全链路禁用，`gg_read/read_bytes/view_memory` 的 `force` 参数实际是"传了就报错" | `driver_read_ex` 直接 `set_err` 返回 -1 | 要么从 schema 移除该参数（现在靠文档劝退），要么修驱动 `is_force_read` |
| R3 | **内核模块源码不在版本控制**。`git ls-files build` 只有 1 个文件（`build/ko/rwNova33_6.12.69.ko` 产物），`build/rwProcMem33/` 整个目录未跟踪 —— 而内存通道、hwbp 修复、gg-touch 全在那里 | `git ls-files build \| wc -l` = 1 | 把 `rwProcMem33Module` / `hwBreakpointProcModule` 源码纳入仓库（或独立 repo 子模块），否则 KO 侧修改无法追溯 |

### P1（会咬人，但当前有边界）

| ID | 问题 | 依据 |
|---|---|---|
| R4 | **静态共享缓冲依赖"隐式锁"**。`search_refine` / `pointer_scan` 用 `static uint8_t chunk[256KB]`，安全性完全建立在"handler 都在 `g_eng.mtx` 内"这个前提上；护栏只覆盖 `rwproc_buf`，不覆盖这两处 | `search.c:867`、`search.c:1365` |
| R5 | **单帧/重组上限是硬天花板**。`get_results(limit=10000)` 约 610–740KB，余量不大；`gg_screen_watch_refine` 一次调用内部会发多轮截图，但单响应仍受 1MiB 约束。旧客户端（不带 `frag:1`）超限只能拿 `GGERR_CAP` | `wire_protocol.h`、`wire_io.c:161` |
| R6 | **`call_tool` 全程持锁 + 同步阻塞 handler**。`gg_screen_watch_refine` 最长阻塞 60s 且持 `Session::mtx_` → HTTP 传输下所有并发工具调用被串行化最长 60s | `session.cpp:1216`、`session.cpp:458` |
| R7 | **XOR 是混淆不是加密**。seed 固定 `0xA5`（编译期常量），滚动 XOR 自逆；注释已声明"非加密安全"，但部署方容易误读 | `wire_protocol.h:63` |
| R8 | **死代码 / 半死代码**：`log_enhance.h`（无人 include）、`perf_simple.c/.h` + `atomic_simple.h`（编译进 agentd 但**零引用**，`g_perf_stats` 从未被读写）、`version_from_package()`/`version_syntactic()`（零调用，前者还 `popen dumpsys`）、`stat_fallbacks`（定义 + 上报但**从不自增，恒 0**）、`GGERR_VERSION_MISMATCH`（零使用）、`CallCtx::relay_annotate`（空实现）、`h_view_memory`/`h_shell` 在 `cmd_status.c` 里重复前置声明 | 全仓 grep 计数 |
| R9 | **`.ko` 之外的注释漂移**：`session.cpp` 头部写"45 个 MCP 工具"、`run_tests.bat` 注释写"45-tool"，实际 **51**；PC 端 `SEG = 65536 // 与 agentd h_read_bytes 上限一致`，而 agentd 实际上限是 `GG_SEARCH_CHUNK = 256KB` —— PC 主动砍到设备能力的 1/4 | `session.cpp:1`、`session.cpp:690`、`cmd_mem.c:67` |
| R10 | **watch.c 编译告警**（snprintf 截断） | `run_tests.bat` 输出 |

### P2（可接受，但值得知道）

| ID | 问题 |
|---|---|
| R11 | `search_range` 的整数下界 `(uint64_t)vmin` 对负数会回绕（`search_filter_exact` 有 `vmin > 0` 钳制，range 没有）→ 负下界对无符号类型静默返回 0 条 |
| R12 | maps 缓存是**单 pid 单槽**（TTL 5s）：多客户端交替 attach 会互相踢缓存，只带来抖动不带来错 |
| R13 | 冻结 256 槽 × 10 次/秒 = 2560 次驱动写/秒；freeze 与 search 共用 `rwproc_mtx`，满载会互相拖慢 |
| R14 | agentd 连接上限 8；AUTH 前只放行 PING/AUTH —— 本地进程可占满 8 个未认证连接（拒绝服务面，非提权面） |
| R15 | `main.c` 里 `hide_process()` 调了两次（588 / 635），第二次多余但无害 |
| R16 | `dispatch` 中 `case GGCMD_AUTH` 分支实际不可达（前面握手门已 return），仅为消除 `-Wswitch` 保留 |
| R17 | pagemap 驻留判定只在真机生效；Windows 单测跑的是 fallback 分支 —— "present 位准确"这条只在真机 itest 里被验证过 |
| R18 | `GGCMD_SELF_TEST` 无 MCP 入口，只能直连 wire 触发，实际处于"半废弃"状态 |

---

## 八、结论

**这套代码的成熟度明显高于“能跑就行”的阶段**：12k 行里注释密度极高，且几乎每条重要注释都对应一个**真实踩过的坑**（Winsock timeval 被截断、cJSON 字符串节点 `valueint` 恒 0、`--safe` 被 `memset` 抹掉、分片必须逐片 XOR、`SESSION` 只能靠 starttime 判定……）。参数校验、失败语义、协议 ABI 兼容性三条线都做到了"宁可显式报错，也不静默换个操作"。

真正的短板集中在三处，且都不是算法问题：

1. **内核模块源码没进版本控制**（R3）—— 这是唯一的"结构性"风险。整套系统的地基（rwNova33 / rwNova）在仓库外，一旦丢失无法重建。
2. **验证闭环缺口**（R1 + R17）—— 设备在位，但设备端行为只有在 itest 里才被真正验证；源码梳理能证明"逻辑正确"，不能证明"真机正确"。
3. **能力天花板**（R2 + R5 + R6）—— `force` 永久缺失、单帧 1MiB、长阻塞工具串行化，三者都会在"大内存 / 大结果集 / 并发使用"时先撞墙。

优先修的三件事，按投入产出排序：**① 把 KO 源码纳入 git；② 把 `force` 参数从 schema 摘掉（或修驱动）；③ 把 PC 端 `SEG` 从 64KB 提到 256KB（设备端本来就支持，白捡 4 倍 dump 速度）。**

---

## 九、修复记录（2026-10-23 19:30）

### 9.1 已修

| ID | 处置 | 落点 |
|---|---|---|
| R2 | `force` 从 4 个工具的 schema 移除；PC 端收到 `force=1` 显式拒绝（`force_guard`），不再下行到设备端才失败 | `schema.cpp`、`session.cpp`、`test_session.cpp`(f2 断言改为"拒绝") |
| R3 | KO 源码以**快照**入库 `ko/rwProcMem33/`（184 个源文件 + 说明文档）；配套 `scripts/check_ko_src_sync.py` 检测快照与工作副本漂移，`scripts/sync_ko_src.py` 单向同步；护栏已接入 `run_tests.bat` | `.gitignore`、`ko/`、两个新脚本 |
| R4 | `search.c` 两处 `static uint8_t chunk[256KB]` 改堆分配，去掉"隐式依赖全局锁" | `search.c`（`search_refine`、`pointer_scan`） |
| R6 | `gg_screen_watch_refine` 的 `timeout_ms` 上限 60s → **10s**，并把"阻塞期间其他调用排队"写进 schema 与注释 | `session.cpp`、`schema.cpp` |
| R8 | 删 `log_enhance.h`、`perf_simple.c/.h`、`atomic_simple.h`、`version_from_package`、`version_syntactic`、`stat_fallbacks`（含 JSON 字段）、`CallCtx::relay_annotate`、`cmd_status.c` 重复前置声明；`GGERR_VERSION_MISMATCH` 保留枚举加注释；另删已失效的 `scripts/test_compile.bat`（只编 11 个源文件，缺 9 个，早就不可能链接通过） | 多文件 |
| R9 | save_to 分段 64KB → **256KB**（= `GG_SEARCH_CHUNK` = `RWPROC_MAX_IO`）；hex 模式保持 64KB 但注释改为"有意收紧"（1 字节 → 2 字符，256KB 会灌 512KB 文本进对话）；工具数量注释不再写死（45/43 → 由护栏统计） | `session.cpp`、`schema.cpp`、`run_tests.bat`、`check_dispatch_consistency.py` |
| R10 | `watch.c` 两条含 `last_error` 的 `snprintf` 加 `%.200s`，消除截断告警 | `watch.c` |
| R11 | `search_range` 整数路径负下界钳到 0、`max<0` 视为空区间；上界按 2^64 饱和 | `search.c` |
| R12 | maps 缓存单槽 → **4 槽 LRU + TTL**，避免 gg-ui 与 MCP 交替操作时互相踢缓存 | `process.c` |
| R14 | 未认证连接读超时 300s → **10s**，AUTH 成功后再放宽到 300s（`conn_arm_idle_timeout`） | `main.c`、`wire_auth.c`、`engine_internal.h` |
| R15 | 删掉 main() 里重复的 `hide_process()` 调用，并说明为什么保留的是靠前那次 | `main.c` |
| R18 | 新增 `gg_self_test` 工具，SELF_TEST 不再是"无 MCP 入口"的半废弃命令 | `schema.cpp`、`session.cpp` |

### 9.2 有意不改（附理由）

| ID | 理由 |
|---|---|
| R5 | 单帧 1MiB / 重组 8MiB 是协议设计而非缺陷。当前最大响应（`get_results` 10000 条）约 610–740KB，余量足够；真要突破得改分片窗口协议，代价远大于收益。已在 schema 与 `send_ok` 里把"超限怎么办"写明。 |
| R7 | 滚动 XOR 的 seed 固定确实可改进，但**改 seed 不提升实质安全性**：JSON 明文前缀 `{"r":{` 就是已知明文，滚动 XOR 一试即出。要真加密得上 AEAD，那是协议级改动。当前把定位说清楚（混淆≠加密）比换个 seed 更有价值。 |
| R13 | freeze 与 search 共用 `rwproc_mtx` 是**驱动侧共享缓冲**的必然结果；根治要么让驱动线程安全（改内核），要么每线程独立缓冲 + 去掉锁（要确认驱动侧无共享状态）。都不是能靠用户态改动安全完成的，留作后续。 |
| R16 | `dispatch` 里不可达的 `case GGCMD_AUTH` 删除会触发 `-Wswitch`，保留并已有注释说明。 |
| R17 | pagemap 的 present 位判定只能在真机验证（Windows 上没有 `/proc/<pid>/pagemap`）。归入 itest 待办。 |

### 9.3 本次验证

| 项 | 结果 |
|---|---|
| `build_pc.bat` | OK |
| `build_ndk.bat` | OK，且 **`-Wall` 零告警**（修复前 `watch.c` 有一条截断告警） |
| `run_tests.bat` | 全绿：14 个可执行测试 + **6 条护栏**（新增 ko 源码快照护栏） |
| 工具一致性 | schema 52 / session 52，完全一致 |
| stdio 冒烟（真实启动 exe） | `protocolVersion=2024-11-05`、52 工具、`gg_self_test` 在列、四个读工具的 `properties` 里**已无 `force`** |
| `check_ko_src_sync.py` | `OK (184 source files identical)` |

### 9.4 仍未闭环

**设备端 itest 未执行**（`deploy.bat` / `run_itest.ps1`）。本次改了 `search.c` / `process.c` / `main.c` / `watch.c` / `wire_auth.c` 五个设备端文件，编译通过只证明"能编"，不证明"真机行为正确" —— 尤其是：未认证 10s 超时会不会误杀正常客户端、4 槽 maps 缓存的淘汰逻辑、`search_range` 负下界钳制后的实际命中集。设备 `134d2f8` 在位，跑一次即可闭环。
