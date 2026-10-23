# main.c 拆分方案

> **状态**：**已完成（2026-10-23）** · 拆前 2049 行 → 拆后 **658 行**（-68%）
> 执行记录与偏差见文末第 11 节 —— 先读那节，方案里有两处与实际不符。
> **前提**：已有拆分约定（`engine_internal.h` 声明层 + `cmd_*.c` 外迁模式）验证可行，护栏脚本（`check_dispatch_consistency.py` / `check_kernel_only.py` 等）不受影响。每步必须编译 + 跑 `tests/unit/run_tests.bat` 全绿再进下一步。

---

## 1. 当前 main.c 功能段分布（精确行号）

| 行号区间 | 内容 | 行数 | 性质 |
|---|---|---|---|
| 1 - 43 | 头文件 includes | 43 | 留 main.c |
| 44 - 76 | 全局状态（`g_eng` / `g_watch` / `g_hwbp_*` / `g_last_resp` / `g_clients_*mtx` 等） | 33 | **留 main.c**（单一所有权） |
| 77 - 98 | wire 鉴权预备（`g_wire_key` / `ct_equal` 常量时间比较） | 22 | 搬 Phase 1·③ |
| 99 - 110 | `log_msg` | 12 | 留 main.c（engine_internal.h 已声明） |
| 111 - 166 | `parse_u64_strict` / `parse_force` / `read_full` / `send_full` | 56 | 留 main.c（已在 engine_internal.h 声明） |
| 167 - 210 | `g_send_mtx` / `send_frame` | 44 | 搬 Phase 1·① |
| 212 - 254 | `send_frame_fragmented`（含 `client_supports_frag` 前置声明） | 43 | 搬 Phase 1·① |
| 256 - 296 | `send_ok` | 41 | 搬 Phase 1·① |
| 298 - 318 | `send_err` | 21 | 搬 Phase 1·① |
| 319 - 338 | `gg_client_t` / `g_clients` / `g_clients_mtx` | 20 | 搬 Phase 1·② |
| 339 - 418 | `clients_register/unregister/set_subscribed/set_frag` / `client_set_send_timeout` / `GG_SOCK_SEND_TIMEOUT_MS` | 80 | 搬 Phase 1·② |
| 420 - 424 | `now_ms` | 5 | 搬 Phase 1·②（事件系统共用） |
| 427 - 440 | `clients_drop` | 14 | 搬 Phase 1·② |
| 442 - 499 | `broadcast_event` + `GG_EVENT_MAX_SUBS` | 58 | 搬 Phase 1·② |
| 500 - 508 | `event_simple` | 9 | 搬 Phase 1·② |
| 510 - 558 | `cmd_name` | 49 | 搬 Phase 1·② |
| 560 - 583 | `event_cmd_start` | 24 | 搬 Phase 1·② |
| 586 - 610 | `event_cmd_end`（依赖线程局部 `g_last_resp`） | 25 | 搬 Phase 1·② |
| 613 - 619 | `event_session_change` | 7 | 搬 Phase 1·② |
| 622 - 628 | `event_search_update` | 7 | 搬 Phase 1·②（engine_internal.h 已声明） |
| 630 - 677 | `event_results_update` + `RESULTS_SYNC_MAX` | 48 | 搬 Phase 1·② |
| 680 - 730 | `watch_hit_json` | 51 | 搬 Phase 1·② |
| 733 - 735 | `event_watch_hit` | 3 | 搬 Phase 1·② |
| 737 - 764 | `parse_value_str` | 28 | 留 main.c（engine_internal.h 已声明） |
| 766 - 781 | `j_scalar_str` | 16 | 留 main.c |
| 782 - 861 | wire 鉴权密钥管理（`gen_random_hex` / `read_key_file` / `load_or_create_ephemeral_key` / `GG_EPHEMERAL_KEY_PATH` / `GG_WIRE_KEY_BYTES` / `g_wire_key_ephemeral`） | 80 | 搬 Phase 1·③ |
| 862 - 876 | `j_addr_u64` | 15 | 留 main.c（engine_internal.h 已声明） |
| 877 - 914 | `h_auth` | 38 | 搬 Phase 1·③ |
| 915 - 921 | `h_ping` | 7 | 搬 Phase 2·④ |
| 923 - 955 | `h_self_test` + `h_list_processes` | 33 | 搬 Phase 2·④ |
| 956 - 976 | `resolve_pid` | 21 | 搬 Phase 2·④ |
| 977 - 1021 | `session_guard` / `session_drop` | 45 | 搬 Phase 2·④（需 engine_internal.h 新增声明） |
| 1022 - 1092 | `h_attach` / `h_detach` | 71 | 搬 Phase 2·④ |
| 1093 - 1162 | `h_target_info` / `h_module_list` | 70 | 搬 Phase 2·④ |
| 1163 - 1191 | `parse_vt` | 29 | 留 main.c（engine_internal.h 已声明） |
| 1192 - 1257 | `h_disassemble` | 66 | 搬 Phase 2·⑤ |
| 1258 - 1359 | `h_watch`（set/clear/poll/inject 四合一，含 hwbp 探测） | 102 | 搬 Phase 2·⑤ |
| 1362 - 1385 | `h_module_base` | 24 | 搬 Phase 2·⑤ |
| 1387 - 1421 | `h_calc_offset` | 35 | 搬 Phase 2·⑤ |
| 1423 - 1452 | `h_pointer_scan` | 30 | 搬 Phase 2·⑤ |
| 1456 - 1480 | `h_ghost_tap` | 25 | 搬 Phase 2·⑥ |
| 1482 - 1507 | `h_ghost_swipe` | 26 | 搬 Phase 2·⑥ |
| 1509 - 1563 | `h_kernel_status`（快速通道无锁 + 原子快照 + `SNAP` 宏） | 55 | 搬 Phase 2·⑦ |
| 1573 - 1584 | `h_device_info` | 12 | 搬 Phase 2·⑦ |
| 1592 - 1598 | `h_subscribe` | 7 | 搬 Phase 2·⑦ |
| 1600 - 1611 | `h_relay_event` | 12 | 搬 Phase 2·⑦ |
| 1613 - 1794 | `dispatch()` —— 中央路由器 | 182 | **留 main.c**（紧耦合握手门/地址门/safe_mode 门） |
| 1798 - 1834 | `conn_thread()` —— 连接生命周期 | 37 | **留 main.c** |
| 1838 - 1851 | `print_usage` | 14 | **留 main.c** |
| 1859 - 1867 | `hide_process` | 9 | **留 main.c** |
| 1868 - 2049 | `main()` —— 参数解析 + 引擎初始化 + 驱动 attach + accept loop | 182 | **留 main.c** |

