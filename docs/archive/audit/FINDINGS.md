# FINDINGS — gg-mcp 缺陷清单（2026-10-21）

> 基线提交 `dddda9b`，审查分支 `audit/full-review`。
> 每条都给出**代码路径证据**；无法用 PC 单测复现的条目在「验证」列写明实际手段。
> 状态：✅ 已修（附验证方式）｜⏸ 未修（附理由）

## 统计

| 级别 | 数量 | 已修 |
|---|---:|---:|
| P0（数据静默损坏） | 1 | 1 |
| P1（功能错误 / 进程可被终止） | 4 | 4 |
| P2（错误语义 / 工具链假绿 / 测试可靠性） | 7 | 7 |
| P3（健壮性 / 可维护性） | 4 | 2 |

---

## P0

### F-01 `rwproc_buf` 是进程级共享缓冲，却在锁外被多线程使用

| 项 | 内容 |
|---|---|
| 位置 | `agentd/driver.c` — `static uint8_t rwproc_buf[]`（原 100 行）、`rwproc_call_ex`、`rwproc_read_mem`、`rwproc_open_process`、`rwproc_hwbp_*` |
| 触发条件 | 并行搜索（`search.c:140` 起 N 个 worker 同时 `driver_read`）与冻结线程（`freeze.c:32` 每 50ms `driver_write`）并发，且两者都**不在** `g_eng.mtx` 覆盖内 |
| 根因 | 请求写入、`read(fd,...)`、结果 `memcpy` 三步直接操作同一个静态缓冲，没有任何同步。`search.c:17` 的注释"driver_read 为独立 ioctl, 线程安全"与实现矛盾 |
| 实际影响 | 请求头与结果区被别的线程覆盖 → 读到别的地址的数据、`OPEN_PROCESS` 句柄错位、搜索命中值对不上。**不崩溃、不报错，只是数据是错的** |
| 验证 | 代码路径证据（三处调用点 + 共享 static 缓冲 + 无锁）；PC 端无法构造 fd 模拟，追加结构护栏 `check_rwproc_locking.py` 锁死该约束 |
| 修复 | `rwproc_call_ex` 增加 `out/out_cap`，把结果拷出与长度校验搬进 `pthread_mutex_lock(&rwproc_mtx)` 临界区；所有调用点改为由 out 取结果，删除锁外 `memcpy` |
| 回归 | `check_rwproc_locking.py`：断言 `rwproc_buf` 的每处引用都在临界区区间内、无函数外引用、`rwproc_mtx` 静态初始化。已做负向验证（删掉 lock 行 → FAIL） |

---

## P1

### F-02 `h_write` 把短写当成功

| 项 | 内容 |
|---|---|
| 位置 | `agentd/main.c` — `h_write` |
| 触发条件 | 驱动返回 `0 < w < vt_size(type)`（跨页写、目标页中途不可写） |
| 根因 | 只判 `w < 0`，随后把 `w` 原样放进响应的 `bytes` 字段并判定成功 |
| 影响 | 调用方以为值已改完，实际只落进部分字节 → 后续逻辑基于错误内存状态运行 |
| 修复 | `w != vt_size(type)` 时返回 `GGERR_INTERNAL` + `short write: N of M bytes` |
| 回归 | 无自动化用例（`h_write` 是 `main.c` 的 static handler，且 `driver_write` 为真实现，PC 端无 fd 可 mock）。**标注：需真机 itest 覆盖** |

### F-03 `h_read` 把短读当成功

| 项 | 内容 |
|---|---|
| 位置 | `agentd/main.c` — `h_read` |
| 触发条件 | `driver_read` 返回 0..sz-1（页未驻留，`is_force=0` 时不翻 PTE） |
| 根因 | `buf[8]` 零初始化，短读后仍按完整值解析 |
| 影响 | "页读不到"被报成"值是 0"，是最容易误导逆向判断的一类错误 |
| 修复 | `got != sz` → `GGERR_INTERNAL` + `short read: N of M bytes (page not resident?)` |
| 回归 | 同上，需真机 |

### F-04 `Session::mtx_` 是死成员，HTTP 传输下并发无保护

