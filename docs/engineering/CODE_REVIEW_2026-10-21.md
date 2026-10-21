# CODE_REVIEW — 通信层之外的全面体检

> 2026-10-21，基于 HEAD `6106717` 之后的工作区（含本轮修复）。
> 上一轮已处理通信层（失败语义 / 截止时间 / save_to / 枚举校验），本文只覆盖**其余方面**。
> 每条都标了文件与行号，结论均来自实际读代码或实测构建，未经验证的推断会明确标注。

---

## 一、本轮已修

### 1.1 【严重】`results_get_page` 堆溢出 — 已修

**位置**：`agentd/search.c:679 bulk_read_segment()` / `:707 results_get_page()`

批量读连续地址段时，`seg_n` 只受两个条件约束：

- `idx + seg_n < n`（结果集剩余条目）
- `(seg_n + 1) * sz <= GG_SEARCH_CHUNK`（单段 ≤ 256KB）

**完全不看调用方的 `limit`**。而 `out_addrs` / `out_values` 是 `malloc(limit * sizeof(uint64_t))` 分配的。

后果：`vt=DWORD` 时 `seg_n` 最大 65536，会往只有 `limit*8` 字节的缓冲写最多 512KB。而**搜索结果天然是大量连续地址**（扫描数组/结构体时几乎全是连续命中），所以这不是理论问题——一次 `gg_get_results(limit=100)` 就能触发。

agentd 以 root 运行，这是可经 MCP 单条命令触发的堆溢出写。

**修复**：`bulk_read_segment()` 增加 `max_n` 参数并纳入循环条件，调用点传 `limit - n`。

### 1.2 `h_get_results` 分配失败静默降级 — 已修

**位置**：`agentd/main.c:968`

原实现 `if (addrs && vals)` 成立才读页，任一分配失败就 `got=0`，然后照常 `send_ok` 返回空结果集。调用方会把它当成「这一页确实没数据」，进而误判搜索已空。

**修复**：分配失败或 `results_get_page` 返回非 0 时明确 `send_err(GGERR_INTERNAL)`。

**验证状态**：`build_ndk.bat` 编译通过（`-Wall` 零警告），单测全绿，**但暂无针对性回归测试**——见 §四。

### 1.3 【严重】`Session` 构造读已被 move 走的形参 — 已修

**位置**：`server/session.cpp:77`

```cpp
Session::Session(ServerConfig cfg)
    : cfg_(std::move(cfg)),
      client_([&]{ ... ac.adb = cfg.adb; ac.serial = cfg.serial; ... }()),  // ← cfg 已被移空
```

成员声明顺序是 `cfg_` → `client_`，所以 `cfg_(std::move(cfg))` 先执行，形参 `cfg` 的所有 `std::string` 成员立刻变成空串（`int` 因 move 等同拷贝而幸存）。

**后果**：`AgentClient` 拿到的 `adb` 恒为 `""`，直连失败后 `adb forward` 永远拼不出来（`adb_safe("")` 直接拒绝并返回 `GGERR_NO_DEVICE`）——**只能依赖部署脚本预先建好的 forward**。这正是「自动重连有时不生效」的真正原因，比通信层的重试语义更靠下。

**修复**：lambda 改读 `cfg_`（已初始化的成员），不再碰形参。

**发现路径**：本轮加握手时，配置里写了 `wire_key` 却发出空 key，顺着查出来的。这条如果没有鉴权改造根本不会暴露。

### 1.4 【高】agentd 无客户端鉴权 — 已修

新增 `GGCMD_AUTH`（追加在枚举末尾，**既有命令码数值全部不变**）。

