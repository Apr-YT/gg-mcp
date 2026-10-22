# AGENTS.md — GG-MCP 工作区指令（每个会话自动加载）

## 项目一句话

内核驱动式内存修改 MCP 服务：PC 上的 AI agent 通过 MCP 直接读写安卓游戏进程内存。
**只走 KO 内核驱动通道**（rwproc33），GG 核心采用外挂圈子加载（ELF 桩 + hook syscall 270）。

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

## 已知债（2026-10-15 复核，2026-10-23 清理：删除已清项与被取代的重复条目，33 条 → 28 条）

1. **main.c 巨型文件**：已拆 shell（`cmd_shell.c`）、view/符号/指针链（`cmd_view.c`）、
   搜索与结果集（`cmd_search.c`）、内存读写/冻结（`cmd_mem.c`）。现 **2049 行**
   （2026-10-23 `wc -l`）。待拆 `cmd_proc`（list/attach/target_info/module_* /
   calc_offset / pointer_scan）与 dispatch。拆前后必跑 `check_dispatch_consistency.py`
   和 `scripts\build_ndk.bat`（NDK+capstone 已就位，不是盲改）。
2. **wire 协议的三个遗留**：XOR seed 固定 `0xA5`（`shared/wire_protocol.h`）；serial
   硬编码在 config；分片虽有（第 18 条）但上限是硬的 —— 单帧
   `GGWM_MAX_PAYLOAD` 1MiB、重组 `GGWM_MAX_REASSEMBLED` 8MiB。
   - `gg_get_results` 上限 10000 条时约 510KB；分片后单帧上限不再是瓶颈，
     **但上调 limit 前先看 8MiB 重组上限**，超了会被 `read_frame`
     （`server/device/wire.cpp:286`）直接判非法。
3. **无真机连接时 `tests/integration/run_itest.ps1` 无法执行** → 设备端改动等于没有闭环，只能等设备接上后补验，
   期间不得把设备端改动当作「已验证」上报（2026-10-21）。
4. **rwproc33 崩内核的四点修复（2026-10-22）—— 现状见第 27 条**。
   根因在驱动侧：`__pa(si.totalram)` 当物理地址上界、`get_proc_phy_addr` 不判 pmd 大页、
   `__va()` 后无 pfn 有效性守卫、页表遍历在 `mmap_lock` 外。四处都改在
   `build/rwProcMem33` 源码里。
   ⚠️ 第四点「加守卫」当时的实现写错了地址基准（`PHYS_PFN(cur - PAGE_OFFSET)` 少减
   `memstart_addr`），反而引入"部分页静默丢弃"的回归；真因分析与修法见第 27 条。
   「不能用 totalram 当上界」这条本身成立：真机实测合法 paddr 可达 0xaffffefff
   （末端 bank），而 RAM 只有 11.6GB（PA_BITS=48）。
5. **`--safe` / `--shell` / `--hwbp` 曾长期静默失效**（2026-10-22 已修，真机验证）：
   `main()` 的 `memset(&g_eng,0,...)` 在参数解析**之后**，三个开关解析完立刻被清零。
   `--safe` 失效 = **只读保护形同虚设**（write/freeze/shell 全放行）；`--shell` 永远打不开；
   `--hwbp` 永远被拒。已把 memset + `session_pid`/`result_cap`/`mutex_init` 前移到解析之前。
   验证：`--safe` 下 write/freeze/shell 均 `code=13 SAFE_MODE`，search 正常。
   **改 main() 初始化顺序时别再把它挪回解析之后。**
6. GG-U1（shell 无超时锁死守护进程）已修：`read`+`waitpid` 双超时，超时 SIGKILL 返回
   `GGERR_TIMEOUT`。真机：`cat /dev/zero` 10.0s 超时、其后 ping 0.040s 返回。
7. GG-U2（pid 复用）已修：会话身份改为 (pid, `process_starttime()`)，attach 记录、
   dispatch 每请求校验、`freeze_loop` 每轮校验，不一致即清会话（含结果集）。
   真机两条分支都实测到：目标退出 → `exited (expected=182255)`；用 `pidspin` 让新进程
   占住同一 pid → `reused by another process (227844 vs 221527)`，读写均 NO_SESSION。
   **改会话相关逻辑时注意**：判"还是不是同一个进程"只能靠 starttime，comm/uid/maps 都不可靠。
