# AGENTS.agentd.md — 设备端 agentd / 搜索引擎 / 会话（AGENTS.md 分册）

> 2026-11-04 从 AGENTS.md 拆出（原 50 条/963 行，见 BACKLOG P1-15）。
> **编号沿用主文件历史顺序，不重排**（条目间互引"见第 N 条"依赖原编号）。
> 跨文件引用请回主文件《已知债索引》查条目所在分册。

1. **main.c 剩余 683 行**（2026-10-23 `wc -l`）。已拆 shell（`cmd_shell.c`）、
   view/符号/指针链（`cmd_view.c`）、搜索与结果集（`cmd_search.c`）、内存读写/冻结
   （`cmd_mem.c`）、进程/会话（`cmd_proc.c`）、分析域（`cmd_analyze.c`）、
   状态（`cmd_status.c`）、虚拟触摸（`cmd_touch.c`）、屏幕（`screen.c`）、
   出站帧（`wire_io.c`）、握手鉴权（`wire_auth.c`）。
   剩 dispatch + 连接生命周期 + 参数解析 helper 留在 main.c，**不再拆**：
   dispatch 是命令表的唯一映射处，拆出去只会让"命令→handler"多跳一层。
   各类 handler 的声明统一在 `engine_internal.h`，不要在 `cmd_*.c` 里重复前置声明。

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

14. ~~**`gg_search_undo` 只回退模糊搜索**~~ —— 已被第 16 条取代。历史状态：位图环形栈 3 级、
   不回退数据快照，精确搜索误操作只能重搜。

16. **`gg_search_undo` 现在覆盖模糊与精确/区间两种搜索**（两套独立撤销栈，
   深度都是 3）。模糊存位图副本（256MB 区 8.4MB/级），精确存 addrs+vals
   副本（≤1.6MB/级）。

17. **`gg_disassemble` 要求 addr 按 4 字节对齐**，否则直接报错。ARM64 从非
   对齐地址解码会得到一两条"看着像指令"的垃圾然后停住 —— 实测 `count=1`
   但 `bytes_read=48`，很容易被当成真有个怪函数。

19. **search.c 的 log_msg 是 weak 默认实现**。单元测试常只链接 search.c，
   没有 main.c —— 加诊断日志不用再给每个测试补 shim。产品构建里 main.c 的
   强符号会覆盖它。同理，新增其他跨文件依赖时优先考虑 weak 默认实现。

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

37. **main.c 拆分完成**（2026-10-23）。2049 → **658 行**，方案见
   `docs/archive/MAIN_C_SPLIT.md`（含执行记录与三处偏差）。

   | 文件 | 内容 |
   |---|---|
   | `wire_io.c` | 出站帧（send_frame/fragmented/ok/err）+ `g_send_mtx` |
   | `events.c` | **当晚又整体删除**（见第 38 条）。`clients_*` 搬到 `wire_io.c`，`watch_hit_json` 搬到 `cmd_analyze.c` |
   | `wire_auth.c` | g_wire_key / ct_equal / 临时密钥 / h_auth |
   | `cmd_proc.c` | ping/self_test/list/attach/detach/target_info/module_list |
   | `cmd_analyze.c` | disassemble/watch/module_base/calc_offset/pointer_scan |
   | `cmd_touch.c` | ghost_tap / ghost_swipe |
   | `cmd_status.c` | kernel_status / device_info / subscribe / relay_event |

   **main.c 只剩**：全局状态、log_msg、参数解析 helper、`j_addr_u64`、
   dispatch（182 行）、conn_thread、print_usage/hide_process、main()。

   ### 拆的时候踩到的坑（下次拆别再踩）

   1. **按"函数区间"一刀切，会连带删掉别人留下的前向声明。**
      原 main.c 在 `h_kernel_status` 之后有一小段
      `/* cmd_view.c（拆分外迁） */` + 3 个声明 + `h_shell` 声明，
      混在 handler 区里。按区间删除就一起没了，编译报 implicit declaration。
      现在统一收在 `engine_internal.h` 的对应分段里。
      **删区间前先 grep 区间内有没有 `^void h_` 这类前向声明。**
   2. **方案的边界不一定对。** `j_addr_u64` 夹在"wire 鉴权密钥"区中间，
      不能整块搬 —— 要切成两半，只搬它之前的部分。
   3. **行数目标别信估算。** 方案估 450，实际 658。差额是 includes(43) +
      全局状态注释(33) + helper(60) + dispatch(182) + main(182) +
      conn_thread/usage(60) ≈ 575 的必然结果。**658 就是合理终点。**

   ### 每步的验收（照抄，别省）

   ```
   scripts\build_ndk.bat          # agentd
   scripts\build_pc.bat           # PC server
   tests\unit\run_tests.bat      # 单测 + 5 道护栏（dispatch 一致性会校验 46 工具）
   tests\integration\run_itest.ps1   # 真机（设备在线时）
   ```
   本轮实测：全绿；itest ALL PASS（含 hwbp INST/GET_HIT_COUNT/UNINST）；
   dispatch 的 `GGCMD_*` case 数拆分前后都是 42，diff 无差异。

   ### 拆分时保持不变的约束（写在各自文件头）

   - `wire_io.c`：负载混淆作用于副本；分片逐片独立 XOR 且同锁发完
   - `wire_auth.c`：`ct_equal` 常量时间；密钥永不进日志；空 key 是 fail-closed
   - `cmd_proc.c`：`session_guard()` 在 `lock(mtx)` 之后、`switch` 之前
   - `cmd_analyze.c`：`g_hwbp_probe` 是全局缓存的（探测有副作用，别每请求重探）
   - `cmd_status.c`：`kernel_status`/`device_info` 走无锁快速通道，别挪进锁内

