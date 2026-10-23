# DESIGN.md — GG-MCP 设计说明

> 基准：2026-10-22，HEAD `718f015`。行数、工具数、限额均为当场实测。
> **2026-10-23 补注**：限额与计数已变（工具 52、`force` 摘除、save_to 分段 256KB、
> 未认证连接 10s 超时），以 `PROJECT_MAP.md` 与本文 §限额表 的最新值为准；设计约定部分未变。
>
> 三份文档的分工，别互相替代：
> - `PROJECT_MAP.md` —— 现状（有哪些模块、多少行、什么能跑）
> - `PROTOCOL.md` / `shared/wire_protocol.h` —— 字段与命令码（唯一事实源）
> - **本文** —— 两者之间：跨组件才看得出来的约定、边界、以及"为什么非得这样"
>
> 只写读代码不容易一眼看出的东西。函数级实现看源码。

---

## 1. 边界与信任模型

| 组件 | 运行身份 | 对外暴露面 | 信任级别 |
|---|---|---|---|
| MCP client（AI / IDE 插件） | 普通用户进程 | —— | 不可信 |
| `gg-ghost-mcp.exe` | Windows 普通用户 | stdio；或 HTTP `127.0.0.1:8788/mcp` | 半可信：持有 wire key，本身不是安全边界 |
| `gg-agentd` | **设备 root** | `127.0.0.1:34127`（只绑回环） | 全权：能读写任意进程、执行白名单 shell、注入触摸 |
| KO（rwProcMem / hwBreakpointProc） | kernel | `/proc/<key>/<key>` | 全权 |

**信任根只有两个**：设备上的 root，和设备上的 wire key。PC 端的配置、safe_mode 开关都不构成安全边界 —— 谁能启动 `gg-ghost-mcp.exe`，谁就能改配置。所以真正的闸门在 agentd 一侧（§8）。

链路中没有任何一段跨网络：`adb forward` 把设备回环端口映射到 PC 回环端口，两端都只听 127.0.0.1。

---

## 2. 一次调用的完整通路

以 `gg_search`（已知值搜索）为例，五跳：

```
MCP client
  └─ JSON-RPC tools/call ──▶ gg-ghost-mcp
       ├ Session::mtx_ 加锁（整次工具调用串行）
       ├ 参数规范化（"dword"→3，地址→"0x..." 字符串）
       └ AgentClient::call(cmd=GGCMD_SEARCH, {"p":{...}})
            ├ 20B 头 + JSON 负载，XOR（若已协商）
            └ TCP 127.0.0.1:34127 ──▶ adb forward ──▶ agentd
                 ├ conn_thread 收帧 → dispatch
                 ├ 握手门 → 地址闸门 → safe_mode 闸门
                 ├ g_eng.mtx 加锁 → session_guard → h_search
                 │    └ search_exact：4 worker 并行，256KB 分块
                 │         └ driver_read → rwproc_call_ex → read(/proc/K/K, 33+N)
                 └ {"r":{...}} 或 {"e":{...}}
```

失败语义按跳分层，别混在一起看：

| 跳 | 超时 | 失败表现 |
|---|---|---|
| MCP client → PC server | 客户端自定 | JSON-RPC `error` 或 `isError:true` |
| PC server → agentd 连接建立 | `connect_timeout_ms`，默认 5s | `GGERR_NO_DEVICE`，`CallOutcome::ConnectFailed`（唯一可重放） |
| 单次请求整体 | `io_timeout_ms`，默认 **300s** | `GGERR_TIMEOUT` / `OutcomeUnknown` |
| agentd 内引擎 | 无独立超时，受上一条约束 | 业务错误码 |

`io_timeout_ms` 是**整个请求**的预算（建连 + 发送 + 收完），不是单次 `recv` 的上限。每次读写前按剩余时间重设 socket 超时（`wire.cpp:394`、`wire.cpp:445`），所以对端持续吐杂散帧也拖不过这个总预算。默认给 300s 是因为全内存搜索可以跑几十秒到几分钟。

---

## 3. 协议设计

### 3.1 帧与 ABI

```
偏移 0   u32 magic  0x47474D43 "GGMC"
偏移 4   u16 ver    1
偏移 6   u16 cmd
偏移 8   u32 seq
偏移 12  u32 flags
偏移 16  u32 payload_len
偏移 20  JSON 负载
```

头是直接按结构体内存布局发送的（`wire.cpp:392` `reinterpret_cast<const char*>(&h)`），**没有显式编码/解码**。这条隐含了三个前提，改动前必须知道：