8. GG-U3（事件广播锁内阻塞）已修：锁内只快照 + `dup`，锁外发送，socket 设 `SO_SNDTIMEO=1s`。
   A/B 实测：旧实现下不读的订阅者让**新连接 auth 卡 7.44s**（`clients_register` 抢不到
   `g_clients_mtx`），修复后 0.01s。
   **测试方法有坑**：从 PC 侧测不出这个场景 —— `adb forward` 会替客户端读走数据，
   发送缓冲永远填不满。必须用 `build/ko/_dev/sub_stuck.c` 在**设备本地**建连接且不读。
9. GG-U6（cmd_end 丢 result）已修：`agentd/last_resp.h` 记 `full_len`/`truncated`，
   超长时只留 256B 预览并回退 UTF-8 边界，cmd_end 改给
   `result_truncated`/`result_bytes`/`result_preview`。
   **改这里的注意点**：不要再把「截断后的串」当 JSON 解析 —— 那是原缺陷的成因；
   新增事件字段时先用 `test_last_resp` 覆盖 UTF-8 三种相位（`s[256]` 落在
   continuation / lead byte / ASCII）。
10. GG-U4（无 key 即放行）已修：未指定 `--wire-key`/`--wire-key-file` 时生成
   16 字节临时密钥，落盘 `/data/adb/GG-Ghost/wire.key.ephemeral`（0600）并复用，
   stderr 打印；`/dev/urandom` 不可读才拒绝启动。AUTH 的空 key 分支改为拒绝。
   **手工调试须知**：不传 key 启动时，客户端要改用启动日志里打印的临时 key
   （或在设备上读那个 .ephemeral 文件）。正常部署不受影响。
   **另一个坑**：`start_agentd.sh start` 在端口被别的实例占用时会**误报"已启动"**
   （只按 pidfile 判断存活）。手工 `setsid` 起过实例后一定要先 `kill $(pidof gdm)`。
11. **大响应分片已实现（GG-U5）**，规则见第 18 条。调分片/超限分支时注意：
   现在没有可构造的超限响应，要验证得临时把 `GGWM_MAX_PAYLOAD` 改小**只重编设备端**
   （PC 端不用动），测完立即恢复。
12. **模糊搜索的三个坑（2026-10-22 靶场实测）**：
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
13. **模糊搜索要在数值稳定的时刻 init**。目标活跃（游戏在跑）时 init，后续
   refine 会把期间所有变化都算进去，收敛不到东西。正确姿势：菜单/暂停/操作
   间隙 init，再让目标发生一次明确变化后 refine。
14. **`gg_search_undo` 只回退模糊搜索**（位图环形栈 3 级，不回退数据快照）。
   精确搜索的 undo 需要另存地址数组副本，尚未实现，调用时会明确报错。
15. **agentd 重启后 PC 端第一条请求会自动重连重放**（2026-10-22 修）。判据是
   「整帧是否发完」：`wr<0` 才重放，`wr==0` 绝不重放（对端可能已执行）。
   注意实测里失败通常是 `wr=-1`（帧头塞进缓冲成功、payload 阶段断），
   不是"零字节"，所以不能只判零字节。
   **调试 wire 层**：`GG_LOG_LEVEL=1` 才出日志（默认 2），且要重定向到文件，
   管道给 tail 看不到实时输出。
16. **`gg_search_undo` 现在覆盖模糊与精确/区间两种搜索**（两套独立撤销栈，
   深度都是 3）。模糊存位图副本（256MB 区 8.4MB/级），精确存 addrs+vals
   副本（≤1.6MB/级）。
17. **`gg_disassemble` 要求 addr 按 4 字节对齐**，否则直接报错。ARM64 从非
   对齐地址解码会得到一两条"看着像指令"的垃圾然后停住 —— 实测 `count=1`
   但 `bytes_read=48`，很容易被当成真有个怪函数。
18. **大响应分片已实现**（`GGWF_MORE`，2026-10-22）。规则：
   - 客户端 AUTH 带 `"frag":1` 才会收到分片；旧客户端不变（超限仍 `GGERR_CAP`）
   - 同一响应所有分片共用 seq，最后一片不带 MORE
   - 每片 ≤ `GGWM_MAX_PAYLOAD`，重组上限 `GGWM_MAX_REASSEMBLED`（8MiB）
   **改这块极易踩的两点**：① XOR 必须逐片独立做（整体加密后切会让第二片起
   偏移全错）；② 整组分片要在同一次发送锁持有期间发完（中途插入别的响应会
   让接收端按 seq 拼错）。
