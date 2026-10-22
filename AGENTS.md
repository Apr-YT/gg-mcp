# AGENTS.md — GG-MCP 工作区指令（每个会话自动加载）

## 项目一句话

内核驱动式内存修改 MCP 服务：PC 上的 AI agent 通过 MCP 直接读写安卓游戏进程内存。
**只走 KO 内核驱动通道**（QX / rwproc33 / hook / dev 四型），GG 核心采用外挂圈子加载（ELF 桩 + hook syscall 270）。

## 项目结构

```
server/    PC 端 gg-ghost-mcp（C++17）：MCP 协议 + 会话 + wire 客户端
agentd/    设备端 gg-agentd（C, NDK arm64, root）：搜索/读写/冻结/监视 + 驱动对接
shared/    wire_protocol.h — PC/设备双端协议唯一事实源（20B 头 + JSON 负载；含 GGCMD_AUTH 握手）
tests/     unit（posix_stub 可 PC 跑）/ integration / regression 三层
scripts/   构建（build_pc.bat / build_ndk.bat / build_all.bat）、部署、测试、基准
configs/   gg-ghost.example.json（模板，入库）；gg-ghost.json 由 deploy 生成、**不入库**
tools/     辅助脚本（cfm_coord_read.py 等）
docs/      engineering/（工程文档）+ research/（逆向研究笔记，仅限本仓），索引见 docs/README.md
```

## 构建与验证

```powershell
# PC server（MinGW g++，-O3 -flto；本机实测 16.2.0）
scripts\build_pc.bat              # 或 scripts\build_all.bat [-Target pc|android|all]

# 设备端（需 NDK r30 + capstone 5.0.1）
scripts\build_ndk.bat

# 运行
build\pc\gg-ghost-mcp.exe --config configs\gg-ghost.json

# 测试
tests\unit\run_tests.bat          # 单元 + 两道结构护栏（先跑这个）
scripts\test_all.bat              # 单元 + 集成 + 基准（缺前置会明确 SKIP）
scripts\benchmark.bat             # ⚠️ 仓内无基准源码，会直接报错退出

# 结构守护（拆分/改分发后必跑；run_tests.bat 已自动执行）
python tests\unit\check_dispatch_consistency.py
python tests\unit\check_kernel_only.py
```

> **本机工具链现状（2026-10-21 复测）**
> - MinGW g++ **16.2.0**（`D:\Kiln\mingw64`，已入 PATH）；Python 3.13 可用于两道结构护栏。
> - NDK 为 **r30**（`D:\Kiln\android-ndk-r30`，`Pkg.Revision = 30.0.16248370`）；
>   `toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android28-clang.cmd` 存在。
> - **capstone 5.0.1 已就位**：`D:\Kiln\capstone\capstone-5.0.1\libcapstone.a`（17:44 装入）。
> - **agentd 现已可本地完整编译**。实测 `scripts\build_ndk.bat` 全绿，产出
>   `build\agentd\gg-agentd`（2.4MB，ELF64 **AArch64 PIE**，仅 NEEDED `libc.so`/`libdl.so`，`-Wall` 零警告）。
> - ⚠️ **编译通过 ≠ 功能可用**：设备端改动仍必须过真机 itest（`deploy.bat` → `run_itest.ps1`）才算完成。
> - `run_tests.bat` 的护栏探测已修（此前是死代码）：找不到 python 会**直接 `exit /b 1`**，
>   不再静默跳过报假绿。确需跳过时显式设 `GG_SKIP_GUARDRAILS=1`；
>   或用 `GG_PYTHON=<python.exe 全路径>` 指定解释器。
> - **WSL2 已装（2026-10-21）**：`Ubuntu 24.04.5`（WSL 3.0.3 / 内核 6.18.54.1），
>   磁盘在 `D:\WSL\Ubuntu`，swap 在 `D:\WSL\swap.vhdx`，apt 走清华源，gcc 13.3 + make 4.3。
>   用途：跑 Windows 上无法验证的 POSIX 行为（如 `install_file` 的 rename 覆盖语义）。
>   ⚠️ **原写的「`run_tests.bat` 末尾自动调用 `tests/unit/run_tests_wsl.sh`」不成立**：
>   该脚本从未落地（`git ls-files | grep wsl` 为空），`run_tests.bat` 里也没有任何 WSL 调用。
>   要用 WSL 跑测试得手动 `wsl -d Ubuntu -- bash <脚本>`。
>   直接编译 `/mnt/d/...` 可用但 9p 较慢，重负载建议把源码放 WSL 内部的 `/home`。