| 项 | 内容 |
|---|---|
| 位置 | `server/session.h:107` 声明；`server/session.cpp` 全文无 `lock_guard` |
| 触发条件 | `--transport http`（httplib 线程池并发处理 `POST /mcp`），两个请求同时进入 `call_tool` |
| 根因 | `AgentClient::mtx_` 只串行化单次 `call`；`client_.last_error`（std::string）与 `last_outcome` 在锁外被跨调用读写；`device_call` 的 `close()+重放` 会把另一个线程正在用的连接掐断 |
| 影响 | 并发下 `last_error` 读写是数据竞争（std::string 撕裂 = UB）；偶发 `NO_DEVICE`，且难以归因 |
| 修复 | `call_tool` 全程持 `mtx_`（复活该成员）；`local_status()` 加注释说明必须在锁内调用（非递归锁，不得自锁） |
| 回归 | 无并发压测（httplib 并发需起真实 server + 假 agentd，成本高）。**标注：未自动化** |

### F-05 `force` 参数类型不符会让 `call_tool` 抛出未捕获异常并终止进程

| 项 | 内容 |
|---|---|
| 位置 | `server/session.cpp` — `tool_read` / `tool_read_bytes`（两处）/ `tool_view_memory` / `save_to` 分支，原写法 `a["force"].get<int>()` |
| 触发条件 | MCP 调用 `gg_read` 时传 `{"force": "abc"}` 或 `{"force": true}` |
| 根因 | `get<int>()` 对 string/bool 抛 `json::type_error`；只有 `McpProtocol::handle` 外层有 `catch`，直接调用 `call_tool` 的路径没有 |
| 影响 | 经 MCP 协议层时被吞成 `-32603 Internal error`（参数错报成服务端故障）；**实测直接调用路径 `std::terminate`，进程退出码 127** |
| 证据 | 用 `dddda9b` 版 `session.cpp` 编译同一测试：`terminate called after throwing an instance of 'nlohmann::json...type_error'`，RUN_EXIT=127 |
| 修复 | 新增 `parse_flag01()`（bool / 0-1 整数 / "0""1" 字符串），非法值返回 `GGERR_BAD_REQUEST` |
| 回归 | `test_session.cpp` 新增 f2 组 4 条断言（`"abc"`、`2` 被拒；`1` 放行）。**旧版下该组直接崩进程，新版 ALL PASS** |

---

## P2

### F-06 负数字符串地址被当成巨大正地址

| 项 | 内容 |
|---|---|
| 位置 | `server/session.cpp` `parse_addr`；`agentd/main.c` `parse_u64_strict`（dispatch 集中门使用） |
| 触发条件 | `{"addr": "-1"}` |
| 根因 | `strtoull` 接受前导 `-` 并做模运算，不设 `ERANGE`。实测 `"-1" → 18446744073709551615, errno=0`、`"-0x10" → 0xFFFFFFFFFFFFFFF0` |
| 影响 | 写错的负地址被当成合法巨地址；错误信息变成"读失败"而非"参数非法" |
| 修复 | 两处解析器跳过前导空白后遇到 `-` 直接判非法 |
| 回归 | `test_session.cpp` f1 组 4 条（`-1` / `-0x10` / `gg_read_bytes` 三种入口）。旧版行为已实测确认会落到 `device_call` |

### F-07 `h_read_bytes` 的 `len` 先 cast 再校验

| 项 | 内容 |
|---|---|
| 位置 | `agentd/main.c` — `h_read_bytes` |
| 触发条件 | `{"len": -1}` 或 `{"len": 0}` 或 `{"len": "abc"}`（`valuedouble` 取到 0） |
| 根因 | `(size_t)valuedouble` 先转换后判断，负值变巨大值、0 变 0 长度；只靠 `len > GG_SEARCH_CHUNK` 钳制兜底 |
| 影响 | 负 len 变成 256KB 静默读；0 长度以 0 调驱动 |
| 修复 | 先按 double 判 `len >= 1`，再转换 |

### F-08 `lint.bat` 恒返回成功

| 项 | 内容 |
|---|---|
| 位置 | `scripts/lint.bat:39` `exit /b 0`（无条件） |
| 触发条件 | cppcheck 报任何缺陷 |
| 影响 | 静态分析失败被打印后吞掉，CI 保持绿 —— "错误地忽略失败" |
| 修复 | cppcheck 加 `--error-exitcode=2` + 仅对 `third_party` 抑制 `syntaxError`；`errorlevel 2` → `exit /b 1` |
| 回归 | 负向验证：对故意越界的临时文件跑 cppcheck，`EXIT=2`；对当前 agentd `EXIT=0` |

### F-09 `test_all.bat` 会执行源码已删除的陈旧测试二进制