---

## 2. 已有拆分约定（验证过的 `engine_internal.h` 模式）

```
engine_internal.h  ←  跨翻译单元共享声明的**唯一事实源**
cmd_search.c       ←  h_search/h_search_range/h_unknown_init/h_refine/...
cmd_mem.c          ←  h_read/h_read_bytes/h_write/h_freeze/...
cmd_view.c         ←  h_view_memory/h_resolve_symbol/h_pointer_chain
cmd_shell.c        ←  h_shell
main.c             ←  定义所有权 + dispatch + conn_thread + main()
```

**已在 engine_internal.h 声明**（这些函数搬去 cmd_*.c 后**零接口变更**）：
- 帧收发：`read_full` / `send_full` / `send_frame` / `send_ok` / `send_err`
- 参数解析：`parse_u64_strict` / `parse_force` / `parse_vt` / `j_scalar_str` / `parse_value_str` / `j_addr_u64`
- 引擎单例：`extern gg_engine_t g_eng` / `extern gg_watch_t g_watch`
- 事件：`event_search_update` / `event_results_update`

---

## 3. Phase 1：基础设施外迁（main.c 降到 ~1350 行）

**风险最低、收益最大**——这些函数与 handler 紧耦合度最低，且 engine_internal.h 已经提前声明了。

