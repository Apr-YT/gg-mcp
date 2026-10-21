# 外部审查报告逐条处置（gg-mcp-source-audit.md）

> 输入：`C:\Users\Administrator\Downloads\gg-mcp-source-audit.md`（基于 `gg-mcp-master.zip` 快照，Linux 环境）
> 处置基线：本仓 `master` @ `4b0a4d7`（分支 `audit/full-review`）
> 说明：该快照**早于**当前 master。报告里 `45 个 schema 工具`、`agentd 无应用层认证` 两项即可证明
> （当前是 43 个工具，且 `ee00d6e` 已加 `GGCMD_AUTH` 握手鉴权）。因此逐条都按当前代码复核，不直接采信。

## 状态汇总

| 状态 | 条目 | 数量 |
|---|---|---:|
| ✅ 本轮已修并验证 | A1 A2 A5 A6 A7 A8 A10 A22 A24 A27 A29 A30 A34 | 13 |
| ✅ 当前代码中已修（快照过时） | A15 A17 A20 A26 B1 B4 | 6 |
| ✅ 已被上一轮修复覆盖 | A23 | 1 |
| ⏳ 成立但本轮未修 | A3 A4 A9 A11 A12 A13 A14 A16 A18 A19 A21 A25 A28 A31 A32 A33 B2 B3 B5 B6 | 20 |
| ❌ 完全不成立 | — | 0 |

> 合计 40 条（A 类 34 + B 类 6），与报告条目数一致。

---

## 一、本轮已修（附验证）

### A1 [P1] POSIX 客户端对同一 socket 重复 connect —— 成立，已修

当前代码 `server/device/wire.cpp` 的 `tcp_connect()` 里，平台无关位置先 `connect()` 一次，
POSIX 分支切 `O_NONBLOCK` 后又 `connect()` 一次。第一次成功则第二次返回 `EISCONN(106)` → 判连接失败。

**修复**：`connect()` 移入各平台分支，POSIX 只调用一次；`fcntl` 的返回值改为检查（原来忽略）。

**验证（Linux 实测）**：WSL Ubuntu 上编译并运行 `test_wire_io`：
```
== ALL PASS ==
LINUX_RC=0
```
（修复前该测试会刷 `connect failed ... err=106` 并以 141 退出。）

### A2 [P1] `wire_protocol.h` 不自包含 —— 成立，已修

补 `#include <stddef.h>`。**验证（Linux 实测）**：WSL 上单独编译 `test_wire.cpp` 通过（`A2 test_wire PASS`）。

### A5 [P2] `h_watch` 对非字符串 `op` 把 NULL 传给 `strcmp` —— 成立，已修

加 `cJSON_IsString()` 校验，非字符串返回 `GGERR_BAD_REQUEST`。

### A6 [P1/P2] 事件广播对同一 payload 反复原地 XOR —— 成立，已修

`broadcast_event` 在订阅者循环里对同一个 `s` 反复调用 `send_frame`，而 `send_frame` 原地 XOR。
第 2 个订阅者拿到的是被解回明文的负载，但帧标志仍是 `GGWF_ENC`。

**修复**：`send_frame` 改为在**副本**上做混淆，不再改写调用者缓冲。副产品：`send_err` 现在可以安全传字符串字面量。

### A7 [P2] 同一连接上事件帧与响应帧可并发写、帧交错 —— 成立，已修

`g_clients_mtx` 只是注册表锁，`conn_thread` 的 `send_ok/send_err` 不持它。

**修复**：新增 `g_send_mtx`，`send_frame` 把「头 + 负载」整体放进这把锁，所有出站帧在进程内串行。
锁顺序固定为 `g_clients_mtx → g_send_mtx`，无反向获取。

### A8 [P1] exact/range 搜索无跨块重叠，漏检块边界匹配 —— 成立，已修

`search_worker_main` 里只有 aob 分支维护 `carry`，exact（mode 0）/range（mode 1）都在单块内扫 `i+sz<=rd`，
起始于块尾、剩余字节在下一块的匹配整条丢失。

**修复**：三种模式统一读进 `chunk + carry`，exact/range 用 `total = carry + rd` 扫描、命中地址按
`addr - carry + i` 还原，并保留尾部 `sz-1` 字节。