| 项 | 内容 |
|---|---|
| 位置 | `scripts/test_all.bat:32` `for %%t in ("%ROOT%\build\pc\test_*.exe")` |
| 触发条件 | `build\pc\` 里存在任何历史产物 |
| 证据 | `build\pc\test_ko_downgrade.exe` 存在，而 `git ls-files` 无 `tests/unit/test_ko_downgrade.c`（随 GG 核心链路一并删除） |
| 影响 | 用当前源码无法重建的二进制给出"通过"结论；旧二进制还可能因源码演进误报失败 |
| 修复 | `[1/4]` 改为 `call tests\unit\run_tests.bat`（单一事实源：重建 + 运行） |

### F-10 `max_save_bytes` 配置只校验 `== 0`

| 项 | 内容 |
|---|---|
| 位置 | `server/main.cpp:84` |
| 触发条件 | 配置写 `"max_save_bytes": -1` |
| 根因 | 负值通过校验；后续 `len > max_save_bytes` 恒真 |
| 影响 | 配置错误静默变成"该功能全部拒绝"，没有任何提示 |
| 修复 | 改为 `<= 0` |

### F-16 stdio 冒烟测试的"无设备"用例依赖外部环境，有设备在线时假失败

| 项 | 内容 |
|---|---|
| 位置 | `tests/regression/test_stdio_smoke.py` — 用例 5（`gg_list_processes` 期望 NO_DEVICE） |
| 触发条件 | 本机 adb 有设备 + forward 已建立 + 设备上 agentd 在跑 |
| 根因 | 脚本直接用仓库配置 `configs/gg-ghost.json`（`agent_port=34127`），"无设备"是被动假设，不是构造出来的状态 |
| 证据 | 修复前 `test_all.bat` 在 [2/4] 就失败：`FAIL: no-device call returns an error -> {"processes":[{"comm":"init",...}]}` —— 它真的连上了设备并返回了进程列表 |
| 影响 | 有设备时整套测试必红，掩盖真正的回归；无设备时才是绿的，测试结论随环境漂移 |
| 修复 | 生成临时隔离配置（`agent_port=9`，discard 端口）再启动 server，把"无设备"变成确定性路径 |
| 回归 | 设备在线状态下复跑：18 passed / 0 failed |

---

## P3

| 编号 | 位置 | 问题 | 状态 |
|---|---|---|---|
| F-11 | `agentd/main.c` `h_read`/`h_write`/`h_freeze` | `vt_size(VT_BYTES) == 0`，用 `type: "bytes"` 做单值读写/冻结会以 0 长度调驱动，再把零值当结果 | ✅ 三处都判 `sz <= 0` 并提示改用 `gg_read_bytes` |
| F-12 | `server/device/wire.cpp` `write_frame` | `enc_on_` 且 payload 为空时对 `&enc[0]` 取址（C++11 起对空 string 取非 const 指针是 UB） | ✅ 增加 `!enc.empty()` |
| F-13 | `agentd/main.c` `ct_equal` | 长度不等立即返回，泄露密钥长度 | ⏸ 未修：密钥固定 64 hex，实际泄露量为 0；改成定长比较会引入"长度也不一致"的新分支，收益不抵风险 |
| F-14 | `tests/unit/run_tests.bat:60-64` | 仍按 `build\native\libKernelGg.so` / `lib5_stub` 分支跑 `test_elfsym`，而这两个产物已随 GG 核心链路删除，真实产物断言长期走空 | ✅ 删除该分支，改为单一路径 |
| F-15 | `tests/integration/run_itest.ps1` | itest 的中文输出经 adb 回传后按 GBK 解释，显示为乱码（`寮曟搸闆嗘垚娴嬭瘯`） | ⏸ 未修：只是显示问题，断言走退出码。修法是在 itest.c 侧统一输出 ASCII 或让 PS 侧按 UTF-8 解码 |

---

## 未修总表

| 编号 | 原因 |
|---|---|
| F-13 | 风险/收益不符，且改动会新增分支 |
| F-15 | 纯显示问题，不影响判定 |
| F-02 / F-03 / F-04 | 已修，但**无自动化回归**（static handler / 真 fd / 并发 server 不可在 PC 侧 mock）；已在 FINAL_REPORT 列为"需真机或后续补" |
| 设备端功能闭环 | 设备 KO 是 hook 型，与 rwproc33-only 代码不匹配，itest 无法通过（见 BASELINE §4.1） |