### 3.1 `wire_io.c`（~180 行 · 首选验证拆交流程）

**搬的内容**：

| 来源行号 | 符号 | 备注 |
|---|---|---|
| 167 | `static pthread_mutex_t g_send_mtx` | `static` → 去掉，改为全局 |
| 173-210 | `send_frame` | engine_internal.h 已声明 |
| 216-254 | `send_frame_fragmented` | 与 `send_frame` 共用 `g_send_mtx`，必须同文件 |
| 256-296 | `send_ok` | engine_internal.h 已声明 |
| 298-318 | `send_err` | engine_internal.h 已声明 |

**engine_internal.h 需补充**：

```c
extern pthread_mutex_t g_send_mtx;          /* 分片组必须在同一锁持有期间发完 */
int send_frame_fragmented(int fd, uint16_t cmd, uint32_t seq, uint32_t flags,
                          const char *payload, size_t plen);
```

**依赖**：`gg_xor_payload`（wire_protocol.h inline）、`now_ms`（Phase 1·② events.c）、`log_msg`（留 main.c）。`now_ms` 可以先定义在 wire_io.c 里，events.c 之后再挪。

### 3.2 `events.c`（~400 行 · 最大独立子系统）

**搬的内容**：

| 来源行号 | 符号 | 备注 |
|---|---|---|
| 330-338 | `gg_client_t` / `g_clients` / `g_clients_mtx` | 结构体定义 + 全局注册表 |
| 344-352 | `GG_SOCK_SEND_TIMEOUT_MS` / `client_set_send_timeout` | |
| 354-398 | `clients_register/unregister/set_subscribed/set_frag` / `client_supports_frag` | 补齐前置声明 |
| 400-418 | `clients_set_subscribed` | 含"广播失败重新登记"分支 |
| 420-424 | `now_ms` | 从 wire_io.c 挪过来 |
| 427-440 | `clients_drop` | |
| 442-499 | `broadcast_event` + `GG_EVENT_MAX_SUBS` | ⚠️ **锁外 dup() 发送模式别改** |
| 502-508 | `event_simple` | |
| 510-558 | `cmd_name` | 日志展示用 |
| 560-583 | `event_cmd_start` | |
| 586-610 | `event_cmd_end` | 依赖 `_Thread_local g_last_resp` |
| 613-619 | `event_session_change` | |
| 630-677 | `event_results_update` + `RESULTS_SYNC_MAX` | |
| 680-730 | `watch_hit_json` | |
| 733-735 | `event_watch_hit` | |

**engine_internal.h 需补充**：

```c
/* 事件系统 */
int  client_supports_frag(int fd);
void client_set_send_timeout(int fd);
void clients_register(int fd);
void clients_unregister(int fd);
void clients_set_frag(int fd, int on);
void clients_set_subscribed(int fd, int on);
void clients_drop(int fd);
void broadcast_event(cJSON *ev);
void event_cmd_start(uint16_t cmd, cJSON *p);
void event_cmd_end(uint16_t cmd, uint64_t t0);
void event_session_change(const char *state, pid_t pid, const char *name);
void event_watch_hit(const gg_watch_hit_t *h);

/* watch_hit_json 被 h_watch（Phase 2·⑤）调用 */
cJSON *watch_hit_json(const gg_watch_hit_t *h);

/* cmd_name 被 dispatch 日志用 */
const char *cmd_name(uint16_t cmd);

/* event_cmd_end 依赖线程局部存档 */
extern _Thread_local gg_last_resp_t g_last_resp;
```

### 3.3 `wire_auth.c`（~130 行）

**搬的内容**：