38. **事件广播子系统已删除（2026-10-23）—— 别再按"订阅推送"设计功能**

   `events.c` / `last_resp.h` / `test_last_resp.c` 整体删除，配套：
   - `SUBSCRIBE_EVENTS` / `UNSUBSCRIBE_EVENTS` / `RELAY_EVENT` 枚举值保留（不复用），
     dispatch 里统一回 `command removed: events subsystem was deleted in AI-first mode`
     —— 客户端要看到"这个功能没了"，不是等到超时
   - 分片注册表 `clients_register/clients_set_frag/client_supports_frag` 搬到 `wire_io.c`
     （只剩 `frag_ok` 一个标记，`send_ok` 超长时查它）
   - `watch_hit_json` 搬到 `cmd_analyze.c`（poll 是唯一消费路径）
   - **watch 一直是 pull 模型**（`watch_poll`），删推送不改它的语义
   - `engine_internal.h` 里 4 个 `event_*` 空宏只是让旧调用点先能编译，
     **新代码不要再用**；见到就顺手删掉调用
   - PC 端连接后不再发 `SUBSCRIBE_EVENTS`（`server/device/wire.cpp`）

   理由：推送只服务手机悬浮窗，PC 侧一直是空的。留着 = 一个常驻线程 + 一套订阅
   状态机 + 每个 handler 都要维护的响应存档（`last_resp`）。

40. **pointer_scan 曾漏接 pagemap 跳过（2026-10-23 修，11.7s → 1.1s）**

   可写区里绝大部分是 malloc 保留但从未触碰的页。原实现逐块 256KB `driver_read`，
   读失败后靠 `gg_fault_advance` 一页页推进 —— 微信 585MB 扫 2833 个目标要 11.7s，
   九成时间在读不存在的页。搜索路径（第 30 条 X2）早接了 `pm_open`/`pm_try_skip`，
   指针扫描漏了。

   - 跳过是**等价**的：absent 页读出恒为 0，而目标是地址，`v==0` 分支已直接跳过
   - `pmfd < 0`（pagemap 打不开）时 `pm_try_skip` 恒返回 0，自动退回逐页探测
   - **凡是"扫一片内存"的新功能，先确认有没有接 `pm_try_skip`**，否则默认慢 10 倍

41. **日志别逐请求打（2026-10-23）**

   `conn: magic=...` 每请求一行，UI 每 900ms 轮询 `result_count` → 一天上万行。后果：

   - 启动期 `driver self-test: channel OK` 被挤出 `check_stealth.sh` 的 `tail -c 2000`
     窗口 → 自检**误报 FAIL**（差点被当成模块坏了）
   - 白写 flash

   现在只在协议异常时记（bad header / payload 超限 / payload 读失败）。
   `check_stealth.sh` 第 10 项也改成锚定"最后一次 `starting (port=`"之后的日志，
   不再看固定尾部窗口。