19. **search.c 的 log_msg 是 weak 默认实现**。单元测试常只链接 search.c，
   没有 main.c —— 加诊断日志不用再给每个测试补 shim。产品构建里 main.c 的
   强符号会覆盖它。同理，新增其他跨文件依赖时优先考虑 weak 默认实现。
20. **两个内核模块都改过名**（2026-10-22 改 hwbp，2026-10-23 改内存模块）。
   改法一致：模块目录的 Makefile 换 `obj-m`，源文件名保持不变。

   | 原模块名 | 现模块名 | 产物 | 改法 |
   |---|---|---|---|
   | `hwBreakpointProc_module` | **rwNova** | `rwNova_6.12.69.ko` | `obj-m += rwNova.o` / `rwNova-y := hwBreakpointProc_module.o` |
   | `rwProcMem_module` | **rwNova33** | `rwNova33_6.12.69.ko` | `obj-m += rwNova33.o` / `rwNova33-y := rwProcMem_module.o` |

   **两个名字不能相同** —— 同名模块第二个 `insmod` 直接 `-EEXIST`。

   ⚠️ **`build/rwProcMem33/` 整个驱动源码目录不在版本库里**（`.gitignore` 的 `build/*`），
   所以 `obj-m` 的改动只体现在**本机的 Makefile 和入库的 `.ko` 产物**上。
   换机器/重新 clone 后要重做这两处改：
   - `build/rwProcMem33/rwProcMem33Module/rwProcMem_module/Makefile`：
     `obj-m += rwNova33.o` / `rwNova33-y := rwProcMem_module.o`
   - 编出的 `rwNova33.ko` → 放 `build/ko/rwNova33_<内核版本>.ko`
   编译要用 `/root/gki`（有完整 `modules` 目标），且必须跑
   `build/ko/_dev/fix_modc.py` 修 `__version_ext_names[] = ;` 那个上游缺陷，
   见 RWPROC33_ADAPTER.md §10.4.1。
   **proc 节点名不受影响**：它来自 `CONFIG_PROC_NODE_AUTH_KEY`
   （内存 `e84523…` / 断点 `dce377…`），跟模块名无关，所以 agentd 的探测路径照旧。

   原来
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
21. **hwbp 曾被误判为「不可用」**（2026-10-22，已改判）。当时把 `GET_NUM_BRPS=-1`
   直接写成「模块命令码不一致」，实际是 bp 节点错指到了内存模块。**以第 22/23/24 条为准**，
   别再照旧文案去重编模块。
22. **两个模块的 proc 节点长得一样，光看路径分不出谁是谁**（2026-10-22 修）。
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
23. **hwbp 在 `[stack]` 最后一页（紧邻栈顶 4KB）注册会 `EINVAL`**（2026-10-22 实测）。
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
24. **hwbp 命中链路已真机验证**（2026-10-22 夜，`hits=64` + 寄存器/反汇编都对）。
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
25. **AI-Root 侧（AR）审计项已全部关闭**（2026-10-22 夜）。改 policy / consent /
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
26. **main.c 拆分进度**（2026-10-22 深夜）。已拆出：`cmd_shell.c`（2026-08）、
   `cmd_view.c`（2026-08）、**`cmd_search.c`（本轮）**。
   `cmd_search.c` 收走搜索与结果集 8 个 handler（search / search_range /
   unknown_init / refine / result_count / get_results / clear_results /
   set_results）+ 域内 helper `parse_region` / `parse_refine_op`。
   main.c 2501 → 2049 行（cmd_search 一轮、cmd_mem 一轮）。

   跨域共享的东西上移到 `engine_internal.h`，**别在 cmd_*.c 里再写一份**：
   `parse_vt`（读/写/冻结/指针扫描/搜索五域都用）、`j_scalar_str`、
   `parse_value_str`、`j_addr_u64`、`event_search_update`、`event_results_update`，
   以及 `VT_HINT` / `REGION_HINT` 两个错误文案宏。
   拆分只是搬代码 + 去 `static`，逐字等价（用 diff 核对过），但**一定要真机回归**：
   本轮就是因为设备侧驱动状态，白排查了两小时（见第 27 条）。
