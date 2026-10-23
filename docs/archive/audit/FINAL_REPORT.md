# FINAL REPORT — gg-mcp 全面审查（2026-10-21）

> 分支：`audit/full-review`（已 rebase 到 `master` @ `4b0a4d7`，无冲突）
> 审查基线为 `dddda9b`；rebase 后重新执行了构建、单元测试与冒烟测试，全部通过
> 审查范围：PC server（C++17）、agentd（C, NDK arm64）、wire 协议、MCP 服务层、构建/测试体系
> 说明：`fix/mem-io-integrity` 分支基于旧代码（落后 20+ 提交，且其修改的 hook/dev/qx 三型驱动已被删除），
> 本次未合并、未使用。

## 1. 修复清单

| 编号 | 级别 | 问题 | 修复 | 验证 |
|---|---|---|---|---|
| F-01 | **P0** | `rwproc_buf` 进程级共享缓冲在锁外被搜索 worker 池与冻结线程并发使用 → 请求/结果互相覆盖，数据静默错误 | `rwproc_call_ex` 增加 `out/out_cap`，请求写入 + `read()` + 结果拷出进同一临界区；8 处调用点改为由 out 取结果 | 新护栏 `check_rwproc_locking.py` 通过；负向验证（删 lock → FAIL） |
| F-02 | P1 | `h_write` 把短写当成功 | `w != vt_size(type)` → 报错 | 编译 + 代码审查；**无自动化用例**（static handler + 真驱动） |
| F-03 | P1 | `h_read` 把短读当成"值是 0" | `got != sz` → 报错 | 同上 |
| F-04 | P1 | `Session::mtx_` 是死成员，HTTP 并发下 `last_error`/`last_outcome` 数据竞争、`close+replay` 互掐连接 | `call_tool` 全程持锁；`local_status` 注明必须在锁内 | 编译 + 代码审查；**无并发压测** |
| F-05 | P1 | `force` 类型不符 → `json::type_error` 未捕获 → **进程终止（实测退出码 127）** | 新增 `parse_flag01()`，非法值返回 `GGERR_BAD_REQUEST` | 新增 4 条断言；旧代码同一测试实测崩溃 |
| F-06 | P2 | 负数字符串地址被 `strtoull` 模运算成 `0xFFFF...` | `parse_addr` / `parse_u64_strict` 拒绝前导 `-` | 新增 4 条断言；`strtoull` 行为已实测 |
| F-07 | P2 | `h_read_bytes` 的 len 先 cast 再校验（负数变巨大、0 变 0 长） | 先按 double 判 `>= 1` 再转换 | 编译 + 代码审查 |
| F-08 | P2 | `lint.bat` 恒 `exit /b 0`，cppcheck 失败被吞（CI 假绿） | `--error-exitcode=2` + 仅抑制 third_party；改用 goto 结构传播失败 | 结构副本实测 RC=1；负向 cppcheck EXIT=2 |
| F-09 | P2 | `test_all.bat` 扫描 `build\pc\test_*.exe`，会跑源码已删除的 `test_ko_downgrade.exe` | `[1/4]` 改为 `call tests\unit\run_tests.bat`（单一事实源） | 脚本审查；见 §4 test_all 结果 |
| F-10 | P2 | `max_save_bytes` 只校验 `== 0`，负数通过 | `< = 0` | 编译 |
| F-11 | P3 | `VT_BYTES`（`vt_size==0`）可用于单值读写/冻结 | `h_read`/`h_write`/`h_freeze` 判 `sz <= 0` | 编译 |
| F-12 | P3 | `write_frame` 空 payload 时 `&enc[0]`（UB） | 增加 `!enc.empty()` | 编译 |
| F-14 | P3 | `run_tests.bat` 仍按已删除的 `libKernelGg.so`/`lib5_stub` 分支跑 elfsym | 删除死分支 | 单测全绿 |
| F-16 | P2 | 冒烟测试的"无设备"用例依赖外部环境：有设备在线时 `gg_list_processes` 成功 → 用例假失败 | 生成隔离配置（`agent_port=9`）再启动 server | 设备在线状态下复跑 18/18 |
| 附 | P3 | `wire.cpp:462` 把 `#ifdef` 放进 `GG_LOG` 宏参数（cppcheck `directiveAsMacroParameter`） | 平台错误码先取局部变量 | 编译 |