1. 两端同为小端（x86_64 / aarch64 都满足）；
2. 编译器不插入填充（5 个字长字段自然对齐，实际 offset 就是上表）；
3. 结构体定义在 `shared/wire_protocol.h`，双端共用同一份头文件。

`tests/unit/test_wire` 用 `static_assert`/运行时偏移检查守这三条。要支持大端或异构平台，得先补 encode/decode（`PROJECT_MAP.md` 列为第 4 优先级）。

**枚举值不回收。** `GGCMD_LOAD_GG`(28) / `GGCMD_CORE_VERSIONS`(29) 对应的功能已删，但枚举值留着 —— 删掉会让后续命令码整体前移，和已部署的 agentd 错位。调用这两个码回明确的 `BAD_REQUEST`。同理，新命令只能**追加在枚举末尾**：C 枚举允许重复值且不报错，插进中间会和后面的自动值撞车却查不出来。

### 3.2 负载四态

| 方向 | 结构 | 标志 |
|---|---|---|
| 请求 | `{"p":{...}}` | `GGWF_REQ` |
| 成功 | `{"r":{...}}` | `GGWF_RESP` |
| 失败 | `{"e":{"code":n,"message":"..."}}` | `GGWF_RESP\|GGWF_ERR` |
| 事件 | `{"ev":{"type":...,"data":...,"ts":...}}` | `GGWF_EVENT`，`seq=0` |

约定：**响应的 `cmd` 必须回填请求的 `cmd`**。这条踩过 —— `hex_dump` 与 `read_bytes` 共用一个 handler，早期硬编码回填 `GGCMD_READ_BYTES`，导致 `gg_hex_dump` 的响应 cmd 与请求不一致。

### 3.3 工具与命令不是一一对应

44 个 MCP 工具 → 42 条 wire 命令。差额来源：

- 6 个工具纯 PC 本地实现（标注库 5 个 + `status`），不下发；
- 多工具共用一条命令：`read_bytes`/`hex_dump` → `GGCMD_READ_BYTES`；`watch_*` 4 个 → `GGCMD_WATCH`（靠 `op` 区分）；`search_undo` → `GGCMD_SEARCH` 的 `op=undo`。

所以加工具不一定要加命令；加命令时要想清楚它服务几个工具。

### 3.4 混淆是协商出来的，不是强制的

XOR 滚动 key（`GG_XOR_SEED + 位置`）只做"防明文一眼看穿"，不是加密。

协商方式：PC 端**收到过**带 `GGWF_ENC` 的响应后，才给后续请求加混淆（`enc_on_ = true`，`wire.cpp:458`）。目的是让新 server 能连旧 agentd（全明文）。两点必须守住：

- `enc_on_` 是**连接级**状态，重连时归零（`wire.cpp:198`）。残留 true 会让每条请求都带加密标志，对端解析全失败，表现为所有命令 `ProtocolError` 直到进程重启。
- agentd 广播事件时，XOR 必须作用在**副本**上（`main.c:186`）。同一份 payload 要发给多个订阅者，XOR 自逆，原地改会让第二个订阅者收到明文却仍带 `GGWF_ENC`，解码成乱码。

### 3.5 AUTH 门的位置

`dispatch` 里顺序是：解析 `{"p":{}}` → **AUTH 处理** → 未认证则只放行 PING → 地址闸门 → safe_mode 闸门 → 加锁分发。

放在 payload 解析之后，是因为 AUTH 本身要读 `{"p":{"key":"..."}}`。放在 safe_mode 之前，是因为认证失败时不该暴露 safe_mode、驱动状态这类引擎信息。

未认证连接的 `PING` 放行（无副作用，部署自检要用）。其余一律 `GGERR_PERM`。

密钥比较用常数时间（`main.c:88` `ct_equal`），避免按首个不同字节提前返回泄露前缀。

没配 `--wire-key` / `--wire-key-file` 时**不再放行所有本地连接**：生成 16 字节临时密钥，落 `/data/adb/GG-Ghost/wire.key.ephemeral`（0600）并复用，stderr 打印。`/dev/urandom` 不可读则拒绝启动。

### 3.6 单帧 1MiB 的推论

`GGWM_MAX_PAYLOAD = 1MiB`。这是**硬上限，没有分片**。

| 位置 | 超限行为 |
|---|---|
| PC 发送 | `write_frame` 返回 -4，`GGERR_BAD_REQUEST`，不重连不重放 |
| agentd 接收 | `payload_len > 1MiB` 直接断连（`main.c:2163`） |
| agentd 发送 | `send_ok` 序列化后先 `gg_payload_fits`，超限回 `GGERR_CAP` 错误帧（带实际字节数 + 上限 + 修法），不再静默丢帧 |