27. **KO 版本谱系 + 读路径的 memstart_addr 陷阱**（2026-10-23 两次实测定案）。

   **现役 KO**：`rwNova33_6.12.69.ko`（305824 = 内存读写）+ `rwNova_6.12.69.ko`
   （359504 = 硬件断点），都由 `scripts/build_rwproc_ko.sh` 从当前源码重编，
   ITEST ALL PASS、全量搜索 3.3s / 20698 条不崩。

   | 文件 | 大小 | 状态 |
   |---|---|---|
   | `rwNova33_6.12.69.ko` | 305824 | **现役**（含 memstart_addr 修复 + 隐藏项） |
   | `rwNova_6.12.69.ko` | 359504 | **现役**（hwbp，加 `ccflags-y += -Wno-error` 后可编） |
   | `rwproc_v2.ko` | 307640 | 历史最好的旧版，保留备查 |
   | `rwProcMem_gki.ko` | 307584 | 读全 0，弃用 |
   | `_dev/rwproc_dbg.ko` | 330712 | 唯一带 `CONFIG_DEBUG_PRINTK`，只用来抓页表日志 |
   | `prebuilt/rwProcMem_slim.ko` | 39552 | **vermagic 不匹配**，insmod 直接 Exec format error |

   ### 读路径的真实根因（推翻此前两次结论）

   现象：`driver self-test` 读**自身进程**时好时坏（"both reads FAILED, r=4 但内容 0"），
   部分页静默读不到；跨进程读低地址却正常。

   此前写过两个错误结论，都废弃：~~"virt_addr_valid 把合法 pfn 判成非法"~~、
   ~~"paddr 越界"~~（paddr 0x89b68124c 落在 0x880000000–0x8af8fdfff 段内，合法）。

   **真因**：`phy_mem_fast.h` 的守卫算 pfn 时用了
   `PHYS_PFN((unsigned long)cur - PAGE_OFFSET)` —— 少减了 `memstart_addr`。
   arm64 线性映射是 `__va(p) = p - memstart_addr + PAGE_OFFSET`，所以
   `cur - PAGE_OFFSET = p - memstart_addr`，**不是 p**。本机 memstart_addr ≈ 2GB
   （0x80000 页）。诊断版实测同一次读：

   ```
   rwdbg[0] pfn_calc=0xa6238e   ← PHYS_PFN(cur - PAGE_OFFSET)，少了 2GB
            pfn_v2p =0xae238e   ← virt_to_pfn(cur)，正确
   ```

   后果：真实 paddr 有效、但错位的 pfn 落进 MMIO 空洞时 `pfn_to_online_page` 返回
   NULL → 整页静默丢弃。**每页是否被丢取决于地址，所以表现为"时好时坏"，
   而不是稳定失败 —— 这是它查了两轮才揪出来的原因。**

   修法（已落地）：两处守卫改成 `pfn_to_online_page(virt_to_pfn((void *)cur))`，
   `virt_to_pfn` 内部走 `__virt_to_phys`，已扣 memstart_addr。

   **修复前后实测（同一台设备、同一进程类型）**：全量搜索 count 13681 → **20698
   （+51%）**，耗时 3.3s 基本不变 —— 也就是说**之前约三分之一的页根本没被读到**，
   搜索结果是"漏"的。这条对所有读写路径生效，不只是 self-test。

   **部署源**：`build/ko/rwNova33_6.12.69.ko` 已入库（`build/ko/*.ko` 是早期就被
   跟踪的少数文件）；`build/ko_built/` 未跟踪、只在本机。换机器先 `ls -la` 对大小。

28. **「一个驱动变成两个」的根因：deploy 的 staging 目录从不清理**（2026-10-23 修）。

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

