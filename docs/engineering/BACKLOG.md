# 项目体检 / 待办清单

> 2026-10-23 夜。整理自最近几轮实际动手时的观察，**每条都带证据或数据**，
> 不是"感觉可以优化"。
>
> 优先级定义：**P0** 收益明确且改动小；**P1** 收益明确但成本中等；**P2** 需要
> 架构级改动、要权衡；**不做** 已论证过不划算或不可行。

## 已完成（近期）

### UI ↔ AI 联动（2026-10-23）

用户需求："我手动搜索的值可以与 AI 联动"。

做成了双向：`gg_context` 让 AI 读到人在 UI 里搜的值/类型/区间模式/选中行；
`gg_ui_command` 让 AI 反过来驱动窗口；UI 每 900ms 比对 `result_count`，
AI 在后台搜完人的表格自动跟上。

关键认知：**agentd 的 `g_eng` 是全局单例，UI 和 MCP 连同一个 agentd 时
结果集本来就是同一份**，缺的只是"对方在干什么"。详见
`docs/engineering/GG_UI.md` 的"与 AI 联动"一节和 `AGENTS.md` 第 36 条。

---

### 靶场实测：wire 延迟 / pointer_scan / 日志（2026-10-23 中午）

一次靶场跑出三个问题，都是"功能全对、手感不对"那类：

| 问题 | 现象 | 根因 | 结果 |
|---|---|---|---|
| wire 小帧 40ms | 单点 READ 稳定 40.0ms，64KB 读只要 19ms | `send_frame` 两次 send + 未关 Nagle，撞上对端 delayed ACK（40ms） | 0.44ms（90x） |
| pointer_scan 11.7s | 微信 585MB 扫 2833 目标 | 没接 pagemap 跳过，九成时间在读 absent 页 | 1.1s（10.3x） |
| 日志一天上万行 | `conn:` 每请求一行 | UI 每 900ms 轮询 result_count | 只在协议异常时记 |

顺带删掉整个事件广播子系统（`events.c`），详见 `AGENTS.md` 第 38-41 条。
新的回归探针：`python tests/integration/lat_probe.py`（直连 34127 量单次延迟）。

---

## P0 — 收益明确、改动小

### 1. UI 结果分页

现状：只显示前 `显示上限` 条（200~20000），超过就看不到。搜索常出几万条，
但真正要看的往往是"某一段地址范围内"的。
做法：加 offset 翻页 / 按地址区间过滤（`gg_get_results` 已支持 offset/limit）。
成本：约 30 行。

**状态：翻页已完成（2026-10-23）**，按地址区间过滤没做（有分页后优先级降了）。
- 标题行 `结果 N 条 | 第 X - Y [< 上页] [下页 >]`，默认 1000 条/页
- `page_offset` 在 attach / 精确 / 区间 / 模糊首帧 / 清空时归零
- `refresh_results` 开头把 offset 夹回合法范围（结果变少时落到最后一页）
- `--page N` 命令行参数走同一条路径，便于脚本验证
- 实测：第 2 页显示 "第 1001 - 2000"、`offset=1000`；`--page 100` 自动夹到
  offset=9000 返回 832 条

### 2. 批量写入

现状：只有"批量冻结前 N 条"。批量写入（把搜索结果全部改成同一个值）是同类需求。
做法：复用 `do_freeze_all` 的循环，换 `gg_write`。
成本：约 15 行。**风险**：写错值不可逆 —— 必须保留数量上限和二次确认。

**状态：已完成（2026-10-23）。**
- 只作用于当前页已加载的 hits；上限 `min(页内条数, N 滑条, 500 硬顶)`
- 界面二次确认：第一次点变红"确认写 N 条？"，3 秒不点自动取消
- 写完自动 refresh_results，能直接看到值变化
- CLI `--write-value V --write-all` 绕过确认，只给靶场/脚本用
- 验证：新增靶子 `build/ko/_dev/wt_target.c`（64 个相同 volatile 全局变量，
  不自增，只在 SIGUSR1 时改值），实测 66 条命中 → 批量写 999 → 65 成功
  （1 个只读页 short write）→ 靶子 slot0 变 999、表格 hex 显示 0x3e7

### 3. `hwbp_proc.c` 是死文件

现状：`hwBreakpointProc_module/Makefile` 只编 `hwBreakpointProc_module.o`，
`hwbp_proc.c`（约 700 行，含 cdev/`/dev` 那套）**从不参与编译**。
它里面的 cdev/device_create 曾经误导过排查（以为有 `/dev/rwNova` 通道）。
做法：移到 `_attic/` 或文件头加显著注释；不建议直接删（含 AFEI 的 ioctl 参考实现）。
成本：5 分钟。**状态：已完成（2026-10-23，文件头加了 20 行显著标注）。**

### 4. `rwproc_call_ex` 的 out 长度校验

现状：`n = (r > 0) ? r : out_cap` —— 把"系统调用返回值"当"回写长度"。
对 `CMD_ANON_GET_FD` 这类"返回值就是 fd 号/句柄"的命令会误报 overflow
（本周就踩了，fd 号 > out_cap 就报错）。