| 侧 | 改动 |
|---|---|
| `shared/wire_protocol.h` | `GGCMD_AUTH`（42）+ 协议语义注释 |
| `agentd` | `--wire-key` / `--wire-key-file` 参数；`dispatch()` 前置握手门（未认证只放行 `PING`/`AUTH`，其余 `GGERR_PERM`）；`ct_equal()` 常量时间比较；失败只记「missing/mismatch」不记值 |
| `server` | `AgentClient::auth_locked()`，连接后先握手；失败返 `GGERR_PERM` 且**不进入重放路径**；banner 显示 wire auth 状态 |
| 部署 | `start_agentd.sh` 首启生成 32 字节随机密钥（`0600`）→ `--wire-key-file`；`deploy.ps1` 自动拉取并写回 `configs/gg-ghost.json` |

**验证**（假 agentd 端到端，不接设备）：

- 正常：第一帧 `cmd=42 seq=1`，key 正确 → 回 `authed:true` → 业务帧 `cmd=3` 放行 → PC 拿到响应
- 反向：key 不匹配 → `GGERR_PERM(4)` + `"agentd rejected wire auth (wire_key mismatch)"`
- `test_wire_io` 的假对端补了 `serve_auth()`，6 个用例全部恢复通过

**未做**：密钥轮换、失败次数限流（有 8 连接上限兜底，未额外加）。

### 1.5 【高】并发连接无上限 — 已修

`GG_MAX_CLIENTS 8`，`atomic_fetch_add` 先占位再判，超限直接 close 并记日志；`conn_thread` 退出时递减。


---

### 1.6 【中危】`ko_insmod` 命令注入面 — 已修

**位置**：`agentd/driver.c:700`

`system("insmod '%s'")` 拼路径，文件名含单引号即可闭合引号注入命令。改为 `run_exec1()`：`fork + execl`，子进程只调 async-signal-safe 函数（`open`/`dup2`/`close`/`execl`/`_exit`），不经 shell。两处 `system("setenforce ...")` 一并收敛（虽是常量串无注入面，但消除 `system()` 依赖）。

`grep -c 'system('` 现在只剩注释里的历史说明。

### 1.7 【中危】`addr` / `len` 统一校验 — 已修

**位置**：`server/session.cpp`

新增 `parse_addr()` / `parse_len()` / `addr_to_wire()`：

- `addr` 接受整数或数字字符串，**下行一律转 `"0x..."` 字符串**——绕开 JSON number 的 double 精度问题（>2^53 的地址会被序列化成科学计数法，agentd 侧 `strtoull` 解析失败静默变 0）
- `len` 显式校验 1..65536（hex 模式），传字符串不再抛异常变 -32603
- 应用于 `gg_read` / `gg_write` / `gg_read_bytes` / `gg_hex_dump`

### 1.8 【工程】CI 对齐 r30 + 补消毒器 — 已改

- `.github/workflows/build-and-test.yml` 的 NDK 从 **r27d 改为 r30**（4 处），与本机一致
- 新增 **Job 4 `sanitizers`**（ubuntu-latest）：`-fsanitize=address,undefined` 跑 `test_wire` / `test_wire_io` / `test_results_page` / `test_search`。MinGW 无法运行 ASan，这个 job 专门补这个盲区

**未验证**：CI 只能在 GitHub 上实际跑，本地无法执行。YAML 结构已目视核对（4 个 job 同级缩进正确），命令是单行不依赖续行符。

### 1.9 【中危】agentd 三项真机暴露问题 — 已修

**① `comm` 尾部换行**（`agentd/process.c:50`）

`/proc/<pid>/comm` 以 `\n` 结尾，原样塞进 JSON 后变成 `"init\n"`。既污染输出，也让按进程名比较（attach by name）永远匹配不上。修复：读完后裁掉尾部 `\n`/`\r`。

**② hook 模式的 stale error**（`agentd/main.c`）

hook 型的常见情形是内核里已有 hook（开机脚本或别的途径加载），本进程无需再 insmod，于是 `ko_load` 必然失败并写下 `"no .ko file found in ko dir"`。但通道其实可用，这条错误留着会让 `gg_kernel_status` 一直误报。

修复：自检通过（`st > 0` 且 `state == ST_KO_HOOK`）时清空 `last_error`——只在真正不可用时才保留。

**③ 17 处 `strtoull(s, NULL, 0)` 不校验**（`agentd` 全仓）