29. **隐蔽性基线（2026-10-23 落地，动驱动前先读这条）**。

   两个内核模块原本各 10 处裸 `printk`（含 `Hello/Goodbye <设备名>`、
   `[rwProcMem] module hidden by default`），dmesg 里 `grep -i rwproc` 能直接看到；
   proc 节点权限是 `S_IRUGO|S_IWUGO`（0666）。**任何 App 都能打开它**，等于
   "藏了个地址但没锁门"。现在：

   | 项 | 改法 |
   |---|---|
   | 20 处裸 `printk` | → `printk_debug`（受 `CONFIG_DEBUG_PRINTK`，当前关闭＝空函数） |
   | 4 处 `proc_create` 权限 | `S_IRUGO\|S_IWUGO` → `S_IRUSR\|S_IWUSR`（0600） |

   真机实测（重启后干净状态）：`dmesg` 里 rwproc/rwnova **0 条**；节点 `-rw-------`；
   非 root 打开 → `Permission denied`；`ls /proc` / `/proc/modules` / `/sys/module`
   均查不到。**改完必须同时验证这三条，缺一条就是没生效。**

   **仍是暴露面，按性价比排**（要动之前先确认目标）：
   - `CONFIG_PROC_NODE_AUTH_KEY` 是**编译期固定值**（内存 `e84523…` / 断点 `dce377…`），
     全设备同一把钥匙 —— .ko 一旦外流就永久失效。`strings` 能直接读出来。
   - 目录项只在 readdir 里被过滤（kprobe hook `proc_root_readdir`），
     **知道路径就能 open**。真正的"不存在"要靠 `anon_channel.h`（匿名 inode，不建
     proc 节点），代码已写好但 `ver_control.h` 里是 `//#define CONFIG_USE_ANON_CHANNEL`。
   - `cfi_bypass.h` 只 patch、**没有 restore**，内核 text 长期处于被改状态，
     完整性自检能抓到。
   - debugfs 挂上后 `/sys/kernel/debug/kprobes/list` 会列出 `proc_root_readdir` 探针。

   **hwbp 编不过的坑（已解）**：`anti_ptrace_detection.h` 里两个未引用的 static 函数
   被完整 GKI 树的 `-Werror` 判死，`KCFLAGS=-Wno-error` **压不住**（那是内核自带的），
   必须用模块级 `ccflags-y += -Wno-error`。另外 `hwbp_proc.c` **不参与编译**
   （Makefile 只有 `obj-m += hwBreakpointProc_module.o`），里面的 cdev/`/dev` 节点
   全是死代码 —— 别照着它找 `/dev/rwNova`。

30. **搜索性能：瓶颈是请求次数，不是驱动**（2026-10-23 实测定案，3.2s → 0.15s）。

   在驱动里加 `ktime` 分段计时（临时诊断版）量出的真实数据：

   ```
   per_page_walk = 357ns    ← 页表遍历
   per_page_phys = 559ns    ← 物理读 + 拷贝
   reads = 258400, pages = 55002   ← 平均每次 read 只读到 0.21 页
   ```

   **驱动内每页只花 916ns**，全量扫 341MB 理论上 80ms —— 那 3.2s 里 97% 不是驱动。
   真因：`rw-p` 堆里大量页"映射了但从未 touch"，驱动读它们失败，而
   `gg_fault_advance()` 每次只前进一页 → 25 万次 syscall（每次 ~12μs）。

   **修法**：用 `/proc/<pid>/pagemap` 的 present 位（bit63）批量筛选
   （`agentd/search.c` 的 `pm_open / pm_present_run / pm_skip_absent`）：
   一次 `pread` 8 字节/页、最多 64 页（256KB），只对 present 的连续页发
   驱动读，absent 页直接跳并清零 `carry`（字节流断开，跨 absent 的匹配不存在）。
   pagemap 打不开（`pmfd < 0`）时退回原逐页逻辑 —— 零回归风险。

   **为什么不用"连续失败就大步跳过"**：X4 试过、已否决 —— 大步会过冲混合
   region 末端可读页。pagemap 不猜，只跳确定为 absent 的页，因此**不漏**。

   实测：systemui 341MB 全量搜索 **3.21s → 0.15~0.22s**（15~20 倍），
   结果数不减反增（20698 → 21954，此前"遇到 absent 就中断整块"会丢掉块内
   后面的可读页）。

   **正确性验证（改搜索逻辑必须做这一步）**：取一个命中密集的 64KB 区间，
   用 `gg_read_bytes` 分块拉回本地扫描，与 `gg_search` 在该区间的命中集合
   逐地址比对 —— 实测 192 vs 192、集合完全相同。只在两边数量相等还不够，
   要比**集合**。

   模糊路径（`search_unknown_init` / `search_refine`）也接了，用 `pm_try_skip()`
   统一处理：absent 段在快照里补 0、并从候选位图清掉，语义与原来的 `got<=0`
   分支完全一致。实测模糊首帧 **0.31s / 6710 万元素（256MB 快照）**，
   refine 0.26s，undo 正常。

   **改搜索逻辑的验收步骤（务必照做）**：
   1. `run_itest.ps1` 必须 ALL PASS
   2. 精确搜索的 count 与改前同量级、抽查结果值正确
   3. 取命中密集的 64KB 区间做「集合比对」（`gg_read_bytes` 分块拉回本地扫）
   4. 模糊：init → refine → undo 状态机走通，remaining 数合理