设备：Redmi myron（serial `134d2f8`，KernelSU root，内核 6.12.69，SELinux Permissive）。
adb 在 `D:\Kiln\platform-tools\adb.exe`（已入 PATH，`where adb` 可验）。2026-10-22 晚设备在位并已跑通 itest。
服务/窗口排查：`dumpsys window windows | grep ggmcp`；崩溃：`logcat -s AndroidRuntime:E`。

## 已知债（2026-10-15 复核，2026-10-21 校准，2026-10-22 增补）

1. **main.c 巨型文件**：已拆 shell 域（`cmd_shell.c`）、view/符号/指针链域（`cmd_view.c`），
   现 2097 行（2026-10-22 `wc -l` 实测，因地址参数加固增加约 250 行）；待拆 `cmd_search` / `cmd_mem` / `cmd_proc` / dispatch。拆前后必跑 `check_dispatch_consistency.py`，
   并跑 `scripts\build_ndk.bat` 确认可编译（NDK+capstone 已就位，不再是盲改）。
2. wire 协议：1MiB 上限无分片（`GGWM_MAX_PAYLOAD`）；XOR seed 固定 `0xA5`（`shared/wire_protocol.h`）；serial 硬编码在 config。
   - 实测余量：`gg_get_results` 上限 10000 条（`agentd/main.c:1166` 硬钳），每条 `addr`/`value`/`value_hex` 三个字符串，
     单条序列化 61–74B → 整帧 JSON 约 **610–740KB**，距 1MiB 剩约 29%–42%（2026-10-21 按序列化格式实测）。
     → **补分片之前不要上调 limit**，否则单帧会被 `read_frame`（`server/device/wire.cpp:286`）直接判非法。
3. ~~`docs/` 工程文档与逆向研究笔记混放，待分目录~~ —— 2026-10-21 已分为 `docs/engineering/` / `docs/research/`，
   索引见 `docs/README.md`（研究笔记仅限本仓，不外发）。
4. ~~`archive/framework_v1/` 全量版框架（perf/atomic/error/mempool/conn_pool/io_batch）未接线~~ —— 2026-10-21 已删除（8 个文件）。
   理由：未接线，且与现役 `*_simple` 是同一职责的两套实现，留着必然被误用。需要时从提交 `a29871b` 的 `archive/framework_v1/` 恢复。
5. ~~**capstone 5.0.1 的 aarch64 静态库缺失**~~ —— 2026-10-21 17:44 已装入并实测 `build_ndk.bat` 全绿（本节保留为历史记录）。原判断：
   agentd / native 无法本地编译（`D:\Kiln\capstone\capstone-5.0.1\libcapstone.a`
   当时不存在，`scripts/build_ndk.bat` 直接 `exit /b 1`）。**该阻塞项已解除。**
6. **无真机连接时 `tests/integration/run_itest.ps1` 无法执行** → 设备端改动等于没有闭环，只能等设备接上后补验，
   期间不得把设备端改动当作「已验证」上报（2026-10-21）。
7. **rwproc33 全量搜索崩内核已定位**（2026-10-22）：根因在驱动侧，不在 agentd。
   ① `phy_mem_auto_offset.h:45` 用 `__pa(si.totalram)` 算物理地址上限（字节数当内核 VA）→ 范围检查失效；
   ② `get_proc_phy_addr` 不判 pmd 大页，THP 的 pmd 被 `pte_offset_kernel` 当页表指针；
   ③ 本地打开 `CONFIG_ENABLE_FAST_PHYMEM` 的 `__va()` 后无 `virt_addr_valid` 兜底；
   ④ 页表遍历在 `mmap_lock` 外。四处在 `build/rwProcMem33` 已改。
   **2026-10-22 晚真机验证通过**（myron / 6.12.69-android16-6）：GKI 树（设备 config +
   clang-18，SUBLEVEL 改 69、LOCALVERSION 补 `-android16-6-4k`）编出的 KO `this_module=1600`、
   vermagic 逐字匹配；`driver_self_test: channel OK`；`run_itest.ps1` **ITEST ALL PASS**；
   **com.android.systemui（341MB / 5940 VMA）全量匿名搜索 4.8s 完成、`truncated=false`、设备零 Oops**
   —— §十二 会让设备重启的那个操作现在正常收尾。
   关键修正：**不能用 `si.totalram` 当物理地址上界**（真机实测合法 pfn 可达 0x926afc=39.3GB，
   RAM 只有 11.6GB，PA_BITS=48），合法性只由 `virt_addr_valid()` 判。
   地址/值参数的两种形式（`"0x..."` 字符串与 JSON 数字）在真机逐项一致 —— 但**前提是先部署新
   `gdm`**：`run_itest.ps1` 用的是自带 itest 二进制（链接 `driver.c`），它绿了不代表设备上
   跑的 agentd 是新版。本轮就踩过：数字 `value` 报 `bad value`，实为设备还是 00:17 的旧 gdm。
   详见 `docs/audit/RWPROC33_2026-10-22.md`「真机验证结论」节。