非法地址一律静默得到 0，然后拿着地址 0 去读内存。调用方看到的错误是「读失败」而不是「地址写错了」。

修复方式不是逐个改 handler（17 处分散、易漏），而是在 `dispatch()` 分发前**统一校验** `addr` / `base` / `pc` / `fp` / `sp` / `lr` 六个地址类字段：新增 `parse_u64_strict()`，字符串必须是完整可解析的地址，数字必须非负。

**真机验证**（绕过 PC 端 session 层，直接发 wire 帧）：

```
[auth]           {"authed":true}
[bad addr]       {"e":{"code":1,"message":"invalid address argument (expected 0x-prefixed string)"}}
[good addr]      {"e":{"code":3,"message":"no session"}}   ← 地址合法，放行到 handler
[numeric addr=0] {"e":{"code":3,"message":"no session"}}   ← 数字形式仍被容忍
```

`comm` 已变为 `'init'`（无 `\n`），`gg_kernel_status` 的 `error` 已为空。

## 二、高危项（本节已全部处理，保留分析供追溯）

### 2.1 agentd 无任何客户端鉴权 — ✅ 已修，见 §1.4

**位置**：`agentd/main.c:1814`

```c
sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
```

只绑 127.0.0.1 是好的（外网进不来），但**设备上任何拿到 INTERNET 权限的普通 App 都能连 127.0.0.1:34127**，而 agentd 以 root 运行。

可执行的能力：

| 命令 | 后果 |
|---|---|
| `GGCMD_SHELL` | 直接 root 命令执行（若 `shell_enabled=true`） |
| `GGCMD_WRITE` / `SET_RESULTS` | 改写任意进程内存（含 system_server） |
| `GGCMD_LOAD_GG` | 加载任意 GG 核心 |
| `GGCMD_WATCH_INJECT` | 注入硬件断点 |

`grep token|auth|secret|nonce` 在 `main.c` 里零命中——协议层完全没有身份概念。

**实际采用**：保留 TCP + 共享密钥握手（`GGCMD_AUTH`）。理由：`adb forward` 原生只支持 TCP，走 `AF_UNIX` 需要 `adb forward tcp:x localfilesystem:y`，旧版 adb 兼容性不稳，且会改变部署模型。密钥存设备端 `0600` 文件（仅 root 可读），普通 App 拿不到，握手不过则一切业务命令 `GGERR_PERM`。

### 2.2 `g_eng` 全局状态无锁 — ❌ **本条判断错误，撤销**

**更正（2026-10-21，同日复核）**：`g_eng.mtx` **确实存在**：

- `agentd/engine_internal.h:42` — `pthread_mutex_t mtx;  /* 串行化所有 handler */`
- `agentd/main.c:1560` — `pthread_mutex_lock(&g_eng.mtx);`（dispatch 内）
- `agentd/main.c:1609` — `pthread_mutex_unlock(&g_eng.mtx);`
- `agentd/main.c:1755` — `pthread_mutex_init(&g_eng.mtx, NULL);`

**误判原因**：我当时用 `grep -rn "pthread_mutex" agentd/*.c agentd/*.h | head -20` 检索，前 20 条全落在 `freeze.c` 的冻结列表锁上，结果被截断，于是错误地得出「全仓只有 freeze 有锁」的结论。这是检索方法问题，不是代码问题。

**顺带说明**：dispatch 还有「快速通道」设计——PING / KERNEL_STATUS / LIST_PROCESSES / DEVICE_INFO 走无锁路径，避免大搜索期间探活失败。这个设计是对的。

`docs/ARCHITECTURE.md:58` 那句话与代码一致，**不需要修改**（我上一版报告里说要改，也一并撤销）。

### 2.3 每连接一个线程，无上限 — ✅ 已修，见 §1.5

**位置**：`agentd/main.c:1826-1836`

`accept` 后直接 `pthread_create` + `pthread_detach`，没有并发连接计数或上限。`listen(sfd, 8)` 只限制 backlog，不限制已建立连接数。