31. **匿名 inode 通道已启用（2026-10-23）—— 但"节点不建"做不到，别被这个说法误导**。

   现状：`CONFIG_USE_ANON_CHANNEL` 打开后，agentd attach 时通过 proc 通道发
   `CMD_ANON_GET_FD(14)`，驱动在当前进程 `fd_install()` 一个
   `anon_inode:rwproc`，之后**内存模块的所有命令改走 `ioctl(fd, 33, buf)`**。
    实测：`ls -l /proc/<agentd>/fd` 里是 `6 -> anon_inode:rwproc`；
    搜索/读写/refine 全部正常（ioctl 路径与 read 路径耗时持平，0.11~0.18s）。

   **为什么不能"纯匿名、proc 节点完全不建"**（这条别再去试，会白做一轮）：
   `fd_install()` 装的是**发起该系统调用的进程**的 fd 表。它只能在用户态
   触发的路径里执行，而用户态第一次触发的唯一入口就是 proc 节点
   （`anon_channel_install_fd()` 的注释里有 2026-10-10 的真机验证：
   放 init 里只会装进 insmod 进程，insmod 一退 fd 就没了）。
   所以 proc 节点必须保留 —— 它是 bootstrap，也是 agentd 崩溃重启后的重连入口。

   **改动要点**（`agentd/driver.c`）：
   - `rwproc_call_ex` 内部判断 `use_anon = (rwproc_anon_fd >= 0 && fd == rwproc_mem_fd)`，
     命中就换 fd + 走 ioctl；**断点模块仍走 proc read**（只有内存模块启用了匿名），
     两个 fd 不能混用。
   - 取 fd 用 `rwproc_call`（out=NULL）而不是 `rwproc_call_ex`：驱动返回值就是
     fd 号，而 call_ex 的 out 长度校验会把 "r=fd号" 当成回写长度，fd 号大一点
     就误报 `result overflow`。
   - `ioctl` 的 cmd 必须 ≥ 33（驱动用它做布局校验），arg 指向 `[33B 头][payload]`。
   - 拿不到时只记日志、继续用 proc read —— 不致命。

   **仍然存在的暴露面**：proc 节点还在（0600 + readdir 隐藏），知道 key 就能开。
   想再进一步只能接受"agentd 不能重启"（删节点 = 断了重连入口），或者
   让驱动主动往指定 pid 塞 fd（`task_work_add`，但 pid 要在 insmod 时传）。