| 来源行号 | 符号 | 备注 |
|---|---|---|
| 80-84 | `static char g_wire_key[65]` | `static` → 去掉 |
| 92-98 | `ct_equal`（常量时间比较） | |
| 797-801 | `GG_EPHEMERAL_KEY_PATH` / `GG_WIRE_KEY_BYTES` / `g_wire_key_ephemeral` | `static` 去掉 |
| 804-828 | `gen_random_hex` | |
| 831-843 | `read_key_file` | |
| 847-861 | `load_or_create_ephemeral_key` | |
| 886-913 | `h_auth` | |

**engine_internal.h 需补充**：

```c
/* wire 鉴权 */
extern char g_wire_key[65];
extern int g_wire_key_ephemeral;
int  ct_equal(const char *a, const char *b);
int  read_key_file(const char *path, char *out, size_t outsz);
int  load_or_create_ephemeral_key(char *out, size_t outsz);
void h_auth(int fd, uint32_t seq, cJSON *p, int *authed);
```

**注意**：`g_wire_key_ephemeral` 被 main() 里的日志打印读（`fprintf(stderr, "...%s...", g_wire_key_ephemeral ? ", ephemeral" : "")`），所以 main.c 里必须 `extern` 它，**不能**让它在 wire_auth.c 里仍是 static。

---

## 4. Phase 2：handler 外迁（main.c 降到 ~400 行）

照搬已有 cmd_*.c 模式：逐字搬运 + 去掉 static + engine_internal.h 加声明。

### 4.1 `cmd_proc.c`（进程/会话域）

**搬的内容**：

```
h_ping
h_self_test
h_list_processes
resolve_pid
session_guard                  ← 被 dispatch 在锁内调用，必须 extern
session_drop                   ← 被 session_guard 和 h_attach/h_detach 调用
h_attach
h_detach
h_target_info
h_module_list
```

**engine_internal.h 需补充**：

```c
/* 进程/会话域 */
pid_t resolve_pid(cJSON *p);
int   session_guard(void);
void  session_drop(const char *reason);

void h_ping(int fd, uint32_t seq, cJSON *p);
void h_self_test(int fd, uint32_t seq, cJSON *p);
void h_list_processes(int fd, uint32_t seq, cJSON *p);
void h_attach(int fd, uint32_t seq, cJSON *p);
void h_detach(int fd, uint32_t seq, cJSON *p);
void h_target_info(int fd, uint32_t seq, cJSON *p);
void h_module_list(int fd, uint32_t seq, cJSON *p);
```

**关键依赖**：`session_guard()` 被 dispatch 在 `pthread_mutex_lock(&g_eng.mtx)` 之后立即调用（main.c line 1728），这个位置关系**不能改**——dispatch 留在 main.c，session_guard 搬出去只是换了文件，调用时机保持不变。

### 4.2 `cmd_analyze.c`（分析域）

**搬的内容**：

```
h_disassemble                  ← 依赖 disasm.c (Capstone ARM64)
h_watch                        ← 最复杂 handler (~100 行, 含 hwbp 探测)
h_module_base                  ← 依赖 process_modules()
h_calc_offset                  ← 同上
h_pointer_scan                 ← 依赖 g_eng.results (结果集存在)
```

**全局变量归属决策**：`g_hwbp_allowed` / `g_hwbp_probe` / `g_hwbp_probe_rc`——这三个被 main() 的 `--hwbp` 参数解析写、被 `h_watch` 读。

**推荐：留在 main.c，声明 extern 给 cmd_analyze.c**。理由：
- 它们是运行时配置开关（main() 启动参数解析写），不是 cmd_analyze.c 的业务状态
- main.c 里的 `--hwbp` 参数解析代码（line 1923）直接写 `g_hwbp_allowed = 1`，保持就近

**engine_internal.h 需补充**：

```c
/* hwbp 配置开关（定义在 main.c，启动参数解析写） */
extern int g_hwbp_allowed;
extern int g_hwbp_probe;
extern int g_hwbp_probe_rc;

void h_disassemble(int fd, uint32_t seq, cJSON *p);
void h_watch(int fd, uint32_t seq, cJSON *p);
void h_module_base(int fd, uint32_t seq, cJSON *p);
void h_calc_offset(int fd, uint32_t seq, cJSON *p);
void h_pointer_scan(int fd, uint32_t seq, cJSON *p);
```