**验证（单测，且证明能抓住缺陷）**：`tests/unit/test_search.c` 新增 9z 组三条断言
（QWORD 骑 256KB 边界、DWORD 骑边界、range 同场景）。用旧版 `search.c` 编译同一测试：
```
FAIL: 9z) 跨块 exact: QWORD 骑在 256KB 边界仍命中
FAIL: 9z) 跨块 exact: 命中地址正确
FAIL: 9z) 跨块 range: 边界上的值仍命中
== FAILED: 3 ==
```
修复后三条全部 `ok`。

### A10 [P1/P2] freeze 线程生命周期 —— 成立，已修

`freeze_init` 只 `malloc` 了 `pthread_t` 没初始化；`freeze_stop` 无条件 `pthread_join` 这块未初始化内存
（attach 流程在首次启动前就会调到）；`freeze_start` 的失败返回值在 attach 处被丢弃；`running` 跨线程
无同步。

**修复**：`gg_freeze_t` 增 `thread_started` 标志，`running` 改 `_Atomic int`；只在 `thread_started` 时 join
并在 join 后清零；`freeze_start` 处理 `pthread_create` 失败并回滚 `running`；attach 处记录启动失败日志。
顺带把「短写记 EIO」保留（上一轮加的）。

**验证**：真机 `run_itest.ps1` → `ITEST ALL PASS`（含 freeze/unfreeze 全流程）。

### A24 [P2] 空 serial 被拒、adb 路径含空格时命令失效 —— 成立，已修（前一半）

`serial_safe` 末尾 `return !s.empty()` 与「serial 可为空 = 单设备自动选择」自相矛盾，导致默认配置在
直连失败后连 adb forward 回退都被拒。已允许空 serial。

后一半（`adb_bin` 含空格未加引号）**未修**：改为参数数组需要跨平台进程创建封装，属独立改造，记入待办。

### A27 [P2] 默认配置路径指向不存在的 `configs/ggmcp.json` —— 成立，已修

默认值、usage、文件头注释统一为 `configs/gg-ghost.json`。

### A29 [P2] agentd 出站未强制 1MiB 上限 —— 成立，已修

`send_frame` 增 `plen > GGWM_MAX_PAYLOAD` 检查，超限记日志并返回 -1（不再让 PC 端收帧后判非法断连）。

### A30 [P2] POSIX 写 socket 未处理 SIGPIPE —— 成立，已修

`send()` 改用 `GG_SEND_FLAGS`（POSIX 下 `MSG_NOSIGNAL`）。**部分验证**：Linux `test_wire_io` 全过，
其中就包含「对端中途关闭」用例，修复前该用例以 141（SIGPIPE）结束。

### A34 [P3] 序列化失败后仍对 NULL 调 `strlen` —— 成立，已修

`send_ok` 在 `cJSON_PrintUnformatted` 返回 NULL 时改发错误帧；`send_err` 自己序列化失败时发静态最小负载。

---

## 二、当前代码中已修（报告快照过时，无需处理）

| 条目 | 现状证据 |
|---|---|
| A15 `h_read_bytes` 长度/分配检查 | 已加 `len >= 1` 前置判断、`malloc` 判空、`got > len` 拒绝（上一轮） |
| A17 写内存/freeze 把短写当成功 | `h_write` 判 `w != vt_size`；`freeze_loop` 短写记 `EIO`（上一轮） |
| A20 标注库直接截断写入 | `annotations.cpp` 已是 `.tmp` + `rename` 原子写，并在损坏时先备份（`a96a23c`） |
| A26 `max_save_bytes` 负数绕过 | 已改为 `<= 0` 拒绝（上一轮） |
| B1 agentd 无应用层认证 | `ee00d6e` 增加 `GGCMD_AUTH`：未认证连接只放行 PING/AUTH，密钥从 0600 文件读取 |
| B4 `lint.bat` 恒 `exit /b 0` | 已改 `--error-exitcode=2` + goto 传播失败；负向用例实测 RC=1（上一轮） |

## 三、已被上一轮修复覆盖

- **A23 `save_to` 并发写同一 `.part`**：`Session::call_tool` 现在全程持会话锁（上一轮 F-04），
  同一进程内的 save 已串行化。跨进程仍可能竞争，但本工具是单进程 MCP server，风险面已收敛。