32. **fd 表才是最大的暴露面；两个模块现在都走匿名通道**（2026-10-23 定案）。

   ### fd 表比节点本身更容易被命中

   agentd 的 `/proc/<pid>/fd` 里，proc 节点的 fd 会显示成
   `N -> /proc/<key>/<key>` —— **扫一遍 fd 表就能发现节点，不需要知道 key**。
   之前几轮都在优化"怎么藏节点"，没看"谁还开着它"。

   现在两个模块都升级到匿名 inode：

   | 模块 | 通信 | fd 表显示 |
   |---|---|---|
   | rwNova33（内存） | ioctl | `anon_inode:dmabuf` |
   | rwNova（断点） | ioctl | `anon_inode:dmabuf` |

   拿不到匿名 fd 时才退回 proc read（`pmfd<0` 式兜底，不影响功能）。

   ### 三个改动点（按踩坑顺序）

   1. **匿名 inode 名字 `"rwproc"` → `"dmabuf"`**：原名等于在 fd 表里写着
      "内存读写通道"，dmabuf 是 Android 上极常见的内核对象。
   2. **拿到匿名 fd 后立刻 close 掉 proc fd**（`rwproc_anon_upgrade()`）：
      节点路径从 fd 表消失；`rwproc_mem_fd` 直接改指匿名 fd，所有
      `rwproc_mem_fd < 0` 的判断照旧成立，调用点不用改。
   3. **⭐ 按 fd 归属取匿名 fd**（这是 hwbp 的 bug，害我排查了两轮）：

      ```c
      // 错：不管哪个模块都取内存模块的 fd
      int anon_fd = use_anon ? d->rwproc_anon_fd : -1;
      // 对：
      if (d->rwproc_anon_fd >= 0 && fd == d->rwproc_anon_fd)      anon_fd = d->rwproc_anon_fd;
      else if (d->rwproc_bp_anon_fd >= 0 && fd == d->rwproc_bp_anon_fd) anon_fd = d->rwproc_bp_anon_fd;
      ```

      症状极具误导性：断点模块的命令被发到内存模块的匿名 fd 上，
      `GET_NUM_BRPS(3)` 被当成 `READ_PROCESS_MEMORY` 执行，返回 **-14 (EFAULT)**
      （空句柄），表现为 `ioctl(cmd=3) failed: Bad address`。
      而 `getCpuNumBrps()` 是纯位运算 `((read_cpuid(ID_AA64DFR0_EL1)>>12)&0xf)+1`，
      根本不可能返回负数 —— 这个矛盾就是定位的突破口。

   ### 排查工具（`build/ko/_dev/`，不入库）

   - `anon_probe.c`：单模块 —— 分别测 proc read / 取 anon fd / anon ioctl 三步。
   - `anon_two.c`：复现 agentd 的双模块 fd 序列，打印每个 anon fd 上 `cmd=3`
     的返回值，**一眼看出 fd 归属**（返回 6 = 断点模块，返回 -14 = 内存模块）。
   下次怀疑"fd 发错模块"，先跑 `anon_two`，别再靠日志猜。

   ### 仍然做不到的（架构限制，别硬试）

   - **proc 节点不能删**：`fd_install()` 装的是发起系统调用的进程的 fd 表，
     只能在用户态触发的路径里执行；而用户态第一次触发的唯一入口就是 proc 节点。
     节点同时是 agentd 崩溃重启的重连入口。
   - **key 不能随机化**：key 是路径名，agentd 靠它 open；而 readdir 又被
     `hide_procfs_dir` 过滤掉了，随机化后 agentd 无从发现（死锁）。
     要突破只能让驱动用 `task_work_add` 主动往指定 pid 塞 fd，pid 得在 insmod
     时传、agentd 重启即失效 —— 那是换一种运维约束，不是"修好"。

   ### 测 hwbp 的坑

   ARM64 只有 6 个硬件断点槽。`gg_watch_set` 之后**必须 `gg_watch_clear`**，
   否则后续 itest 的 `hwbp INST` 返回 -1（表现为"代码坏了"，实际是槽被占）。
   另：itest 的 hwbp 段在探测失败时会打 `[skip]` 然后继续，
   **`ITEST ALL PASS` 不等于 hwbp 验过** —— 必须看有没有 `bp id` 那两行。

33. **隐藏性自检脚本 `scripts/check_stealth.sh`**（2026-10-23）。

   改完驱动或 agentd 的通信层，**先跑这个再谈"改好了"**：

   ```
   adb push scripts/check_stealth.sh /data/local/tmp/cs.sh
   su -c 'cp /data/local/tmp/cs.sh /data/adb/GG-Ghost/check_stealth.sh
          chmod 755 /data/adb/GG-Ghost/check_stealth.sh
          sh /data/adb/GG-Ghost/check_stealth.sh'
   ```

   12 项检查，任一 FAIL 退出码 1：

   | # | 检查项 | 判据 |
   |---|---|---|
   | 1 | dmesg | 无 rwproc/rwnova/rwproc33 字样 |
   | 2 | /proc/modules | 查不到模块 |
   | 3 | /sys/module | 查不到模块 |
   | 4 | ls /proc | 查不到两个 key 目录 |
   | 5 | 节点权限 | `-rw-------` |
   | 6 | agentd fd 表 | 无 `/proc/<key>/<key>` 链接 |
   | 7 | 匿名 fd 数 | ≥2（两个模块都升级） |
   | 8 | fd 名 | 无 `anon_inode:rwproc`（应为 dmabuf） |
   | 9 | kallsyms | 无 `[rwNova` 符号 |
   | 10 | debugfs kprobe | 未挂载 或 无 proc_root_readdir 探针 |
   | 11 | 非 root 打开 | 必须被拒 |
   | 12 | 功能 | agentd 日志有 `channel OK`（隐藏不能以功能为代价） |

   第 12 项是刻意加的：**只验"看不见"不验"还能用"，很容易把驱动改瘸了还庆祝隐藏成功**
   （本轮 hwbp 的 fd 归属 bug 就是这样 —— 隐藏全过、功能全挂）。

   当前实测：**PASS=12 FAIL=0**。