推论：`get_results` 的 `limit` 在 `main.c:1448` 硬钳 10000，因为 10000 条的 JSON 已占 510–740KB，距 1MiB 只剩 29%–42%。**在补分片之前不要上调这个 limit**，否则单帧会被对端判非法。

### 3.7 事件帧

订阅制：客户端先发 `GGCMD_SUBSCRIBE_EVENTS` 才收。PC 端严格请求-响应，不订阅则行为完全不变。

接收侧遇到 `GGWF_EVENT` 必须跳过并继续等本请求的响应（`wire.cpp:558`）；`seq` 不匹配的杂散帧也要跳过，不能错配。事件无重放、无序号语义，断了就丢 —— 它是监控通道，不是可靠消息流。

---

## 4. 并发模型

### 4.1 PC 侧：两层锁

```
Session::mtx_        整次 tools/call 持锁（含参数规范化、落盘）
  └ AgentClient::mtx_   单次 call 持锁（建连、握手、发帧、收帧）
```

为什么要两层：`AgentClient::mtx_` 只能串行化单次 `call`，保护不到 `last_error` / `last_outcome` 的跨调用读写（`std::string` 并发写是 UB），也保护不到 `device_call` 里"关连接 + 重放"会把另一个线程正在用的连接掐断。HTTP 传输下 httplib 线程池会并发进 `call_tool`，所以必须有会话级锁。

设备端本来就单连接串行，这把锁没有带来实际的额外串行代价。

注意 `local_status()` 假定调用方已持 `Session::mtx_`，内部不再加锁（`std::mutex` 非递归）。

### 4.2 设备端线程清单

| 线程 | 数量 | 职责 |
|---|---|---|
| main（accept） | 1 | 接连接、上限判定、`pthread_create` 后 detach |
| conn_thread | ≤ `GG_MAX_CLIENTS` = 8 | 收帧 → dispatch → 写响应 |
| freeze_loop | 每会话 1 | 每 100ms 写回冻结值 |
| search worker | 每次搜索 4 | 分片扫描 |

没有独立广播线程 —— `broadcast_event` 在触发它的调用者线程（通常是某个 conn_thread 的 handler）里执行。

### 4.3 锁层级与保护对象

| 锁 | 保护 | 谁持有 |
|---|---|---|
| `g_eng.mtx` | 全部引擎操作（搜索、结果集、会话、maps 缓存） | dispatch 主路径 |
| `rwproc_buf` 的 `rwproc_mtx` | 进程级共享缓冲的一次"写请求 → read → 拷出" | 搜索 worker、freeze 线程、主 handler |
| `g_send_mtx` | 单个 fd 上帧头与负载不被交错 | `send_frame` |
| `g_clients_mtx` | 订阅者注册表 | register/unregister/broadcast 快照 |
| `f->mtx`（freeze） | 冻结注册表 | freeze 线程与 handler |

**`rwproc_buf` 是全进程共享的一份缓冲，不是每调用一份。** 三处并发使用它：搜索 worker 池、freeze 线程（只持 `f->mtx`，**不在** `g_eng.mtx` 覆盖范围内）、主线程 handler。只有主线程之间是串行的。请求写入 → `read()` → 结果拷出必须在同一临界区，否则 A 线程刚 read 完还没拷走，B 线程的下一次 memset/read 就把结果覆盖 —— 表现为读到别的地址的数据、句柄错位。`tests/unit/check_rwproc_locking.py` 守这条；删掉这把锁不是性能优化，是数据破坏。

### 4.4 快速通道

`PING` / `KERNEL_STATUS` / `LIST_PROCESSES` / `DEVICE_INFO` 四条命令**不持 `g_eng.mtx`**（`main.c:2063`）。原因：大进程全量搜索可持续数十秒，期间这四条全被堵住，客户端/AI 看到的是"进程假死"。

不持锁的前提下如何保证安全：

- `stat_reads` / `stat_writes` 是 `_Atomic`，零额外开销；
- 整型状态字段用 `__atomic_load` 读；
- `last_error` 快照容忍撕裂（内容短暂不完整，但 JSON 仍合法）；
- `g_last_resp` 是 `_Thread_local`，本线程 handler 写、本线程 `cmd_end` 读。

**代价**：快速通道读到的状态可能与正在进行的引擎操作不一致（例如搜索进行中读 `result_count`）。这是刻意接受的权衡 —— 这四条命令的用途是探活和诊断，不是事务性读取。

### 4.5 踩过的阻塞与死锁