### 4.3 `cmd_touch.c`（虚拟触摸）

**搬的内容**：

```
h_ghost_tap                    ← 依赖 driver_touch_tap()
h_ghost_swipe                  ← 依赖 driver_touch_swipe()
```

最小的一个文件（~60 行），没有跨域依赖。

**engine_internal.h 需补充**：

```c
void h_ghost_tap(int fd, uint32_t seq, cJSON *p);
void h_ghost_swipe(int fd, uint32_t seq, cJSON *p);
```

### 4.4 `cmd_status.c`（状态域）

**搬的内容**：

```
h_kernel_status                ← 快速通道无锁 + 原子快照 + SNAP 宏
h_device_info                  ← 快速通道无锁
h_subscribe                    ← 依赖 clients_set_subscribed（events.c）
h_relay_event                  ← 依赖 event_simple（events.c）
```

**engine_internal.h 需补充**：

```c
void h_kernel_status(int fd, uint32_t seq, cJSON *p);
void h_device_info(int fd, uint32_t seq, cJSON *p);
void h_subscribe(int fd, uint32_t seq, cJSON *p, int on);
void h_relay_event(int fd, uint32_t seq, cJSON *p);
```

---

## 5. 留在 main.c 的最终内容（~380 行）

```
includes                          ~43 行
全局状态（g_eng/g_watch/g_hwbp_*/g_last_resp/g_wire_key_ephemeral/g_clients_mtx）  ~33 行
log_msg + parse 参数 helper（parse_u64_strict / parse_force / parse_vt / j_scalar_str / parse_value_str / j_addr_u64 / j_addr_u64）  ~150 行
dispatch()                        ~182 行
conn_thread()                      ~37 行
print_usage + hide_process         ~23 行
main()                            ~182 行
```

**dispatch 为什么留 main.c**：它紧耦合 auth 门（`h_auth` + 握手门）、地址门（17 处 `strtoull` 统一拦截）、safe_mode 门（`g_destructive_cmds` 数组 + WATCH poll 例外）、快速通道（PING/KERNEL_STATUS 无锁）、锁内串行（`g_eng.mtx`）。把它搬出去收益低、风险高（auth 门要引 `g_wire_key_ephemeral`、地址门要引 `parse_u64_strict`、safe_mode 门要引 `g_eng.safe_mode`——等于又拉回一堆依赖）。

---

## 6. 护栏脚本兼容性矩阵

| 脚本 | 受影响 | 说明 |
|---|---|---|
| `check_dispatch_consistency.py` | ✅ 不受影响 | 只解析 schema.cpp 和 session.cpp |
| `check_kernel_only.py` | ✅ 不受影响 | 只看 driver.c |
| `check_hwbp_detail.py` | ✅ 不受影响 | 只看 driver.c 的 GET_HWBP_HIT_DETAIL 调用 |
| `check_rwproc_locking.py` | ✅ 不受影响 | 只看 driver.c |
| `check_scalar_cmd.py` | ✅ 不受影响 | 只看 driver.c |

**全部护栏零改动**——driver.c 和 dispatch 都没动，这是刻意设计的。

---

## 7. 全局变量归属表