42. **真实目标实践：判据是"同址前后值交集"，不是"搜到新值就行"**（2026-10-23）

   第一个真实商业游戏（登山赛车）跑通"定位 → 改值 → 游戏接受 → 落盘 → 重启仍在"，
   全过程见 `docs/archive/PRACTICE_HILLCLIMB.md`。

   **分工**：AI 主力（attach / 搜索 / 收敛 / 读写 / 截图 OCR / 验落盘），
   人辅助（在游戏里操作、确认界面）。**AI 不要去模拟游戏内点击**。

   **判据**：
   ```
   A = 值=V 时的地址集合     B = 值=V' 时的地址集合
   A ∩ B  →  真变量（地址不变、值跟着变）
   ```
   本轮 9670 搜出 56 条，9775 搜出 45 条，交集 **1 条**，一次命中。
   单纯搜新值全是噪音（关卡数据里一堆同值）。

   **三个必踩的坑**：
   - **界面数字有千位分隔**：`9 775` 被读成 "7775"，搜错值白跑一轮。
     读数值一律截图裁剪 + OCR，不靠转述。
   - **批量写会稀释有效信息**：8 条里只有 1 条真的，一起写看不出效果 →
     候选 > 3 条先收敛，最后**单点**写验证。
   - **UI 与 AI 共用一份全局结果集**：UI 一 attach 就 `session_drop`，
     AI 建的模糊基线直接没了（`refine failed (search first?)`）。
     → 要建基线先跟人说别在 UI 里 attach/搜索；长期方案见 BACKLOG 多结果集。

   **截图坐标**：`screencap` 是 2608x1200，看图工具会缩到 ~1080 宽，
   按缩略图估坐标换算回去偏几十像素（`ghost_tap` 全打空）。
   用 `scripts/find_button.py`（纯标准库找色块）或 `scripts/crop_png.py` 裁剪后定位。

44. **搜索/断线三个 P0 级修复（2026-11-03，BACKLOG 5/6/12 收口）**。

   - **区间搜索结果值宽度**（BACKLOG 5）：`search_range` 不设 `exact_vt` →
     `get_results` 恒按 dword 读值，qword 指针只显示低 32 位。已修：
     `exact_vals_valid` 字段区分"搜索即基线"（精确）与"首 refine 建基线"
     （区间/AoB）。**行为变化**：区间/AoB 搜索后首次 `gg_search_refine`
     只建基线（count 不变、op 被忽略），第二次起才按 op 收缩——schema
     描述已注明，写自动化流程时别假设第一轮就收敛。AoB 顺带修了快照
     全 0 的旧缺陷（UNCHANGED 误判）。单测 test_search.c 用例 10/10b。
   - **PC 端断线重连**（BACKLOG 6）：`ensure_connected_retry()` 指数退避
     （总预算 min(30s, deadline)），auth 拒绝不重试。**「进程退出」的根因
     未定案**——但修掉一条可证 terminate 路径：`protocol.cpp` 的
     `resp.dump()` 原在 try 外，响应含无效 UTF-8 直接打死进程。重连的
     运行时行为**未本地验证**，下次真机 itest 顺带跑。
   - **`aligned` 步进**（BACKLOG 12）：早已实现，状态补记。
   - 详见 `docs/engineering/BACKLOG.md` 对应条目的状态段。