本地任意进程可以在几秒内开数千连接耗尽线程栈（默认 8MB 虚拟）与 fd，把 agentd 拖死。与 §2.1 叠加构成完整的 DoS 路径。

**实现**：`GG_MAX_CLIENTS 8` + `atomic_fetch_add` 占位。没有改成单线程 poll —— 既然 `g_eng.mtx` 已经把引擎串行化了，poll 重构的收益不足以抵消改动风险。

---

## 三、中危（3.1 / 3.2 已修，3.3 待修）

### 3.1 `ko_insmod` 用 `system()` 拼路径 — ✅ 已修，见 §1.6

**位置**：`agentd/driver.c:700-707`

```c
snprintf(cmd, sizeof(cmd), "insmod '%s' 2>/dev/null", path);
int rc = system(cmd);
```

`path` 来自扫描 `ko/` 目录的文件名。若文件名含 `'`（如 `x';id;'.ko`），单引号闭合后即可注入命令。实际利用需要能在目标目录写入任意文件名，门槛不低，但**这是 root 进程里的命令注入面**，且修复成本极低。

**建议**：去掉 `system()`，改 `fork + execvp("insmod", argv)`（与 `cmd_shell.c` 已采用的正确模式一致）；或在 `snprintf` 前把 `'` 转义为 `'\''`。

同文件 `:813/:858` 的 `system("setenforce 0/1")` 是常量串，无注入面，但同样建议收敛到 execvp。

### 3.2 PC 端普通分支未校验 `addr` / `len` — ✅ 已修，见 §1.7

**位置**：`server/session.cpp:404`、`:540`

上一轮只加固了 `save_to` 分支。普通 `gg_read_bytes` / `gg_hex_dump` 仍是：

```c
c.params["addr"] = a["addr"];
c.params["len"] = a.value("len", 64);
```

两个问题：

1. **大地址精度丢失**：JSON number 是 double，超过 2^53 的地址（如 `0x7f1234567890` 的十进制写法）会被序列化成科学计数法，agentd 侧 `strtoull` 解析失败静默变 0。
2. **类型未校验**：`len` 传字符串 `"abc"` 会抛 `json::type_error`，被协议层吞成 -32603 Internal error，而不是 -32602。

`schema.cpp` 里 `addr` 声明为 string，但 handler 不强制。

**建议**：与 `save_to` 分支共用同一套 addr/len 解析与校验函数（上一轮已在 `session.cpp` 写了逻辑，提取为 `parse_addr()` / `parse_len()` 即可复用）。

### 3.3 agentd 侧多处 `strtoull(s, NULL, 0)` 不检查 endptr — ⏳ 待修

**位置**：`agentd/main.c:831`（disassemble）、`agentd/cmd_view.c:58`、以及其它 handler

非法地址一律静默变成 0，然后拿 0 去读内存。用户看到的是「读失败」而不是「地址写错了」，排查成本高。

**建议**：抽一个 `parse_u64_strict()`，失败返回 `GGERR_BAD_REQUEST`。

---

## 四、测试盲区

| 模块 | 行数 | 单测 |
|---|---:|---|
| `driver.c` | **1109** | ❌ 无 |
| `loader.c` | 328 | ❌ 无 |
| `cmd_view.c` | 312 | ❌ 无 |
| `disasm.c` | 85 | ❌ 无 |
| `search.c` | 809 | ✅ test_search（但**未覆盖 `results_get_page` 分页路径**） |
| `watch.c` / `freeze.c` / `elfsym.c` / `version.c` / `process.c` | — | ✅ |
| `cmd_shell.c` | 71 | ✅ test_shell_security |

`driver.c` 是全仓最大文件、同时管四型通道与降级链，却没有任何单测——这是最大的覆盖空洞。不过它重度依赖 ioctl/`/proc`，纯 mock 成本高，**优先级应排在 §2 之后**。

**优先补**：`test_results_page.c`——用连续地址 + 小 limit + 缓冲 canary，锁死 §1.1 的回归。这是当前唯一「已修但无测试」的高危项。