三处值得记住的：

1. **广播持锁发送** → 一个不读的订阅者让新连接 auth 卡 7.44s（`clients_register` 抢不到 `g_clients_mtx`）。现在锁内只快照 + `dup`，锁外逐条发。
2. **socket 无发送超时** → 对端不读时 `send()` 在缓冲满后永久阻塞，而 `send_frame` 持全局 `g_send_mtx`，一条僵死连接能让所有响应与广播一起停住。现在统一设 `SO_SNDTIMEO`（`conn_thread` 先设 5s，`clients_register` 又设 1s，**有效值是 1s**）。
3. **`dup` 是必需的**：广播期间原 fd 可能被连接线程 close，fd 号会被立刻复用，拿裸 fd 去写可能写到别的连接上。

---

## 5. 设备端引擎

**搜索、refine、结果集、冻结全在 agentd 内完成。** 理由：多 GB 内存扫描若跨 wire 流式传输会拖垮链路。PC 只发命令、收摘要。

### 5.1 搜索

分块 256KB（`GG_SEARCH_CHUNK`），4 线程按 region 分片（`GG_SEARCH_NTHREADS`）。

三个容易写错的点：

- **跨块匹配要留 carry**：上一块尾部 `sz-1` 字节要保留到下一块头部，否则起始于块尾的匹配会整条漏掉。三种模式（exact / range / aob）都实现了，`memmove` 到 chunk 头部后继续。
- **故障页要推进而不是中止**：`driver_read` 返回 ≤0 时按 `gg_fault_advance` 把地址推进到下一个页边界（最多推 `rd` 字节），继续扫，整个 region 不会因为一页读不到就提前结束。注意 `gg_fault_advance` 的 `fail_streak` 参数目前是 `(void)` 掉的，没有"连续失败 N 次放弃"的逻辑 —— 坏 region 会逐页走完，代价是白跑。
- **首字节预筛**：精确搜索先用 `memchr` 找首字节再 `memcmp`，比逐字节比较快一个量级。

### 5.2 模糊搜索

数据结构（`gg_results_t`，见 `search.h`）：

| 字段 | 含义 |
|---|---|
| `fuzzy_bitmap` | 候选位图，1 bit / 元素 |
| `fuzzy_snapshot` | 上一轮快照（原始字节） |
| `fuzzy_regions[]` | 全局元素索引 → 地址的映射表 |
| `fuzzy_undo[3]` | refine 前的位图副本 |

流程：`unknown_init` 全置候选 + 快照 → 游戏内改变数值 → `refine(op)` 逐元素比较收缩位图 → 候选足够少时**物化**成 `addrs` 数组。

两个固有限制（写进工具描述，不是缺陷）：

- `init` 必须在数据稳定时做（菜单/暂停）。目标活跃时 init 会把期间的变化全算进基线，收敛不到。
- `refine` 的 `op` 必须同时接受字符串与数字（直连 wire 时字符串的 `valueint` 恒 0，会让候选整体清空）。

**undo 只回退位图，不回退数据快照**（3 级，`GG_FUZZY_UNDO_DEPTH`）。快照是 256MB，每轮拷一份太贵。代价：undo 之后紧跟的 refine 拿"更早一轮的值"做基准，语义上相当于跳过一次比较 —— 对"我搜错了，退回去换个 op 再试"这个用法没有影响。

内存上限 `GG_FUZZY_MAX_BYTES = 256MB`，超出的 region 不纳入扫描。

### 5.3 结果集

- 容量 `GG_RESULT_CAP_DEFAULT = 100000`，可配。
- `get_results` 单页硬钳 10000（见 §3.6）。
- **值类型以结果集自身为准**（`exact_vt` / `fuzzy_vt`），调用方传的 `type` 只兜底。早期按调用方 type 读会读错字节数。
- 短读不能当成功；分页 OOM 必须报错而不是返回空页 —— 空页会被当成"结果已空"。
- 结果集是**会话级**的，attach/detach/目标退出都必须清（里面存的是旧进程地址）。

### 5.4 冻结

独立线程，每 100ms 把注册表内所有地址写回锁定值（`GG_FREEZE_INTERVAL_MS`），上限 256 条。

- 只持 `f->mtx`，**不在** `g_eng.mtx` 内 —— 所以它和主 handler 的并发靠 `rwproc_mtx` 兜（§4.3）。
- 写失败（ESRCH 等）自动停该条目，不让一个失效地址拖住整轮。
- 线程创建失败不致命，但必须留日志：否则冻结静默失效，调用方以为已生效。

### 5.5 watch（硬件断点）