8. **本轮（2026-10-22）已修但设备端未验**：地址参数两端类型约定不一致（数字地址让 `gg_watch_inject`
   `strtoull(NULL)` 段错误、`gg_write {value:100}` 误报缺参）、`parse_value_str` 缺 endptr、HEX_DUMP 响应 cmd 错位、
   safe_mode 漏 `ghost_tap/swipe`、elfsym 字符串表越界；server 端 Windows connect 缺 `SO_ERROR`（把「设备没连上」报成
   「令牌鉴权失败」）、HTTP 暴露面判定、JSON-RPC batch。测试程序 13 → 15 个（新增 `test_protocol` / `test_net_guard`）。
   `run_tests.bat` 全绿只代表 PC 侧逻辑正确，设备端行为仍需接真机。
9. **`--safe` / `--shell` / `--hwbp` 曾长期静默失效**（2026-10-22 已修，真机验证）：
   `main()` 的 `memset(&g_eng,0,...)` 在参数解析**之后**，三个开关解析完立刻被清零。
   `--safe` 失效 = **只读保护形同虚设**（write/freeze/shell 全放行）；`--shell` 永远打不开；
   `--hwbp` 永远被拒。已把 memset + `session_pid`/`result_cap`/`mutex_init` 前移到解析之前。
   验证：`--safe` 下 write/freeze/shell 均 `code=13 SAFE_MODE`，search 正常。
   **改 main() 初始化顺序时别再把它挪回解析之后。**
10. GG-U1（shell 无超时锁死守护进程）已修：`read`+`waitpid` 双超时，超时 SIGKILL 返回
   `GGERR_TIMEOUT`。真机：`cat /dev/zero` 10.0s 超时、其后 ping 0.040s 返回。
11. GG-U2（pid 复用）已修：会话身份改为 (pid, `process_starttime()`)，attach 记录、
   dispatch 每请求校验、`freeze_loop` 每轮校验，不一致即清会话（含结果集）。
   真机两条分支都实测到：目标退出 → `exited (expected=182255)`；用 `pidspin` 让新进程
   占住同一 pid → `reused by another process (227844 vs 221527)`，读写均 NO_SESSION。
   **改会话相关逻辑时注意**：判"还是不是同一个进程"只能靠 starttime，comm/uid/maps 都不可靠。
12. GG-U3（事件广播锁内阻塞）已修：锁内只快照 + `dup`，锁外发送，socket 设 `SO_SNDTIMEO=1s`。
   A/B 实测：旧实现下不读的订阅者让**新连接 auth 卡 7.44s**（`clients_register` 抢不到
   `g_clients_mtx`），修复后 0.01s。
   **测试方法有坑**：从 PC 侧测不出这个场景 —— `adb forward` 会替客户端读走数据，
   发送缓冲永远填不满。必须用 `build/ko/_dev/sub_stuck.c` 在**设备本地**建连接且不读。
13. GG-U6（cmd_end 丢 result）已修：`agentd/last_resp.h` 记 `full_len`/`truncated`，
   超长时只留 256B 预览并回退 UTF-8 边界，cmd_end 改给
   `result_truncated`/`result_bytes`/`result_preview`。
   **改这里的注意点**：不要再把「截断后的串」当 JSON 解析 —— 那是原缺陷的成因；
   新增事件字段时先用 `test_last_resp` 覆盖 UTF-8 三种相位（`s[256]` 落在
   continuation / lead byte / ASCII）。
14. GG-U4（无 key 即放行）已修：未指定 `--wire-key`/`--wire-key-file` 时生成
   16 字节临时密钥，落盘 `/data/adb/GG-Ghost/wire.key.ephemeral`（0600）并复用，
   stderr 打印；`/dev/urandom` 不可读才拒绝启动。AUTH 的空 key 分支改为拒绝。
   **手工调试须知**：不传 key 启动时，客户端要改用启动日志里打印的临时 key
   （或在设备上读那个 .ephemeral 文件）。正常部署不受影响。
   **另一个坑**：`start_agentd.sh start` 在端口被别的实例占用时会**误报"已启动"**
   （只按 pidfile 判断存活）。手工 `setsid` 起过实例后一定要先 `kill $(pidof gdm)`。