| 变量 | 定义位置 | engine_internal.h 声明 | 消费方 |
|---|---|---|---|
| `g_eng` | main.c | ✅ `extern gg_engine_t` | 所有 cmd_*.c + driver.c |
| `g_watch` | main.c | ✅ `extern gg_watch_t` | watch.c + driver.c + cmd_analyze.c |
| `g_last_resp` | main.c | ✅ `extern _Thread_local` | events.c（event_cmd_end） |
| `g_clients` | events.c（搬后） | ❌ 内部用 | events.c 内部 |
| `g_clients_mtx` | events.c（搬后） | ❌ 内部用 | events.c 内部 |
| `g_send_mtx` | wire_io.c（搬后） | ✅ `extern pthread_mutex_t` | wire_io.c 内部（send_frame + send_frame_fragmented） |
| `g_wire_key` | wire_auth.c（搬后） | ✅ `extern char[65]` | wire_auth.c 内部 + main.c（鉴权成功后不直接用） |
| `g_wire_key_ephemeral` | wire_auth.c（搬后） | ✅ `extern int` | main.c（日志打印） |
| `g_hwbp_allowed` | main.c | ✅ `extern int` | cmd_analyze.c（h_watch） |
| `g_hwbp_probe` | main.c | ✅ `extern int` | cmd_analyze.c（h_watch） |
| `g_hwbp_probe_rc` | main.c | ✅ `extern int` | cmd_analyze.c（h_watch） |
| `g_active_clients` | main.c | ❌ main.c 内部 | main.c（accept 循环计数） |

---

## 8. 操作纪律（从已踩坑提炼）

1. **逐次搬 + 逐次编译 + 逐次跑 run_tests.bat**。Phase 1·① `wire_io.c` 是最安全的起步——声明已就绪，依赖最少。
2. **`static` → 去 `static`**。函数从 main.c 搬到 cmd_*.c 后，原来在 main.c 内部的 static 会变成"跨文件不对外"——必须去掉，靠 engine_internal.h 声明暴露。
3. **engine_internal.h 是唯一共享声明源**。新增 extern 一律加这里；不要在 cmd_*.c 里自己写 `extern void xxx()`。
4. **broadcast_event 里的 `dup()` + 锁外发送顺序别动**。那个结构是为了防"僵死订阅者拖死全局"——搬到 events.c 时**逐字照抄**。
5. **send_frame_fragmented 的分片组必须在同一 `g_send_mtx` 持有期间发完**。把它和 send_frame 放在同一个 .c 文件里就是为了保证这一点——千万别分开。
6. **`_Thread_local` 声明不能省**。`g_last_resp` 是线程局部，event_cmd_end 搬去 events.c 后 engine_internal.h 必须显式声明它是 `_Thread_local`，否则编译报错。
7. **dispatch 的快速通道（PING/KERNEL_STATUS 无锁）不能挪进锁内**。那就是为了让大进程搜索期间仍能响应探活。搬到别的文件时，快速通道在锁前、锁内两套 case 分支**都要保留**。
8. **`session_guard()` 在锁内调用的位置不变**。dispatch 里是 `lock(mtx)` → `session_guard()` → `switch(h->cmd)`，这个顺序是"所有 handler 都先经过会话自愈再执行"的语义保证。

---

## 9. 验收步骤（每次搬完一个文件必做）

```powershell
# 1. 编译 agentd
scripts\build_ndk.bat

# 2. 编译 PC server
scripts\build_pc.bat

# 3. 跑单元测试 + 5 个结构护栏
cd tests\unit
.\run_tests.bat

# 预期：ALL UNIT TESTS PASSED + 5 个护栏脚本 [OK]
```

**Phase 1 完成标志**：`wc -l agentd/main.c` ≤ 1400 行；Phase 2 完成标志：≤ 450 行。

---

## 10. 后续可选（拆分之外的小优化）

拆分完成后，以下小改动可以**择机**做（不阻塞拆分验收）：

| 项 | 收益 | 风险 |
|---|---|---|
| `g_last_resp` + `GG_EPHEMERAL_KEY_PATH` 路径常量上移 `engine_internal.h` | 集中管理 | 低 |
| `g_ghost_tap/swipe` 的坐标范围参数化（当前硬编码 x1,y1,x2,y2 但 schema 里没钳制） | 防 OOB | 低（schema 里加 `max`） |
| `dispatch` 里的 `g_destructive_cmds[]` 数组抽到 `engine_internal.h` 宏或 const 数组 | 单一清单 | 极低 |
| `parse_vt` 接受 `"u64"` → `VT_QWORD` 这类别名（已支持 `qword/long`） | 更友好 | 极低 |
| `h_kernel_status` 的快速通道无锁快照（SNAP 宏 + atomic_load）做成通用宏 | 其他快速通道 handler 复用 | 低 |