用途：CE 式"谁访问了这个地址"。

- 断点是 **per-task** 的（perf_event），每个线程独立占硬件槽位。所以要在用户态枚举 `/proc/<pid>/task`，对每个线程做一次 `OPEN → INST → CLOSE`（`driver.c:486`）。驱动内的 `INST_PROCESS_HWBP_ALL`(13) 没有实现，不要在用户态依赖它。
- 注册完成立刻释放进程句柄：`perf_event` 已绑定到 task，模块后续按 `bp_id` 操作。不释放则每个断点泄漏一个内核 pid 引用。
- 模块**没有清除命令**，命中在断点生命周期内累计（上限 1024 条）。调用方（`watch.c`）必须按已消费数去重，`GG_WATCH_MAX_HITS = 128` 是本端缓存的容量。
- 默认**禁用**：不带 `--hwbp` 时先做运行时探测（`GET_NUM_BRPS` 返回 6 才放行；0 起始的旧模块返回 4，此时 cmd=3 其实是 `GET_NUM_WRPS`）。探测不过就报错。

---

## 6. 会话生命周期

```
none ──attach(pid/name)──▶ attached ──detach──▶ none
                             │
                             ├─ 目标退出 ──────▶ none（target_exit）
                             ├─ pid 被复用 ────▶ none（pid_reused）
                             └─ attach 失败 ───▶ none（attach_failed）
```

**身份是 (pid, starttime)，不是 pid。** `session_pid` 只是个编号：目标被杀后内核会回收它，很快分配给新进程（最典型的就是刚被 kill 又自动重启的同一个 App）。此后读写全落到无关进程上，而且**写操作会成功、不报错**。`starttime`（`/proc/<pid>/stat` 第 22 字段）在同一个 pid 的新进程上必然不同，是唯一可靠判据 —— comm / uid / maps 都可能雷同。

校验点三处：`attach` 时记录、`dispatch` 每请求进锁后 `session_guard()`、`freeze_loop` 每轮。不一致即 `session_drop()`。

不变量（改会话相关代码时逐条对）：

1. `attach` **失败也必须清旧会话**。否则调用方以为切到了新目标，后续读写还落在旧目标上。
2. `session_drop` 必须同时：停 watch、停 freeze、清结果集、失效 maps 缓存。结果集里全是旧进程地址，留着会被当成新会话结果直接读写 —— 正是要防的"错进程写"。
3. 只剩一个目标。切换 = drop + 重建，不支持多目标并行。
4. 32 位目标 `attach` 直接拒绝（`GGERR_ARCH_UNSUPPORTED`）—— 驱动协议是 64 位布局。

---

## 7. 驱动通道

### 7.1 `read()` 即请求

```
read(fd, buf, 33 + N)     一次完成「提交请求 + 收回结果」
buf[0..32]   {char cmd; u64 p1; u64 p2; u64 p3; u64 buf_size}  packed
buf[33..]    数据区（请求负载 → 内核；回写结果 → 用户态）
返回值       ≥0 = 结果长度；<0 = -errno
```

尺寸靠 `_Static_assert` 锁死（`driver.c:96`）：请求头 33、命中条目 312、init_info 1053、pt_regs 288。错位会毁掉整条通道。

**`read()` 的 count 必须 ≥ `33 + 期望回写量`，而回写量与请求负载长度不总是相等。** `OPEN_PROCESS` 发 0 字节负载却要收回 8 字节句柄：count 只给 33 时内核写不进去，随后的 `memcpy(buf+33, 8)` 拿到的是上一次请求的残留数据。症状很隐蔽 —— 读自身"成功"（残留碰巧有效），读别的进程返回 0 或 EFAULT。所以有 `rwproc_call_ex(..., resp_cap)` 把"发多少"和"能收多少"分开。

返回值语义还有一条：`r == 0` 表示成功但不报长度（`OPEN_PROCESS` / `INST_HWBP` / `HIT_COUNT` 这类），此时按 `out_cap` 拷。`r > out_cap` 判协议错位直接失败，不截断 —— 截断会把驱动异常伪装成"部分成功"。

### 7.2 句柄生命周期

- 内存模块：换 pid 时先 `CLOSE_PROCESS` 旧句柄再 `OPEN_PROCESS`；`driver_close` 兜底。
- 断点模块：`OPEN_PROCESS` 的句柄与内存模块**不通用**，是独立句柄。
- 硬件断点注册完立刻释放 pid 引用（§5.5）。

### 7.3 自检判据

`driver_self_test` 打开自身进程句柄，回环读一个静态变量 `0x12345678`。读到且值对 → `ST_DRIVER`。