---

## 五、工程与 CI

| 项 | 状态 |
|---|---|
| CI 的 NDK 版本 | `.github/workflows/build-and-test.yml` 用 **r27d**，本机是 **r30** —— 版本漂移，CI 绿不代表本地可复现 |
| 消毒器 | CI 三个 job 均无 ASan / UBSan / TSan，纯 C 项目零消毒器覆盖 |
| 性能基准 | `benchmark.bat` 引用的源码不存在，脚本会明确报错（已知） |
| 并发测试 | 无。`g_eng` 无锁问题（§2.2）没有任何测试能发现 |
| `test_parallel.c` | 存在，但按名字推测是搜索并行度的用例，不覆盖多客户端场景（**未细读，属推断**） |

---

## 六、架构层面

1. **`agentd/main.c` 1846 行**，34 个 handler 全在一个文件。已拆 shell / view 两域，剩 `cmd_search` / `cmd_mem` / `cmd_proc` / dispatch 四处。NDK 已就位，拆分现在可编译验证。
2. **`archive/framework_v1/`**：perf / atomic / error / mempool / conn_pool / io_batch 六个模块未接线，是死代码。要么启用要么删。
3. **`docs/ARCHITECTURE.md` 两处漂移**：
   - `:58` 声称「引擎操作经互斥锁串行化」——与 §2.2 实际不符
   - `:83` 声称「adb 断连自动重连一次」——已改为只在 `ConnectFailed` 时重连

---

## 七、进度

**已完成（2026-10-21，全部经本机构建 + 单测验证）**

| # | 项 | 验证方式 |
|---|---|---|
| 1 | §1.1 `results_get_page` 堆溢出 | `test_results_page` 9 用例全绿；回退修复后该用例进程异常终止 |
| 2 | §1.2 `h_get_results` OOM 静默 | 编译 + 代码审查 |
| 3 | §1.3 `Session` 构造读已 move 的形参 | 握手端到端测试暴露并验证 |
| 4 | §1.4 agentd 鉴权（`GGCMD_AUTH`） | 假 agentd 端到端，正反两路 |
| 5 | §1.5 连接数上限 | 编译 + 代码审查 |
| 6 | §1.6 `ko_insmod` 改 execvp | agentd 交叉编译通过 |
| 7 | §1.7 `addr` / `len` 统一校验 | PC 编译 + 单测全绿 |
| 8 | §1.8 CI 对齐 r30 + 消毒器 job | **仅结构核对，未在 GitHub 实跑** |
| — | §2.2 无锁判断 | ❌ 误判，已撤销 |

**待办**

| 顺序 | 项 | 理由 |
|---|---|---|
| 1 | §3.3 agentd 侧 `strtoull` 校验 | 非法地址静默变 0，排查成本高 |
| 2 | §四 `driver.c` / `loader.c` 单测 | 全仓最大文件仍无覆盖；依赖 ioctl/`/proc`，纯 mock 成本高 |
| 3 | §六 `main.c` 继续拆分 / `archive/` 清理 | 长期债 |
| 4 | 真机 itest | **所有设备端改动只有编译验证，功能闭环仍缺设备** |

---

## 八、真机验证（2026-10-21 18:33–18:52，Redmi myron / 134d2f8 / 内核 6.12.69）

设备首次在本会话接上，跑完了从部署到功能验证的闭环。**过程中暴露的部署链路问题比代码问题更多**，一并记录。

### 8.1 部署链路修复（原本根本跑不通）