45. **`gg_pointer_path`：多级指针链反向搜索（2026-11-04，真机未验）**

   补齐指针工作流的最后一环。现有两个工具各管一半：`gg_pointer_scan` 只找
   **一级**（谁指向它），`gg_pointer_chain` 是**反方向**（按你给的位置逐级
   解引用，用于验证已知链）。缺的是"给目标，自动搜出整条到模块的路"——
   就是 CE 的 pointer scan。

   - `pointer_chain_scan`（search.c）：逐级回溯，每级一次 `pointer_scan`；
     命中 container 落在锚点段即产出一条链，沿 `hit.target` 反查回目标。
   - **锚点 = 路径非空的映射段**（文件映射的 .so/dex/apk 地址同版本稳定）；
     匿名段/[heap]/[stack] 不能当链终点，否则重启即失效。
   - 默认 `lookback=0x1000`：链上每级基本都是"对象首址 + 成员偏移"，
     精确匹配必然断链。`lookback=0` 是给"纯数组指针链"用的。
   - 输出给 `expr`（`[[0x7f..+0x18]+0x24]` 这种 CE 写法）+ `module` +
     `module_offset`，便于换存档/重开游戏后重算。

   **已知限制（别当 bug 报）**：
   - 实现是逐级回溯，**depth 级 = depth 次全内存扫描**（微信量级单级约 10s）。
     没有 ptrmap 一次性建图——桌面 CE 那样做在移动端要吃 1GB+ 内存。
     depth 上限 6 是性能上限，不是能力上限。
   - 每级候选硬上限 2000（`GG_CHAIN_LEVEL_CAP`）。撑满时该级可能被截断，
     表现为"某些链找不到"，会返回 `warning` 字段——此时缩小 `module` 或降 lookback。
   - lookback 会产生噪声链（值恰好落在某目标附近的整数），深度越大越多。
     用 `module` 过滤 + 人工核 `expr` 重读一次值确认。

46. **搜索 module 参数：限定模块范围（2026-11-04，真机未验）**

   `gg_search` / `gg_search_range` / `gg_search_unknown_init` 新增可选
   `module`（子串匹配 maps 路径，如 `"libil2cpp.so"`）。给了就只搜该模块
   的映射段——特征码/静态数据搜索的刚需，此前整片堆一起扫噪声巨大
   （GG 原版的「内存范围=SO 文件」）。

   - 过滤是 `regions_filter_module` 纯函数（search.c，path 子串匹配、
     原地压缩），引擎 5 个函数统一在 `process_search_regions` 之后套。
   - **module 过滤叠加在 region_type 之后**：两者可同传（先按
     anonymous/writable/all 取段，再按模块名筛）。匿名段 path 为空，
     任何 module 都匹配不上。
   - `gg_aob_resolve` 内部的 search_aob 调用**没接 module**（签名适配
     传 NULL，行为不变）——它有自己「先 writable 后 all」的两段逻辑，要加
     得单独设计，别顺手改。
   - 签名变更是破坏性的：引擎 5 个 search_* 都加了 `const char *module`
     尾参，外部如有 fork 的调用点要同步。

47. **`gg_region_dump` / `gg_region_restore` / `gg_dump_list`：内存区域快照（2026-11-04）**

   CE "Memory dump" 对应物：改数值前备份整块区域，改坏了一键写回。
   落盘 `/data/adb/GG-Ghost/dumps/<name>.bin`，32B 头部
   （magic "GGRD"/版本/原址/长度/FNV-1a）。`dump_list` 枚举备份+元信息
   （restore 跨会话不再盲猜 name）。工具面 58。

   - **残缺即失败是语义核心，别"顺手优化"成补 0**：dump 遇短读报错删文件
     （补零备份恢复时会把假零写回目标进程）；restore 前全量校验 FNV，
     文件损坏拒绝写回（坏数据灌进目标进程比不恢复危险）。没有 force
     逃生口 —— is_force_read 在这台内核上 panic（第 27 条）。
   - FNV 用标准 offset basis（`GG_FNV_OFFSET_BASIS`）起步：dump 文件可被
     PC 侧任何现成 FNV-1a 实现独立校验。改成自定义 seed 就丢掉了这个能力。
   - restore 缺省写回 dump 原址（头部里的 orig_addr），显式 `addr` 写别处；
     写操作：server 表 `write_op=true` + agentd safe_mode 双保险。
   - 头部 `payload_len` 超过 `GG_DUMP_MAX_BYTES`(64MB) 在 unpack 里就判非法
     （不依赖调用方先查）；len 双端校验。
   - **核心循环在 dump.c**（`gg_region_dump_to_file/restore_from_file/
     gg_dump_list`，依赖 driver 但无 cJSON/引擎单例），cmd_mem.c handler 只剩
     参数解析薄壳；itest 用例 8 直测核心循环（含 FNV 拒坏文件）。
   - itest 本地交叉编译已过（本轮顺带修了它两处既有断点：search_exact
     的 module 尾参漏同步、`g_ring_enabled` 缺符号）。**教训：itest.c 只在
     真机跑，改 agentd 函数签名时必须连它一起 grep**。