**不看 `/proc/modules`。** 模块开启隐藏后 `/proc/modules` 与 `/sys/module` 里都查不到名字，重复 `insmod` 返回 `EEXIST`；驱动也可能被改名成 `apr_dlkm` 之类。按名字判既会漏判也会误判。最终是否可用只认自检回环。

### 7.4 `is_force=0` 的取舍

读写不翻转 PTE，只读 PTE 有效的页。因此**已被回收的文件页读不到**（返回 0 字节），活跃页正常。

这是为了规避"翻转 PTE 与目标进程活跃页竞争导致内核崩溃"选的。代价是偶发读空 —— `gg_read_bytes` 在 `bytes: 0` 时返回 `note` 说明原因，不让调用方以为是工具坏了。

`force=1` 现在由 agentd **硬拒**（`driver.c:572`）：2026-10-22 在 `rwProcMem_6.12.69` 上对活跃进程实测，连续两次改 PTE 触发内核崩溃 → 设备重启。驱动修好之前不要放开。

### 7.5 驱动侧已知坑

2026-10-22 定位的全量搜索崩内核有四个根因，都在驱动侧（见 `docs/archive/audit/RWPROC33_2026-10-22.md`）。其中最容易再犯的一条：**不能用 `si.totalram` 当物理地址上界**。真机实测合法 pfn 可达 `0x926afc`（39.3GB），而 RAM 只有 11.6GB、`PA_BITS=48` —— 合法性只由 `virt_addr_valid()` 判。

---

## 8. 安全边界分层

从外到内，每层只负责自己的面：

| 层 | 挡什么 | 挡不住什么 |
|---|---|---|
| HTTP 绑定 + token | 同网段其他主机访问 MCP 端口 | 本机任意进程 |
| Origin 白名单 + 8MiB 上限 | DNS rebinding / CSRF、超大 body 撑爆内存 | 直接构造 POST 的本地程序 |
| wire AUTH | 设备上任意有 INTERNET 权限的 App 连回环端口拿 root | 拿到密钥的进程、拿到 root 的进程 |
| safe_mode 清单 | 误操作改内存、注入触摸 | 只读遍历（搜索/读是放行的） |
| shell 白名单 | 任意命令执行（默认关闭，双超时） | 白名单内的破坏性用法（`cat /dev/zero`） |
| 进程伪装 | ACE 类扫描 `/proc/<pid>/cmdline` 的硬特征 | `/proc/<pid>/exe` 指向、内存特征、内核侧检测 |
| `save_to` 路径收敛 | 写到 `export_dir` 之外（绝对路径/盘符/`..` 全拒） | 写满 `export_dir` 所在磁盘 |

具体配置要求：

- HTTP：默认只绑 `127.0.0.1`。非回环地址（含 `::`、具体网卡地址）必须有 `http_token`，否则**启动即 FATAL**。判定用"地址是否回环"而不是字符串是否等于 `0.0.0.0`（`net_guard.h`，纯 inline 可单测）。
- safe_mode：单一破坏性命令清单（`main.c:2030`），在分发前统一拦截 —— write / set_results / freeze / unfreeze / pause / resume / shell / watch / ghost_tap / ghost_swipe。`watch` 的 `poll` 是只读查询，放行。清单式而不是各 handler 自查，是因为早期散落检查漏了 unfreeze / pause / resume / watch。PC 端另有本地拒绝（表驱动的 `write_op` 字段），双保险。
- 进程伪装：清空 `argv[1..]` 与 `environ`，只留程序名（二进制名 `dspbridge`）。**必须在参数解析与驱动初始化之后** —— 提前清空会让 `dtype`/key 失效，静默降级。
- shell：白名单 `ps/ls/cat/dumpsys/getprop/id`，默认关闭；`read` + `waitpid` 双超时，超时 `SIGKILL` 并回 `GGERR_TIMEOUT`。

`GG_MAX_CLIENTS = 8` 也是安全项：不限制的话本地进程可以开几千连接耗尽线程栈与 fd。

---

## 9. 失败语义

### 9.1 七态与重放规则

`GGERR_NO_DEVICE` 把"连不上 / 发不出去 / 等不到 / 收到烂帧"压成一个码，上层无法判断重放是否安全。所以 PC 端内部记录 `CallOutcome`（`wire.h:20`）：