34. **CFI 补丁无法消除（别再试编译期方案）**（2026-10-23 论证）。

   `cfi_bypass.h` 干的事：用 `aarch64_insn_write` 逐 4 字节把
   **`__cfi_check_fail` / `__cfi_slowpath` / `__ubsan_handle_cfi_check_fail_abort`**
   这几个失败处理函数改成空操作。它不碰检查逻辑本身，只让"检查失败"不 panic。

   为什么必须这么做：内核开 `CONFIG_CFI_CLANG=y` 后，**所有间接调用点**都插了
   `__cfi_check()`。hwbp 把 handler 注册成 perf_event 回调，内核在**自己的调用点**
   用函数指针调它 —— 这个检查在调用方，不在模块里。

   所以**编译期关掉模块的 KCFI 插桩（`-fno-sanitize=kcfi`）解决不了**：
   模块自己少插几个检查，内核侧的检查照旧，回调一进来就 panic。
   真要用编译期方案得连内核一起改（不现实）。

   现状风险：patch 常驻 → 内核 text 与磁盘 Image 不一致，理论上完整性自检能抓到。
   判据是"需要 root + 主动算 text hash"，比 dmesg/fd 表那类被动暴露面高得多。
   **做隐藏性优化时不要动这块** —— 关掉直接 panic，且没有替代路径。

   顺带：`disable_kprobe_blacklist()` 也在同一个 `#ifdef` 里，它是给
   `rw_sym_lookup` 探敏感符号用的（`modules` 链表等），关掉会让 UNHIDE 逃生舱失效。

35. **PC 侧搜索界面 `gg-ui`（imgui + Win32 + D3D11）**（2026-10-23）。

   ```
   scripts\build_ui.bat            # 产物 build\pc\gg-ui.exe（静态链接，只有系统 DLL 依赖）
   build\pc\gg-ui.exe --pid 5462 --search --value 100 --type dword
   ```

   **刻意不走 MCP 传输层**：UI 直接链接 `server/session.cpp`，调
   `Session::call_tool(name, args)`。协议、参数校验、新工具全部自动继承 ——
   再加一个前端不用碰 wire。改 UI 时只要认 `call_tool` 这一个入口。

   ### 三个坑（都踩过，别再踩）

   1. **imgui 必须用稳定 tag**（现在 vendored 的是 **v1.91.9**）。
      先用 master（当时是 1.93.0 WIP）：字体明明加载成功（标题栏
      `font=build\msyh.ttc nfonts=1`），界面中文却全是方块 ——
      那一版的 font system 正在改。换 1.91.9 立刻正常。
   2. **字形范围用 `ImFontGlyphRangesBuilder` 手动拼**（Default + 常用 2500 字）。
      只传自己列的字符表一定漏字（第一版漏了"模糊/首次/进程"，界面上是方块）。
   3. **字体文件放 `build/`**：本机实测直接从 `C:\Windows\Fonts\msyh.ttc`
      加载会失败，复制一份到项目内就正常。`build_ui.bat` 会自动复制
      （19MB，**不入库**）。

   ### 调试技巧

   窗口标题带 `font=<实际加载的路径> nfonts=<字体数>`。字体出问题时截个图就能
   区分"没加载"和"加载了但 glyph 没烘进去"，不用加日志、不用猜。

   ### 现状

   已实测：启动自动连接 → 913 个进程列表 → attach systemui →
   搜索 dword=100 得 23168 条 → 表格显示前 1000 条。中文全正常。

   已知限制：结果不分页（只显示前 N 条）、没有 watch/断点面板（低频操作仍走 MCP）、
   进程列表一次拉全量靠过滤框缩小。搜索是同步调用（全量 0.2s / 模糊首帧 0.3s），
   会短暂卡界面 —— 要异步就把操作丢线程，别在 UI 线程里 sleep 等。

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

本文件最后校准：2026-10-23（清掉 5 条已清/被取代条目，修正 main.c 行数与分片状态）。