49. **`gg_results_refresh`：手动刷新 / 重置 refine 基线（2026-11-04，真机未验）**

   「明确不做」挡的是**自动**轮询刷新（设备 IO / 干扰目标进程）；
   **手动**刷新是 GG 同名工作流，此前缺工具——get_results 现读现值，
   但 `exact_vals` 基线停在搜索/refine 时刻，值变几轮后 refine 拿老基线比较。

   - 语义：重读全部候选 → 返回 changed/dropped/count，基线更新为现值，
     之后 refine 从"现在"起算。不筛选、只读、读不到的剔除（同 refine）。
   - 无基线路径（区间/AoB/模糊物化后）refresh 直接建基线（借 fuzzy_vt），
     **不压 undo**——压了会恢复出"全零基线 + valid=1"的噪声态
     （与 refine 建基线路径同约定）；有基线才压，刷错可回退。
     这顺带修通了一个缺口：模糊物化后 exact_vt 未定，refine 链路实际断的。
   - itest 用例 14 的判别点：值写回 refresh 时刻值 → refine UNCHANGED
     恰好保留。第一版用 refine CHANGED 判别是错的——新老基线都会保留，
     无判别力。写这类测试先问一句：**如果实现没生效，这个断言会失败吗？**

50. **`gg_freeze_save` / `gg_freeze_load`：冻结表持久化（2026-11-04，真机未验）**

   冻结表活在 agentd 内存，重启即丢 —— 挂机锁血静默失效。与结果集
   save/load 对称落盘 `/data/adb/GG-Ghost/freezes/<name>.json`。
   纯函数在 `freeze.c`（serialize 手工拼接 + deserialize cJSON），
   handler 只剩文件 IO；test_freeze 构建因此要链 cJSON.c。

   - **全或无 + 单锁原子替换是语义核心**：deserialize 离线校验全部条目，
     任一条非法原表不动（半载入的冻结表 = 用户以为全锁上了，实际锁了
     一半）；替换在**同一把锁内**完成 —— 循环线程正在逐条写回，逐条
     add 会露出半旧半新中间态。serialize 侧同理持锁拷快照再拼串。
   - active=0 条目保留：进程退出自动停写的条目，恢复后保持停写。
   - load 后循环线程（attach 时已启动）下一轮就写目标内存：
     safe_mode 拒绝 + server `write_op=true` 双保险。

52. **`gg_region_diff`：内存区域差异比较（2026-11-08，真机未验）**

   CE "compare memory" 对应物，dump/restore 的配套件。纯状态机在
   `agentd/dump.c`（`gg_diff_begin/feed/finish`），文件级驱动
   （`gg_region_diff_files` / `gg_region_diff_live`）也在 dump.c，
   handler 只做参数解析与 JSON 组装。

   - **按区间报告，不是逐字节**：相邻差异间隔 ≤ `gap`（默认 4）合并。
     `span` 是区间跨度、可能含中间未变字节；`diff_bytes` 才是精确差异
     字节数。只报一个数会让"4 字节全变了"和"跨了 1KB 只改了两处"
     看起来一样 —— 别只留 span 或只留 diff_bytes。
   - **两 dump 互比要求原址与长度一致**：不同范围的逐字节比较会得出
     "整段都变了"的假结论，在头部阶段就拒掉。FNV 在扫描循环里顺便
     校验（不额外多读一遍），坏文件拒绝而不是吐满屏假差异。
   - **dump vs live 短读即失败**：读不到的部分不能当作"相同"，那会把
     真实差异报成"没变"。
   - 前缀（`a`/`b`，起点最多 16 字节）**在段起点块内抓一次**：跨块的长段
     前缀就是起点块剩余部分，可能短于 16 —— 段跨块后原指针已失效，
     不能延后补。要完整内容用 gg_hex_dump。
   - `max_regions` 达到后停止扫描并置 `truncated`（此时跳过 FNV 校验，
     已提前结束）。
   - itest 用例 15 覆盖 dump→改→diff→互比→改回归零全链路；
     test_dump.c 覆盖状态机（区间划分/gap 边界/跨块不断段/截断/尾段 flush）。