| 状态 | 含义 | 可重放 |
|---|---|---|
| `Ok` | 成功 | —— |
| `ConnectFailed` | 连接未建立，一个字节都没发出去 | **可以**（唯一） |
| `SendFailed` | 发送中途失败，远端可能已收到完整请求 | 否 |
| `ResponseTimeout` | 请求已完整发出，截止时间内没有匹配响应 | 否 |
| `OutcomeUnknown` | 读中途断连 / RST | 否 |
| `ProtocolError` | 收到非法帧（magic/ver/len） | 否 |
| `RemoteError` | 远端明确返回失败码 | 否 |

唯一放行重放的地方是 `session.cpp:220`：`rc != 0 && last_outcome == ConnectFailed` → 关连接、重跑 `adb forward`、重放一次。

**在 wire 层还有一条独立的重放判定**（`wire.cpp:500`）：写帧失败且 `wr != -4`（不是本地负载超限）时，关连接、重连、换新 `seq` 重发。判据不是"一个字节都没发"，而是"整帧是否发完" —— 实测 agentd 重启后，20 字节帧头往往还能塞进发送缓冲（send 成功），失败发生在 payload 阶段。整帧没发完，对端连帧都解析不出来，不会执行；发完了绝不重放。

为什么这么保守：`gg_write` / `gg_set_results` / `gg_freeze` 重放一次就是执行两遍。

### 9.2 不降级

唯一通道是 rwproc33。节点打不开、自检失败或驱动报错 → `kernel_status` 回 `state=4 (unavailable)`，读写直接报错。

不回落 `process_vm_readv`，不回落 `/proc/<pid>/mem`。两个理由：语义不一致（降级读到的数据和驱动读到的可能不同），以及用户态读取在目标进程视角下可被观测/检测。`check_kernel_only.py` 守这条：`driver_read/write` 里出现用户态回退即护栏失败。

### 9.3 错误要能定位

几条硬要求：

- 短读不是成功（1/4 字节会把"页读不到"报成"值是 0"）。
- 读到 0 字节必须带 `note` 解释原因（§7.4）。
- 超限不是静默丢帧，而是 `GGERR_CAP` + 实际字节数 + 修法（§3.6）。
- 坏响应不能静默归成 `GGERR_INTERNAL`：要区分"帧非法"、"错误帧不可解析"、"响应体形状不符"。
- 配置项存在但非法 → 启动失败，不静默回落默认值。`max_save_bytes` 判 `<= 0` 而不是 `== 0`（负数会让 `len > max` 恒真，表现为"这个功能永远拒绝"）。

---

## 10. 限额与容量

| 常量 | 值 | 位置 | 触发时 |
|---|---|---|---|
| `GGWM_MAX_PAYLOAD` | 1 MiB | `shared/wire_protocol.h` | 发送拒发 / 接收断连 / `GGERR_CAP` |
| `io_timeout_ms` | 300 s | `ServerConfig` | `GGERR_TIMEOUT` |
| `connect_timeout_ms` | 5 s | `AgentClient::Config` | `GGERR_NO_DEVICE` |
| `GG_MAX_CLIENTS` | 8 | `main.c` | 直接 close 新连接 |
| `GG_EVENT_MAX_SUBS` | 32 | `main.c` | 超出部分事件副本丢弃（记日志） |
| `GG_SOCK_SEND_TIMEOUT_MS` | 1 s | `main.c` | 慢订阅者被摘除 |
| conn 接收超时 | 300 s | `conn_thread` | 空闲连接被断开 |
| `GG_RESULT_CAP_DEFAULT` | 100 000 | `search.h` | 搜索结果封顶 |
| `get_results` limit | 10 000 | `main.c:1448` | 硬钳，不报错 |
| `GG_SEARCH_CHUNK` / `RWPROC_MAX_IO` | 256 KB | `search.h` / `driver.c` | 分块；单次读上限 |
| `GG_FUZZY_MAX_BYTES` | 256 MiB | `search.h` | 超出 region 不扫描 |
| `GG_FUZZY_UNDO_DEPTH` | 3 | `search.h` | 更早的 refine 不可回退 |
| `GG_FREEZE_MAX` | 256 | `freeze.h` | 新增冻结条目失败 |
| `GG_FREEZE_INTERVAL_MS` | 100 | `freeze.h` | —— |
| `GG_WATCH_MAX_HITS` | 128 | `watch.h` | 本端缓存封顶 |
| `GG_RWPROC_MAX_BPS` | 128 | `watch.h` | 线程数超上限的线程无断点 |
| `GG_LAST_RESP_CAP` | 8192 B | `last_resp.h` | 超出只留 256B 预览 + `truncated` |
| `max_save_bytes` | 256 MiB | `ServerConfig` | `save_to` 拒绝 |
| HTTP body | 8 MiB | `main.cpp:129` | httplib 拒绝 |