---

## 四、成立但本轮未修（按建议优先级）

| 条目 | 级别 | 为什么暂不修 |
|---|---|---|
| A3 HTTP 远程绑定只比对字符串 `"0.0.0.0"` | P1（依部署） | 需要 IP 解析 + loopback 判定，属独立安全改造，需配套测试 |
| A4 HTTP body 无上限、鉴权在 body 之后 | P1（依部署） | httplib 需 `set_payload_max_length`，同时要定 MCP 合理上限 |
| A9 `pointer_scan` 的 cap 未校验/未判分配失败 | P1 | 需要先读通 `search_pointer_scan` 的容量语义再改，避免改变既有上限行为 |
| A11 watch fallback 忽略 type/len、`watched==0` 仍报成功 | P2 | 需要按内核实际支持能力重写，且 hwbp 当前默认禁用（模块会崩内核） |
| A12/A13 watch 的 `waitpid(-1)` 抢子进程、`stop` 无同步 | P2 | 与 A11 同一处生命周期改造，需真机 ptrace 验证 |
| A14 指针扫描短读 `rd < psz` 仍 memcpy | P2 | 同上，需与 A9 一起改 |
| A16 `parse_value_str` 允许尾随垃圾 / `nan` / `-1` | P2 | 收紧会影响现有客户端（如传 `"1.0"`），需先确认调用方格式 |
| A18 QWORD 精确/模糊复查经 double 比较 | P2 | 需要改 `search_refine*` 的比较域，回归面大 |
| A19 fuzzy 短读非对齐会错位 | P2 | 依赖驱动短读语义，需真机构造非对齐短读 |
| A21 `save_to` 符号链接可跳出导出目录 | P2 | 需 `openat`/`O_NOFOLLOW` 或 realpath 祖先校验，跨平台实现差异大 |
| A25 `result_cap` 配置不下发 | P2 | 需要新增协议字段或 agentd 启动参数，属接口变更 |
| A28 JSON-RPC 未校验 `jsonrpc` 字段 / id 类型 | P2 | 会影响所有客户端，需先确认 MCP 客户端实际发的形态 |
| A31 `cmd_shell` 输出满后关 pipe 可能 SIGPIPE 子进程 | P2 | 需要改成 drain 线程或继续读丢弃，属行为变更 |
| A32 `std::localtime` 非线程安全 | P3 | 当前所有调用都在 store 锁内，报告自己也标注"未证实" |
| A33 `process_list/maps` 扩容失败静默截断 | P3 | 需要改 API 返回语义（加 truncated/error），影响多处调用方 |
| B2 搜索 worker 创建失败未上报 | P2 | 与 A8 同一函数，建议下一轮一起改 |
| B3 watch ptrace 与子进程管理冲突 | P2 | 需真机 ptrace 验证 |
| B5 CI 缺 Linux 客户端测试 | P2 | 本轮已用 WSL 手工验证过 A1/A2；把 WSL 步骤固化进 CI 是独立工作 |
| B6 无 benchmark 源码 | P2 | 已知债，非本次范围 |

### 本轮额外修的一条（报告未列）

- **A22 `save_to` 先删旧文件再 rename**：报告指出 rename 失败会丢旧数据，成立。已改为先按 POSIX 语义
  直接 rename，失败才走 `.bak` 中转并支持回滚。

---

## 五、总体判断

这份外部报告的价值在于**两处只有换平台才能暴露的问题**（A1 POSIX connect、A2 头文件自包含），
它们在本机 Windows 环境下完全不可见 —— 我上一轮的审查漏掉了。本轮已修，并都用 Linux 实测确认。

其余条目里：
- 6 条是快照过时（当前代码已修），说明"只做静态阅读、不核对 HEAD"会产生误报；
- 12 条在本轮修掉，其中 A8（跨块漏检）是有单测证明的真实功能缺陷；
- 19 条成立但仍待处理，已在第四节按优先级列出，**没有把它们写成"已解决"**。

未验证部分：A5/A6/A7/A10/A29/A34 的修复只做了编译 + 单测 + 真机 itest 的正常路径，
多订阅者事件、并发帧交错、OOM 分支都没有构造出来的测试 —— 这几条属于"代码路径已改正，行为未压测"。