## 2. 未修复

| 编号 | 原因 |
|---|---|
| F-13 | `ct_equal` 长度不等即返回，泄露密钥长度。密钥固定 64 hex，实际泄露 0；改造会新增分支，收益不抵风险 |
| F-15 | itest 中文输出经 adb 回传按 GBK 解释显示乱码。纯显示问题，断言走退出码 |
| F-02 / F-03 / F-04 的自动化回归 | `h_read`/`h_write` 是 `main.c` 的 static handler，`driver_read/write` 为真实现（需真实 fd）；F-04 需起 HTTP server + 假 agentd。三者都缺可注入点，**标注为需真机/后续补，不冒充已覆盖** |

## 3. 验证矩阵（全部为实际执行）

| 项 | 命令 | 结果 |
|---|---|---|
| PC 构建 | `scripts\build_pc.bat` | ✅ 成功 |
| Android 构建 | `scripts\build_ndk.bat` | ✅ 成功（`build\agentd\gg-agentd`） |
| 单元测试 | `tests\unit\run_tests.bat` | ✅ ALL PASS（12 个可执行 + 3 道结构护栏） |
| 结构护栏 · dispatch | `check_dispatch_consistency.py` | ✅ 43 工具一致 |
| 结构护栏 · kernel-only | `check_kernel_only.py` | ✅ |
| 结构护栏 · **rwproc locking（新增）** | `check_rwproc_locking.py` | ✅ 3 个 unlock 点 / 5 处引用在临界区内 |
| stdio 端到端冒烟 | `tests\regression\test_stdio_smoke.py` | ✅ 18 passed / 0 failed |
| 静态分析 | `lint.bat` → cppcheck | ⚠️ **完整跑未完成**（一次用户中断、一次超时）。分项结论：agentd 段 `EXIT=0`；自有代码唯一告警是 `wire.cpp` 的宏参数 #ifdef，已修；`lint.bat` 失败传播用结构副本实测 `RC=1` |
| 完整测试套件 | `scripts\test_all.bat` | ✅ **RC=0**，四阶段：单元 ✅ / 冒烟 18/18 ✅ / 真机集成 ✅ / 基准 SKIP |
| 真机集成测试 | `tests\integration\run_itest.ps1` | ✅ **ITEST ALL PASS** |

### rebase 后复验（master 4b0a4d7）

审查期间 `master` 前进了一个提交（`4b0a4d7` hwbp KO 脚本/文档，与本次改动无重叠）。rebase 后重跑：

| 项 | 结果 |
|---|---|
| `build_pc.bat` | ✅ |
| `build_ndk.bat` | ✅ |
| `run_tests.bat` | ✅ RC=0（含三道结构护栏） |
| `test_stdio_smoke.py` | ✅ RC=0，18/18 |

### 真机结果的两次记录（如实保留）

1. **首次（23:0x）**：`rwproc_attach failed: open /proc/e84523d7.../e84523d7... failed: No such file or directory` → FAIL。
2. **第二次（23:21）**：`driver self-test: 1 (kernel-driver (rwproc33))`，全部断言通过。

两次之间**本机没有做任何部署操作**（未执行 `deploy.ps1`，未 push agentd）。事后在设备上确认：
`/proc/e84523d7b60d5d341a7c4d1861773ecd/e84523d7b60d5d341a7c4d1861773ecd` 现在存在，
`/proc/modules` 里查不到对应模块（模块已隐藏）。即设备侧 rwproc33 模块在两次运行之间变成了已加载状态，
触发原因不在本仓库范围内，未能确定。