`GG_LAST_RESP_CAP` 值得单独说：`gg_cmd_end` 事件里的 `result_preview` 来自这个缓冲。超长时只留 256B 并回退到 UTF-8 边界，同时给 `result_truncated` / `result_bytes`。**不要再把截断后的串当 JSON 解析** —— 那是原缺陷的成因。

---

## 11. 改动时必须同步的地方

### 加一个 MCP 工具

1. `server/mcp/schema.cpp` 加 schema（`inputSchema`）
2. `server/session.cpp` 的 `kTools` 加条目（name / handler / `write_op`）
3. 跑 `python tests/unit/check_dispatch_consistency.py` —— 它比对两份清单，只改一边必失败

### 加一条 wire 命令

1. `shared/wire_protocol.h` 枚举**末尾**追加（不要插中间，不要回收旧值）
2. `agentd/main.c` 的 `dispatch` 加 case；破坏性命令要加进 `g_destructive_cmds`
3. PC 端 `AgentClient::call` 的调用点
4. 判断是否要进快速通道（只读、无副作用、不碰引擎状态）
5. 两端同步部署 —— 命令码错位不会报错，只会执行错命令

### 加一个事件字段

先看 `last_resp.h` 的截断语义，用 `tests/unit/test_last_resp` 覆盖 UTF-8 三种相位（`s[256]` 落在 continuation / lead byte / ASCII）。

### 改 `main()` 的初始化顺序

`memset(&g_eng, 0, ...)` + `pthread_mutex_init` + `session_pid`/`result_cap` **必须在参数解析之前**。挪回解析之后会让 `--safe` / `--shell` / `--hwbp` 解析完立刻被清零 —— 表现是"参数照收、行为不变"，而 cmdline 被伪装清空后从外部完全看不出来。

### 改协议结构体

`driver.c` 的 `_Static_assert` 与 `test_wire` 的头布局断言会拦。加了字段要同时确认 `resp_cap` 语义（§7.1）。

### 改锁

`rwproc_buf` 的"写请求 → read → 拷出"必须在同一临界区；广播的发送必须在 `g_clients_mtx` 之外。

---

## 12. 验证手段

| 手段 | 命令 | 覆盖 |
|---|---|---|
| 三端构建 | `scripts\build_pc.bat` / `build_ndk.bat` | 编译期 |
| 单元测试 16 个可执行 | `tests\unit\run_tests.bat` | PC 可跑的逻辑（含真 socket 假 agentd） |
| 结构护栏 3 道 | 同上自动执行 | dispatch 一致 / kernel-only / rwproc 临界区 |
| 真机集成 | `deploy.bat` → `run_itest.ps1` | 设备端实际行为 |
| 性能基准 | `scripts\benchmark.bat` | ❌ 仓内无基准源码，会直接报错 |

**判设备端行为前必须先部署新二进制。** `run_itest.ps1` 用的是自带 itest（链 `driver.c`），它绿了不代表设备上跑的 `agentd` 是新版 —— 本轮就因此误判过一次（数字 `value` 报 `bad value`，实为设备上还是旧 `gdm`）。

---

## 13. 设计层面的缺口

不复述债务清单（见 `PROJECT_MAP.md` §8），只列会影响设计决策的：

1. **分片已实现，但上限仍是硬的**（`GGWF_MORE`，2026-10-22）—— 单帧 `GGWM_MAX_PAYLOAD`
   1MiB、重组 `GGWM_MAX_REASSEMBLED` 8MiB，且必须两端协商（AUTH 带 `frag:1`），
   旧客户端不协商时超限仍回 `GGERR_CAP`。真正的缺口从"没有分片"变成了
   "8MiB 重组上限 + 半帧的失败语义没定义清楚"。
2. **帧头无显式编解码** —— 依赖两端同为小端且无填充。跨平台/跨编译器时需要先补这层。
3. **单连接单目标** —— 一台设备同时只有一个 attach 目标，切换即清结果集。多目标并行要么多开 agentd 实例，要么改造会话模型（成本高，当前没必要）。
4. **事件不可靠** —— best-effort，订阅者慢就被摘除，事件无重放。作为监控通道够用，不能当审计日志。
5. **精确搜索没有 undo** —— 模糊搜索能回退位图，精确搜索误操作只能重搜（要保存地址数组副本）。
6. **`main.c` 仍是 2398 行的分发中心** —— 分发、闸门、会话、事件、握手都在一起。已拆出 shell / view 两域，剩余拆分以"可编译 + 护栏可跑"为前提。