**状态：已完成（2026-10-23），但没按原计划改签名。**
原因：现存 7 个调用点全是"长度语义"，为了一条还没暴露的坑去改 7 处签名，
引入的回归风险大于收益。改成**结构护栏**：

- `agentd/driver.c` 的 out 分支加了契约注释（返回值 = 回写字节数；标量命令
  必须用 `rwproc_call(out=NULL)`）
- `tests/unit/check_scalar_cmd.py`：扫描所有 `rwproc_call_ex` 调用点，
  发现标量命令（`SCALAR_CMDS` 白名单）走了它就失败，已接入 `run_tests.bat`
- 正反两向验证过：副本上把 `OPEN_PROCESS` 改成 `ANON_GET_FD` → 退出码 1

以后再遇到同类命令，加进白名单即可自动挡住。

### 5. `search_range` 不设 `exact_vt`，`get_results` 值只显示低 32 位

**现象**（2026-10-23 登山赛车实测）：`gg_search_range(type=qword)` 搜
`0x6D6630BDD4..0x6D6630BFD4`，命中地址正确，但 `gg_get_results` 返回的
`value` 是 32 位（如 `1714470764` = `0x6630BE6C`）。用
`gg_read type=qword` 读同一地址，得到的是正确的 64 位值 `0x6D6630BF6C`。

**根因**：`h_get_results` 用 `results.exact_vt` 决定读值宽度
（`int type = VT_DWORD; if (exact_vt > 0) type = exact_vt;`），
而 `search_range` **从不设置 `exact_vt`** —— 于是沿用了上一次 dword 搜索的
残留类型，按 4 字节读 8 字节指针。

**修法有个坑**：只补 `r->exact_vt = vtype` 会引出更糟的问题 ——
`search_refine_exact` 在 `exact_vals == NULL` 时会用 `exact_value` 填满所有
候选（前提是"所有候选都等于搜索目标"，精确搜索成立、区间搜索不成立），
于是首次 refine 把基线当成 0，候选全被判成 changed。

**方案**：加 `int exact_vals_valid` 字段。`search_exact` 填完 exact_vals 设 1；
`search_range` 设 `exact_vt = vtype` 且 `exact_vals_valid = 0`（不填 exact_vals）。
`search_refine_exact` 遇到 `!exact_vals_valid` 时先跑一轮"只读基线、不筛选"
（返回进来的 count），下一轮才真正比较。

成本：约 40 行（结构体 1 字段 + 3 处设置 + refine 一个分支）。
**收益**：区间搜索是找指针的主力工具，结果不可信等于不能用。
本轮绕过方式：拿到地址后用 `gg_read type="qword"` 逐条核对。

## P1 — 收益明确、成本中等

### 6. 搜索真正异步

现状：UI 里搜索是同步调用（全量 0.19s、模糊首帧 0.32s），期间界面卡住。
做法：操作丢进 `std::thread`，结果进队列，UI 线程只做展示；按钮置灰 + 转圈。
成本：约 80 行（要处理"搜索中再点搜索"、"attach 打断搜索"）。
注意：`Session` 内部有锁，但**并发调 call_tool 的语义没验证过**，需要串行化。

### 7. UI 接模块基址 / 指针链

现状：`gg_module_list` / `gg_module_base` / `gg_pointer_scan` / `gg_pointer_chain`
都有工具，UI 没接。找基址+偏移是仅次于搜索的高频操作。
成本：每个约 40 行（面板 + 结果显示）。

**状态：已完成（2026-10-23）。** 两个独立浮窗：
- 模块列表 (F2)：`gg_module_list`，900+ 条带过滤，双击填 base；
  `module_dirty` 标记控制重拉（不能用"列表空就拉"，失败会每帧重试）
- 指针 / 符号 (F3)：一步按钮分别接 `gg_resolve_symbol` / `gg_pointer_chain` /
  `gg_pointer_scan`；扫描完自动把第一个 container 填进地址框
- CLI：`--modules` / `--pointer` / `--ptr-addr 0x...` / `--analyze`
- 修了两处误导性文案：`mapped` 是"指针指向的目标"，不是当前地址；
  匿名映射的 `module:""` 显示成"不在任何模块内"
- `gg_pointer_scan` 的字段名以实现为准（`hits:[{container,target,ptype}]`），
  UI 里最初猜的 `pointers/results` 是错的

### 8. 驱动读路径合并连续物理页

现状：`fast_read_linear` 每页一次 `kmap_local_page` + `memcpy`，实测 559ns/页
（物理读部分）。而 `/proc/<pid>/pagemap` 在本机**能读到真实 pfn**（未被屏蔽）。
做法：用 pagemap 的 pfn 判断连续物理页，一次 `memcpy` 跨多页。
预期：物理读部分 1.5~2×（总耗时从 0.19s 到 ~0.12s 量级）。
**风险高**：这块是"设备重启"和"静默漏读"两次事故的现场，改完必须跑
`check_stealth` + `run_itest` + 64KB 区间集合比对。