---

## 11. 执行记录（2026-10-23，全部完成）

### 实际产出

| 文件 | 行数 | 内容 |
|---|---|---|
| `main.c` | **658** | 全局状态 / log_msg / 参数 helper / dispatch / conn_thread / main() |
| `wire_io.c` | 179 | send_frame / send_frame_fragmented / send_ok / send_err + g_send_mtx |
| `events.c` | 447 | 客户端注册表 / broadcast / event_* / cmd_name / watch_hit_json |
| `wire_auth.c` | 164 | g_wire_key / ct_equal / 临时密钥 / h_auth |
| `cmd_proc.c` | 308 | ping/self_test/list_processes/resolve_pid/session_*/attach/detach/target_info/module_list |
| `cmd_analyze.c` | 295 | disassemble / watch（四合一）/ module_base / calc_offset / pointer_scan |
| `cmd_touch.c` | 68 | ghost_tap / ghost_swipe |
| `cmd_status.c` | 125 | kernel_status / device_info / subscribe / relay_event |

main.c 最终 658 行，比方案估的 ~450 多。原因见下面第 3 条偏差。

### 三处与方案的偏差（下次写这类方案要注意）

1. **方案漏了四个前置声明，拆分时被当代码删掉了。**
   原 main.c 在 `h_kernel_status` 之后、`dispatch` 之前有一小段：
   ```c
   /* cmd_view.c（拆分外迁） */
   void h_view_memory(int fd, uint32_t seq, cJSON *p);
   void h_resolve_symbol(int fd, uint32_t seq, cJSON *p);
   void h_pointer_chain(int fd, uint32_t seq, cJSON *p);
   ```
   还有 `h_shell` 的声明。它们混在"handler 区"里，按区间一刀切就一起没了，
   编译报 `implicit declaration`。现在统一收进 engine_internal.h。
   **教训：按"函数区间"删除时，要一并检查区间内有没有别人留下的前向声明。**

2. **Phase 1·③ 的边界方案写错了。** 方案说 `j_addr_u64` 留 main.c，
   但它夹在"wire 鉴权密钥"区中间 —— 不能整块搬。实际做法是把该区切成两半，
   只搬 `j_addr_u64` 之前的部分，`j_addr_u64` 留下。

3. **main.c 到不了 450 行的原因。** 方案第 5 节估的 380 行里没算：
   includes 43 + 全局状态（含 hwbp 三开关和它们的长注释）33 + 参数 helper 60 +
   `j_addr_u64` 15 + dispatch 182 + conn_thread 37 + print_usage/hide_process 23 +
   main() 182 = **约 575**，实际 658（多出的是一路补的注释）。
   所以 450 这个目标从一开始就偏乐观。**658 是合理终点。**

### 验收（按方案第 9 节）

- `scripts\build_ndk.bat` → OK
- `scripts\build_pc.bat` → OK
- `tests\unit\run_tests.bat` → ALL PASS + 5 道护栏全 OK
  （dispatch 一致性护栏自动校验 46 个工具，拆分没影响它）
- `tests\integration\run_itest.ps1` → **ITEST ALL PASS**（真机，含 hwbp 三项）
- dispatch 的 case 数：拆分前 42 / 拆分后 42，`diff` 无差异

### 顺带修的

- UI 的 `do_read_one` 一直取 `value_hex`，而 `gg_read` 只返回
  `{bytes, value}` —— 日志里那个 hex 一直是 `0x?`。改成按 JSON 类型分支打印。
- 验证 `gg_pointer_scan` 时确认了字段名是 `hits:[{container,target,ptype}]`，
  且 container 里确实存着它报告的 target（读回 `59177788 == 0x386FB3C`）。