因此：**设备端链路的结论以第二次为准（通过）**，但第一次的失败记录保留 —— 它说明这套 itest 对
"模块是否已加载"是强依赖的，冷启动设备上会直接失败。

## 4. 回归测试的有效性

新增用例在**修复前**确实会失败（不是摆设）：

| 用例 | 修复前行为 | 修复后 |
|---|---|---|
| `test_session` f1（`addr="-1"`） | 未拦截，一路走到 `device_call`（日志出现 `[wire] connect failed`） | `GGERR_BAD_REQUEST` |
| `test_session` f2（`force="abc"`） | `terminate called after throwing nlohmann::json type_error`，**RUN_EXIT=127** | `GGERR_BAD_REQUEST`，`force=1` 正常放行 |
| `check_rwproc_locking` | 删掉 lock 行 → `no pthread_mutex_lock` 失败 | 通过 |
| `lint.bat` 失败传播 | 打印 `[FAIL]` 但返回 0 | 结构副本实测返回 1 |

## 5. 剩余风险

1. **F-01 的并发竞争没有被断言验证**。itest 在真机上跑通了 search/read/write/freeze（其中 freeze 线程与主线程交替访问驱动），
   全程无崩溃、数值正确，但这只是"没暴露"，不是"证明无竞争" —— itest 没有并发断言。
   要真正闭环，需要新增"并行搜索的同时保持冻结，逐条比对命中值与实际内存"的用例。
2. **F-02/F-03 的异常路径未构造**。真机跑的是正常读写（4/4 字节全成功），短读/短写的分支没有被触发过。
   语义收紧（短读短写从静默成功改为报错）的方向正确，但**如果 rwproc 对正常读也返回 `r < len`**，
   会从"能读到部分"变成"整条失败" —— 需要在真机上用一个必然跨页/不可读的地址验证一次。
3. **`rwproc_read_mem` 从"截断"改为"拒绝"**：`r > len` 现在直接失败。真机正常路径（r == len）未受影响，
   但异常路径未验证。若确有多报字节的驱动行为，需回退为截断 + 警告。
4. F-04 的会话锁让 HTTP 模式完全串行。设备端本来就单连接，理论上无损失，但未压测。
5. `build\pc\` 里仍有历史产物（`test_ko_downgrade.exe` 等）。本次改为委托 `run_tests.bat` 绕开了问题，
   但没有清理目录 —— 若有人回退 `test_all` 的写法，问题会复现。
6. **lint 的全量执行未完成**（一次用户中断、一次超时）。已用分项与结构副本验证，但 `lint.bat` 端到端的
   "失败 → 退出码 1" 没有在完整脚本上跑过一次。

## 6. 交付物

| 文件 | 内容 |
|---|---|
| `docs/archive/audit/BASELINE.md` | 环境、分支、构建与测试基线（阶段 A） |
| `docs/archive/audit/FINDINGS.md` | 15 条缺陷的完整清单（阶段 B） |
| `docs/archive/audit/FINAL_REPORT.md` | 本文件（阶段 E） |
| `tests/unit/check_rwproc_locking.py` | 新增结构护栏（F-01） |
| `tests/unit/test_session.cpp` | 新增 f1/f2 两组回归（F-05/F-06） |
| `tests/regression/test_stdio_smoke.py` | 用例隔离改造（F-16） |
| 代码改动 10 个文件 | `agentd/driver.c`、`agentd/main.c`、`server/session.cpp`、`server/device/wire.cpp`、`server/main.cpp`、`scripts/lint.bat`、`scripts/test_all.bat`、`tests/unit/run_tests.bat`、`tests/unit/test_session.cpp`、`tests/regression/test_stdio_smoke.py` |

**未执行**：`scripts\benchmark.bat`（仓内无基准源码，已知债）、`scripts\deploy.ps1`（会覆盖设备现有 agentd，需授权）。