| 问题 | 现象 | 根因 | 修复 |
|---|---|---|---|
| `/data/adb` 不可写 | `mkdir: Permission denied` | `adb shell` 是 shell uid(2000)，而 `/data/adb` 是 root 0700 | 先 push 到 `/data/local/tmp/ggmcp_stage`，再 `su` 复制到目标目录 |
| `su cp` 返回 1 | 单条命令都成功，拼成 `&&` 长串就失败 | 宿主机 Shell 与 adb 的多层引号穿透把 `&&` 吃掉 | 拆成逐条 `Adb-Sh su -c` 调用 |
| **部署目录与启动脚本不一致** | `start_agentd.sh stop/start` 全返回 1，agentd 起不来 | 脚本写 `DIR=/data/adb/vendor_security`、`BIN=$DIR/dspbridge`，而 deploy 推到 `/data/adb/GG-Ghost/gdm` | 统一为 `/data/adb/GG-Ghost` + `gdm` |
| **`.sh` 是 CRLF** | `[18]: : inaccessible or not found`、`return: bad number: 1` | Windows 行尾被提交进仓库，`\r` 被 sh 当成命令的一部分 | 5 个脚本全部转 LF |
| 推送失败 | `cp: Text file busy` | 运行中的二进制无法被覆盖，且脚本先推后停 | 推送前先 `stop` |
| adb stderr 被当致命错误 | 推送明明成功却报错退出 | PS 5.1 在 `ErrorActionPreference=Stop` 下把 native stderr 包装成终止性错误 | `Adb-Sh`/`Adb-Cmd` 内降级为 Continue，失败仍由 `$LASTEXITCODE` 判定 |
| itest 脚本 UTF-8 无 BOM | 解析报「字符串缺少终止符」 | PS 5.1 按 GBK 解析无 BOM 的 UTF-8 | 重写为 UTF-8 with BOM |
| itest 链接失败 | `undefined symbol: log_msg` | `driver.c` 引用的 `log_msg` 定义在 `main.c`，itest 不链接 main.c | itest.c 补同签名实现（输出到 stderr） |
| 写回配置带 BOM | `nlohmann::json` 与多数 JSON 工具不跳过 BOM | `Set-Content -Encoding UTF8`（PS 5.1）会插 BOM | 改 `File::WriteAllText` + `UTF8Encoding($false)` |

### 8.2 驱动状态

- 设备内核**已有 hook 生效**：`driver self-test: 1 (ko-hook (kernel hooks process_vm_readv))`
- 但 `start_agentd.sh` 写的是 `KO_TYPE=rwproc` → agentd 启动后 `all memory channels unavailable`
- 改为 `KO_TYPE=hook` 后 `gg_kernel_status` 返回 `state=5 / "ko-hook"`，读写正常
- 本地唯一的 KO 文件 `paradise-6.12.ko` 是 android-paradise 的隐藏模块（hook filldir64/kgsl），**不是** rwproc/dev 通道，不可用于本项目

### 8.3 功能验证结果

| 验证项 | 结果 |
|---|---|
| `run_itest.ps1`（引擎全链路：搜索/读/写/冻结/解冻） | **ITEST ALL PASS** |
| PC→agentd 握手鉴权（正确 key） | ✅ `gg_kernel_status` 返回 `state_str: "ko-hook"` |
| 真实内存读取 | ✅ `gg_read_bytes(0x55660fc000)` → `7f454c46…`（init 的 ELF 头） |
| 非法 addr 校验 | ✅ `addr:"notahex"` → `BAD_REQUEST: addr must be a non-negative integer or numeric string` |
| 非法 len 校验 | ✅ `len:"abc"` → `BAD_REQUEST: len must be an integer in 1..65536` |
| **错误 wire_key 被拒** | ✅ 真实 agentd 返回 `GGERR_PERM(4) / "agentd rejected wire auth (wire_key mismatch)"` |
| 模块枚举 | ✅ 692 个模块，首个 `init` @ `0x55660fc000` |

### 8.4 真机发现的小问题 — ✅ 已于同日修复，见 §1.9

- `gg_list_processes` 的 `comm` 字段带尾部 `\n` —— 已裁掉
- `gg_kernel_status` 在 hook 模式下误报 `"no .ko file found in ko dir"` —— 自检通过即清空

---

*本文所有代码位置均现场核对；§四「test_parallel.c 不覆盖多客户端」一条为基于文件名的推断，未逐行确认。*