### 9. 搜索快照内存上限

现状：`GG_FUZZY_MAX_BYTES` 默认 256MB，超过的 region 不纳入扫描（模糊搜索会漏）。
做法：超限时明确提示"只扫了前 256MB"，并列出被跳过的 region。
成本：约 20 行。**收益**：避免"搜不到"被误当成"值不存在"。

### 10. `pointer_scan` 支持区间匹配（指向对象任意成员）

现状：只做精确相等（`v == target`）。真实 C++ 对象里，持有者存的是**对象首址**，
而目标字段常在 `+0x14` 这类偏移上 —— 精确匹配必然搜不到。

证据（2026-10-23 登山赛车）：金币在对象 `+0x14`，有 3 个容器精确指向它，
但对这 3 个容器再搜"谁指向我"→ **0 命中**。搜对象首址才有意义。

做法：参数加 `range`（默认 0，保持现有语义）：命中条件改为
`v >= target - range && v <= target + range`。
成本：约 30 行（bsearch 换成区间判断，或对每个 target 做二分下界）。
**收益**：一次抓出所有指向该对象的指针，不用每换一个成员偏移就重扫全内存。

### 11. `search_range` 按元素宽度对齐步进

现状：`agentd/search.c` mode 1 分支是 `for (...; i++)` —— **每字节**都试。
后果：一个 8 字节指针落在区间内会被命中 **8 次**（i..i+7 的未对齐窗口都满足），
结果数虚高 8 倍、10 万条 cap 很快打满，命中的地址还是"未对齐窗口起点"。

ARM64 指针/浮点天然按 4/8 字节对齐，按 `sz` 步进既正确又快 8 倍。
（exact 模式走 memchr 首字节预筛 + `i = off + 1`，不受影响。）

做法：`i += sz`。成本：1 行 + 单测。
**风险**：如果有调用方依赖"非对齐命中"（比如找 packed 结构），要保留开关。

## P2 — 需要架构级权衡

### 12. 驱动主动塞 fd（彻底消除 proc 节点）

现状：proc 节点必须保留（`fd_install` 只能由用户态触发，节点是唯一入口 + 重连入口），
key 也因此不能随机化。详见 AGENTS 32。
做法：模块参数传 pid，init 时 `task_work_add` 往目标进程塞 fd，节点建完即删。
代价：agentd 重启后 pid 变了就失效，必须重新 insmod（且隐藏模块 rmmod 不了，
只能装不同模块名的副本）。**这是换一种运维约束，不是纯收益。**

### 13. 匿名通道的 fd 名

现状：`anon_inode:dmabuf`（已从中性的角度选过）。
可再进一步：用 `eventpoll` / `sync_file` 之类更常见的内核对象名。
但区别已经不大（都混在 graphics/io 类 fd 里）。

### 14. AGENTS.md 拆分

现状：35 条、614 行。已经出现"同一条里塞了三件事"的情况。
做法：按主题拆成 `AGENTS.driver.md` / `AGENTS.agentd.md` / `AGENTS.build.md`，
主文件只留索引 + 硬约束。**收益**：新人上手快；**成本**：一次大整理，容易漏引用。

## 明确不做（附理由）

- **CFI 补丁还原**：检查在内核侧调用点，模块编译期管不到；关掉直接 panic。
  详见 AGENTS 34。
- **搜索结果自动刷新值**：每帧重读几百个地址会让设备 IO 打满，且会干扰
  目标进程（读会触发缺页）。要刷新就手动点，或者只刷选中项。
- **UI 里做 watch/断点面板**：低频操作，且 ARM64 只有 6 个断点槽，
  在 UI 里误点一下就把槽占满（本周已经踩过一次，导致 itest 假失败）。
  这类操作留在 MCP 侧，带显式参数和清理步骤。

## 数据留档（做优化前先看这些基线）

| 场景 | 当前 | 备注 |
|---|---|---|
| 精确全量搜索（341MB / systemui） | 0.19~0.22s | 结果 2 万条量级 |
| 精确全量搜索（585MB / 微信） | 0.24~0.46s | 结果 2000~2500 条（writable 0.31s） |
| 模糊首次扫描（256MB 快照） | 0.31s | 6710 万元素 |
| 模糊 refine | 0.26s | 全量重读，未按候选页优化 |
| **wire 单点读写** | **0.44~0.46ms** | 直连；修 Nagle 前是 40.0ms / 35.4ms |
| **wire 读 64KB / 256KB** | **24ms / 93ms** | 受 USB 带宽限制（~2.8MB/s），非驱动 |
| **pointer_scan（2833 目标）** | **1.1s** | 修 pagemap 前 11.7s |
| **get_results 分页 10000 条** | **89ms** | 850KB JSON |
| 驱动单页成本 | walk 357ns + phys 559ns | 逐页 4KB |
| check_stealth | PASS=12 FAIL=0 | 改驱动后必须复跑 |
| run_itest | ALL PASS | 含 hwbp 三项 |