15. GG-U5 的「超限静默丢弃」已修：`send_ok` 序列化后先用 `gg_payload_fits()`
   判上限，超限回 `GGERR_CAP` 错误帧（带实际字节数 + 修法），不再是"什么帧都不发、
   客户端等 300s"。**协议分片仍未做** —— 单帧 1MiB 是硬上限，`get_results
   limit=10000` 已占 510KB，**加 limit 之前必须先分片**。
   调这块时的注意点：验证超限分支现在没有可构造路径，需要临时把
   `GGWM_MAX_PAYLOAD` 改小重编设备端（PC 端不用动），测完立即恢复。

### 已清（2026-10-15，本机实测）

- `run_tests.bat` 两道结构护栏曾是**死代码**（`if exist "%PYEXE%"` 对 PATH 命令名恒判假 → 静默跳过 + 报假绿，
  本地与 CI 同样中招）；已改为完整路径探测 + 校验确为 python 3 + 缺失时 `exit /b 1`。三条路径（正常/缺失/降级）均已实测。
- `build_all.bat` 汇总检查 `gg-mcp-server.exe`（实际 `gg-ghost-mcp.exe`）→ 恒报 `PC: MISSING`；
  且 `-Target` 参数被文档承诺却从未实现。已修正，并让"请求的产物缺失"导致构建失败。
- `test_all.bat` 在 `tests\unit` 下扫 `test_*.exe`（产物实际在 `build\pc\`）→ 循环体空转却打印 `[OK]`；
  且 `cd` 链相对错位。已改为按 `%ROOT%` 绝对路径、失败即退出。
- `benchmark.bat` 引用的 `tests\benchmark\memory_read.c` **全仓不存在**（无任何基准源码）→ 已改为如实报错。
- `run_itest.ps1` 的 `$Root` 少退一级（指向 `<repo>\tests\tests\...`）→ 已修正；adb 增加回退路径。
- 文档/脚本中残存的 `gg-mcp-server.exe` 全部对齐为 `gg-ghost-mcp.exe`（改名发生在提交 `b953bc5`「命名统一」）。
- **IDA Pro MCP 联动整体移除**（2026-10-15，按用户要求）：删 `server/ida_client.{h,cpp}` 与 `tests/unit/test_ida_client.cpp`；
  移除 `configs/gg-ghost.json` 的 `ida` 段、`build_pc.bat` / CI 的编译项、`main.cpp` 的 ida 配置解析、`session.{h,cpp}` 的
  `IdaConfig` / `ida_resolve`、`schema.cpp` 工具描述里的联动说明。`gg_resolve_symbol` 现为**纯本地静态解析**
  （模块 / 加载基址 / 模块内偏移 + ELF .dynsym 最近导出符号），响应**不再含 `ida` 字段**。旧实现可从 git 历史恢复。

### 已清（2026-08-28）

CI 三 job 重写（v4 + Windows 单测 + NDK 构建）；lint.bat 重写落地；双套模块收敛（simple 版留用，全量版入 archive）；
`run_tests.bat` 修复（ASCII/链接顺序/编码）并全绿；`build_ndk` 补 driver.c + 删 shell_util.c 幽灵引用；`deploy.bat` 产物对齐。

详细评审见 `docs/engineering/OPTIMIZATION_SUMMARY.md`（08-22 记录，文首有 08-28 更正声明，**该文档部分声明不实，以更正声明为准**）。

## 约定

- 提交信息：`<域>: <动词开头的一句话>`（域 = agentd / server / wire / scripts / docs / tests / chore）
- **wire 鉴权**（2026-10-21 起）：agentd 未认证的连接只放行 `PING`/`AUTH`，其余一律 `GGERR_PERM`。
  密钥在设备端 `/data/adb/GG-Ghost/wire.key`（`0600`，首启自动生成 32 字节随机 hex），
  通过 `--wire-key-file` 传入；PC 端 `configs/gg-ghost.json` 的 `wire_key` 必须与之一致，
  `deploy.ps1` 会自动拉取并写回配置。**wire_key 与 KO 密钥同级别，不入库、不入对话、不贴日志。**
  因此 `configs/gg-ghost.json` **不进版本库**（已列入 `.gitignore`）：它是部署生成物，
  入库的只有模板 `configs/gg-ghost.example.json`（`wire_key` 为空），首次部署由
  `deploy.ps1` 从模板复制后再写入真值。曾经因为跟踪了它，21 个提交把真实 key 带进过历史。
- 敏感研究笔记（ACE/反作弊对抗）只放 `docs/research/`，不外发；驱动密钥、QX_KEY 等不入库不入对话
- 设备端改动必须在真机过 itest（scripts/deploy.bat 部署后跑 tests/integration/run_itest.ps1）
- 特征字符串纪律：agentd 二进制 strings 不得出现 GG-Ghost / gg-touch / GG_KO 等特征（历史提交 88d83cc、21b246d 有验证方法）
- `.bat` 一律 ASCII-only 注释：cmd 按 ANSI（GBK）解析批处理，UTF-8 中文会吞掉 `^` 续行并破坏脚本

## 历史

apk/ 悬浮窗 APK 与 ggtool/ UI Demo 线已于 2026-08-27 下线（commit bcdcf1f，可从 git 历史恢复）。
旧版工作区指令（apk/ggtool 时代）归档在 `docs/engineering/AGENTD_HISTORY.md`。

本文件最后校准：2026-10-21（工具链、main.c 行数、wire 余量均按本机实测）。

16. **模糊搜索的三个坑（2026-10-22 靶场实测）**：
   - `refine` 的 op **必须同时认字符串和数字**。cJSON 字符串节点的 `valueint`
     恒为 0，而 `REFINE_*` 从 1 开始 —— 直接取 `valueint` 会让四个 op 全部落到
     switch default，**候选被整体清空**。MCP 路径因为 PC 端先 norm 成数字，
     碰巧正常，所以这个 bug 只在直连 wire 时暴露。改这块时先用字符串形式测一遍。
   - 读失败（`got<=0`）要**清位图**，否则这些元素永远读不到、永远不被剔除，
     `remaining` 卡住不动。但**短读（0<got<rd）不能清** —— rwproc 遇不可驻留页
     是短读返回（请求 64KB 只回 8KB），下一轮从 `off+got` 还能读到，清了就是
     误杀可读元素（`unchanged` 直接归零）。
   - 诊断这类问题最快的办法是在 refine 里打 `compared/kept` 计数：
     `compared>0 && kept==0` 说明比较都做了而不是没读到。
17. **模糊搜索要在数值稳定的时刻 init**。目标活跃（游戏在跑）时 init，后续
   refine 会把期间所有变化都算进去，收敛不到东西。正确姿势：菜单/暂停/操作
   间隙 init，再让目标发生一次明确变化后 refine。
18. **`gg_search_undo` 只回退模糊搜索**（位图环形栈 3 级，不回退数据快照）。
   精确搜索的 undo 需要另存地址数组副本，尚未实现，调用时会明确报错。

19. **agentd 重启后 PC 端第一条请求会自动重连重放**（2026-10-22 修）。判据是
   「整帧是否发完」：`wr<0` 才重放，`wr==0` 绝不重放（对端可能已执行）。
   注意实测里失败通常是 `wr=-1`（帧头塞进缓冲成功、payload 阶段断），
   不是"零字节"，所以不能只判零字节。
   **调试 wire 层**：`GG_LOG_LEVEL=1` 才出日志（默认 2），且要重定向到文件，
   管道给 tail 看不到实时输出。

20. **`gg_search_undo` 现在覆盖模糊与精确/区间两种搜索**（两套独立撤销栈，
   深度都是 3）。模糊存位图副本（256MB 区 8.4MB/级），精确存 addrs+vals
   副本（≤1.6MB/级）。
21. **`gg_disassemble` 要求 addr 按 4 字节对齐**，否则直接报错。ARM64 从非
   对齐地址解码会得到一两条"看着像指令"的垃圾然后停住 —— 实测 `count=1`
   但 `bytes_read=48`，很容易被当成真有个怪函数。

22. **大响应分片已实现**（`GGWF_MORE`，2026-10-22）。规则：
   - 客户端 AUTH 带 `"frag":1` 才会收到分片；旧客户端不变（超限仍 `GGERR_CAP`）
   - 同一响应所有分片共用 seq，最后一片不带 MORE
   - 每片 ≤ `GGWM_MAX_PAYLOAD`，重组上限 `GGWM_MAX_REASSEMBLED`（8MiB）
   **改这块极易踩的两点**：① XOR 必须逐片独立做（整体加密后切会让第二片起
   偏移全错）；② 整组分片要在同一次发送锁持有期间发完（中途插入别的响应会
   让接收端按 seq 拼错）。
23. **search.c 的 log_msg 是 weak 默认实现**。单元测试常只链接 search.c，
   没有 main.c —— 加诊断日志不用再给每个测试补 shim。产品构建里 main.c 的
   强符号会覆盖它。同理，新增其他跨文件依赖时优先考虑 weak 默认实现。

24. **内核模块已改名 `hwBreakpointProc_module` → `rwNova`**（2026-10-22）。只在
   `build_hwbp_ko.sh` 里做了 `obj-m += rwNova.o` + `rwNova-y := hwBreakpointProc_module.o`
   （源文件名不变，只改加载名与产物名）。**proc 节点名不受影响** —— 它来自
   `CONFIG_PROC_NODE_AUTH_KEY`（`dce3771681d4c7a143d5d06b7d32548e`），所以
   agentd 的探测路径照旧。

   **改这里最容易出事的一点**：原来 `build/ko/` 里的 afei prebuilt 靠「与自编译版
   **同名覆盖**」被消灭。改名后不再同名 → prebuilt 会以 `hwBreakpointProc_*.ko`
   留在 staging，而 `install_ko.sh` 会遍历目录里所有 `.ko` 逐个 insmod →
   **那份 prebuilt 一加载就可能打崩整机**（枚举从 0 起始、命中回调用 mutex）。
   `deploy.ps1` 现在显式 `Remove-Item $Stage\ko\hwBreakpointProc_*.ko`，别删这行。
   同理 push 要按 **staging 目录**遍历，不能按 `$koFiles`（rwNova 是后补进 staging 的）。

25. ~~**hwbp / watch 当前不可用**~~ —— **2026-10-22 深夜复测：可用，原结论是误判**。
   原记录说「探测返回 GET_NUM_BRPS=-1，自编译模块命令码与 driver.h 不一致」。
   真机直连复现（`build/ko/_dev/hwbp_probe.c`，向两个模块节点各发一次 cmd=3）：
   - 断点节点 `/proc/dce377…/dce377…` → `read=6`，**命令码基准就是对的**
   - 内存节点 `/proc/e84523…/e84523…` → `read=-1 EFAULT`（cmd3 在内存模块里是
     READ_PROCESS_MEMORY，p1=0 是空句柄）

   所以 -1 只可能来自"bp_fd 指向了内存模块"。经 agentd 实测：probe=6 → usable，
   `watch_set` 注册 16 线程、`UNINST` 正常、槽位满时内核回 `ENOSPC`（说明
   perf_event 真的建了起来）。**别再照第 25 条旧文案去重编模块。**

   **这一条真正的教训**：`-1` 和"0 起始旧模块的 4"被写进了同一句错误提示，
   排查方向直接被带偏。现在 `main.c` 按 `g_hwbp_probe_rc` 分开报：
   `<0` = 节点没开/开错，`==4` = 旧模块，两条不同的文案。

26. **两个模块的 proc 节点长得一样，光看路径分不出谁是谁**（2026-10-22 修）。
   都是 `/proc/<auth_key>/<auth_key>`（mem `e84523…` / bp `dce377…`），
   把 `--rwproc-bp-key` 写成 mem 的 key 时，open 会**成功**，然后 GET_NUM_BRPS
   打到内存模块上返回 -1 —— 表现和"模块命令码不对"一模一样。
   现在 `driver.c` 的 `rwproc_open_node` 带类型校验：打开后先发一次
   `GET_NUM_BRPS`，`read>0` 判为断点模块、`read<=0` 判为内存模块，不匹配就
   关掉并明确报「is the memory module, not breakpoint — key 配反了」。
   **探测只有这一条命令是安全的**：内存模块的 cmd4 是 WRITE_PROCESS_MEMORY、
   cmd5 是 CLOSE_PROCESS，拿它们试探会在真正的断点上写内存 / put_pid。

   **自动扫描（不传 key）实测在真机上是无效的**：模块 `hide_procfs_dir` 之后
   `readdir("/proc")` 列不出 key 目录、`access()` 也失败 → 「memory module node
   not found」。所以 `start_agentd.sh` 里那两行显式 key 是**必需**的，别当成冗余删掉。
   扫描分支留着只为模块未隐藏的场景（本地调试）。

27. **hwbp 在 `[stack]` 最后一页（紧邻栈顶 4KB）注册会 `EINVAL`**（2026-10-22 实测）。
   同一 vma 里 0x…df000 成功、0x…ff000~0x…fff00 全部 `errno=22`，
   与进程无关（换 pid 一样），换 type/len 也一样。设断点时避开栈顶那一页。
   **排查时别被 `no thread registered` 带偏**：`start_all` 把每个线程的失败原因
   全吞了，地址不合法和进程不可注册报的是同一句话。用
   `build/ko/_dev/hwbp_inject.c` 逐步打印 OPEN/INST/UNINST 的返回值与 errno。
   另外 `HWBP_MAX_HANDLES=16`（模块静态句柄表，`hwBreakpointProc_module.c:28/288`）：
   已注册 16 个断点时再 INST 由模块直接回 `ENOSPC`。这是正常语义，但它同时意味着
   **watch 对一个进程最多只覆盖前 16 个线程**（`start_all` 按 `/proc/<pid>/task`
   顺序注册，第 17 个起全部 ENOSPC 被 `continue` 跳过）。systemui 这种 200+ 线程的
   进程，只有前 16 个线程的命中能被看到 —— 别把"没命中"当成"地址不对"。

28. **hwbp 命中链路已真机验证**（2026-10-22 夜，`hits=64` + 寄存器/反汇编都对）。
   验证用自建靶子而不是猜地址：`build/ko/_dev/bp_target.c`（每秒写 1000 次全局变量，
   打印 `&g_var` 和 `tick` 的地址，`-static` 非 PIE 所以地址稳定），
   `hwbp_hits.c` 直连模块看 COUNT/DETAIL。游戏进程会冻结、栈地址又挑不出稳定可写点，
   拿它们验证命中率纯靠运气。

   **踩到的真 bug（`driver.c` 的 `rwproc_hwbp_get_hits`）**：33 字节请求头里的
   `buf_size` 才是内核决定回写多少的字段，`resp_cap`（read() 的 count）不进协议头。
   原来 DETAIL 调用传的是 `NULL, 0, bytes`（data_size=0）→ 模块
   `if (size < sizeof(HWBP_HIT_ITEM)) return -EINVAL;` 恒失败 →
   **watch_poll 永远 0 条**，而 COUNT 正常（它不检查 size），所以表面上
   "断点能注册、就是没命中"。已改为 `NULL, bytes, bytes`,并加了结构护栏
   `tests/unit/check_hwbp_detail.py`（`run_tests.bat` 会自动跑，正反两向验过）。

   **能力边界**：模块 `hit_arr` 只在 `hit_count < HWBP_HIT_CAP(1024)` 时写入
   （`record_hit_details`），满了就不再记录 —— 高频断点看到的是**最早 1024 次**
   命中，游标推完就永远 0。watch 适合低频事件（某函数被调用、某变量被改几次），
   不适合监控高频写。


29. **AI-Root 侧（AR）审计项已全部关闭**（2026-10-22 夜）。改 policy / consent /
   config 前必读这三条实测结论：
   - **自保护路径判定是两段式**：词法不命中后再用 `GetFinalPathNameByHandleW`
     解析真实路径。别把第二段删掉当"性能优化"——它挡的正是 8.3 短名
     （`PROGRA~1`）、junction、尾随点空格。解析结果若是 `C:\$Extend\$Deleted\*`
     必须丢弃（那是"已删除但仍有句柄"的返回值，拿去比对会得出假的"不命中"）。
   - **Windows 上 delete / rename 不能当并发抢占点**。多线程可以各自打开同一文件
     句柄（默认共享删除），对同一 file object 的删除/改名**每个调用都返回成功** ——
     实测 8 线程并发 `remove_file` 出现 2 个成功、`rename` 出现 4 个。要用
     `dwShareMode = 0` 独占打开 + `SetFileInformationByHandle(FileDispositionInfo)`。
   - **`default_autonomy` 允许解释器 = 允许任意代码执行**，这是刻意取舍。只把
     「即时执行外部内容」降为 Confirm（`npx`/`npm run`/`pip install`/`cargo install`/
     `go run`/`git -c`/`dotnet run`）。**改动只影响新生成的默认策略**，已部署的
     `policy.toml` 不会自动变。

30. **main.c 拆分进度**（2026-10-22 深夜）。已拆出：`cmd_shell.c`（2026-08）、
   `cmd_view.c`（2026-08）、**`cmd_search.c`（本轮）**。
   `cmd_search.c` 收走搜索与结果集 8 个 handler（search / search_range /
   unknown_init / refine / result_count / get_results / clear_results /
   set_results）+ 域内 helper `parse_region` / `parse_refine_op`。
   main.c 2501 → 2204 行。

   跨域共享的东西上移到 `engine_internal.h`，**别在 cmd_*.c 里再写一份**：
   `parse_vt`（读/写/冻结/指针扫描/搜索五域都用）、`j_scalar_str`、
   `parse_value_str`、`j_addr_u64`、`event_search_update`、`event_results_update`，
   以及 `VT_HINT` / `REGION_HINT` 两个错误文案宏。
   拆分只是搬代码 + 去 `static`，逐字等价（用 diff 核对过），但**一定要真机回归**：
   本轮就是因为设备侧驱动状态，白排查了两小时（见第 31 条）。

31. **部署链路的两个坑（都会让「代码没问题但功能全挂」）**。

   **(a) `build\ko\` 里的 rwProcMem 是 afei prebuilt（39KB），全量搜索会打崩内核。**
   2026-10-22 深夜连续复现四次：attach 正常、`gg_search` 一发出去设备就重启
   （无 pstore 记录 = 看门狗硬复位，不是 panic）。换成自编译修复版
   （307KB，`build\ko\_dev\rwProcMem_gki.ko` → 现已在
   `build\ko\` 和 `build\ko_built\` 各放一份）后，同一个目标搜索秒回正常结果。
   `deploy.ps1` 已加显式替换 + 缺失时告警。**这个 KO 不入库（build/ 被 ignore），
   换机器时务必确认设备上那份是 307KB 级、不是 39KB。**
   顺带确认：`_dev` 里 4 个 KO（new/gki/dbg/v2）都能 insmod，但只有 gki 这份跑过
   完整验证；`rwproc_v2.ko` 比它新 8 分钟但没验过，别随手换。

   **(b) `driver_self_test` 读「自身进程」会返回 0，别当硬门禁。**
   现象：`read=4`（成功）但内容全 0，`data` 和栈上变量都这样；同一 KO 干净重启后
   第一次能过、之后几次就失败，HEAD 版和拆分版二进制都复现。而同一时刻
   读**目标进程**完全正常（phone 搜出 150 条、值/地址都对）。
   结论：驱动对「target == current」这条路径有缺陷，跨进程读不受影响。
   现在口径：读到期望值 → OK；读成功但值不对 → **WARN + 按可用处理**；
   读失败/句柄打不开 → 才判不可用。依据是「节点能开 + CMD_INIT_DEVICE_INFO 成功」
   这两条硬事实。日志里会留 `self-test: WARN self-read returned wrong values`。
   **真正的驱动根因没修**（属于 `docs/audit/RWPROC33` 里没查完的那节），
   所以别把这个 WARN 当噪音删掉。

32. **KO 版本谱系 —— 用错一个就白查一晚**（2026-10-23 凌晨定案）。

   `build/ko/_dev/` 里四个自编译 KO，只有一个是好的。别凭时间戳选：

   | 文件 | 大小 | 读写 | 全量搜索 | 结论 |
   |---|---|---|---|---|
   | `rwproc_v2.ko` | 307640 | **ITEST ALL PASS** | **3.6s，不崩** | **用这个** |
   | `rwProcMem_gki.ko` | 307584 | 读全 0 | 正常 | 有回归，别用 |
   | `rwproc_dbg.ko` | 330712 | — | — | 唯一带 `CONFIG_DEBUG_PRINTK`，只用来抓页表日志 |
   | `rwProcMem_new.ko` | 160288 | 未测 | 未测 | — |
   | `prebuilt/rwProcMem_slim.ko` | 39552 | — | — | **vermagic 不匹配**，insmod 报 `disagrees about version of symbol module_layout` |

   `rwProcMem_gki.ko` 的回归点在 `phy_mem_fast.h` 的 `virt_addr_valid()` 守卫：
   它在本机把合法 pfn 判成非法，读到全 0（页表遍历本身是对的 —— 用 dbg 版抓日志
   能看到 pgd/pud/pte 正常、paddr 正确算出）。源码里已改成
   `pfn_to_online_page(PHYS_PFN(cur - PAGE_OFFSET))`，但**没编译验证过**
   （WSL 里的内核树 /root/hwbp-src 缺 `modules` 目标），v2 是不是这么修的也没确认。

   **部署源现状**：`build/ko/rwProcMem_6.12.69.ko` = v2，`build/ko_built/` 同步。
   这两个目录都不入库（build/ 被 ignore），换机器时必须重新确认大小是 **307640**。

33. **「一个驱动变成两个」的根因：deploy 的 staging 目录从不清理**（2026-10-23 修）。

   `deploy.ps1` 把 `build\ko\*.ko` 拷到 `%TEMP%\ggmcp_stage\ko\`，再**按 staging
   目录遍历推送**（这是为了不漏掉后补进去的 rwNova）。但 staging **跨次保留且从未
   清空** —— 2026-10-21 首次部署时 `build\ko\` 里那个 `apr_dlkm.ko`（rwProcMem 的
   原始编译名，`build_rwproc_ko.sh` 的产物就叫这个）被拷进去后再没出来，
   而 `build\ko\` 后来改用了 `rwProcMem_<内核版本>.ko` 命名，
   于是这份 10-21 的旧文件每次部署都被推到设备上。

   后果：设备上 `apr_dlkm.ko`（39552）与 `rwProcMem_6.12.69.ko` 并存。两个都是
   rwProcMem、模块名不同（`apr_dlkm` / `rwProcMem_module`）所以都能 insmod，
   但 proc 节点只有一个 —— 先加载的占住 `/proc/<key>/<key>`，后加载的
   `proc_mkdir` 失败 fallback 到 /proc 根（`strings apr_dlkm.ko` 里能看到这句日志）。
   **agentd 永远打开先加载那个**，于是「换了 KO 但行为没变」就成了灵异现象。

   已修：`deploy.ps1` 每次部署前 `Remove-Item "$Stage\ko\*"`。
   若再看到设备上出现 `apr_dlkm.ko`，说明 staging 又被污染了 —— 直接删掉即可
   （它没有任何独立功能，就是 rwProcMem 的别名）。
