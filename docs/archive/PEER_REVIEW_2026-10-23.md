# 外部同类项目对照（2026-10-23）

给接手者的用法：想知道「这个功能有没有现成轮子」「要不要自己做某个能力」，先看这份。
数据抓取时间 2026-10-23，星数为当日快照；结论基于各仓库 README、源码树与元数据，未逐行审计对方实现。

---

> **2026-10-23 17:30 落地**：§4.2 的 `gg_set_results(all=true)` 与新增的 `gg_freeze_results`
> （设备端 `GGCMD_FREEZE_RESULTS`）已实现并通过真机验证，细节见 `BACKLOG.md`「已完成（近期）」。
> 本文其余条目仍是待办/不采纳判定。

## 一、结论

1. **有一个近乎同构的项目：`niqiuqiux/AndroidMiniMem`（29★）。** 它同样是
   「PC GUI + 设备常驻 socket 服务 + 线协议 + 内核驱动 + MCP」四层，且 `hwbp` / ELF 符号 / 指针偏移链 / Lua
   都在工具面上。差别有三点，见 §七：我们自研 KO（它用第三方卡密驱动）、搜索/指针扫描/冻结在我们的工具面
   （它整批砍掉了）、MCP 直连 agentd（它走 Python → HTTP IPC → GUI 三段桥）。
   除此之外的公开项目都是「设备端单体服务 + 直连现成 KPM」。
2. **功能面互补，不是替代关系。** 对方强在实战工作流（GG 本体绑定、XOR 值搜索、批量冻结、快照交叉），
   我们强在分析链（硬件断点、反汇编、符号解析、多级指针、标注库）。
3. **三项待评估的结论：`access_process_vm` 不做；通道伪装我们已是更优解（唯一可抄的是 fd 名，已在 BACKLOG 第 13 条）；
   工具面差集 19 项里，对方最抢眼的 GG 绑定类 7 项经真机实测不成立。真正值得抄的是**参数增强**（§4.2 的 4 项），
   新增独立工具只有 2 个值得做。**

---

## 二、项目清单

| 项目 | ★ | 语言 | 形态 | 定位 |
|---|---|---|---|---|
| [niqiuqiux/AndroidMiniMem](https://github.com/niqiuqiux/AndroidMiniMem) | 29 | C++ | PC ImGui GUI + 设备常驻 socket 服务（`minimem_server`）+ 线协议 + 内核驱动 + MCP | **架构最接近我们**，见 §七。砍掉搜索/指针扫描/冻结，专做「AI 驱动的内存调试」 |
| [niqiuqiux/AMem](https://github.com/niqiuqiux/AMem) | 98 | C++ | 同作者的全功能版（ImGui + DX12 + Lua + CE 式扫描/指针链/断点窗口） | MiniMem 的前身/超集，GPL-2.0，94MB |
| [giaoimgiao/gameguardian-mcp-server](https://github.com/giaoimgiao/gameguardian-mcp-server) | 24 | Go | 单体跑在设备 root，MCP over HTTP `:8788`，ioctl 直连 MemoryDriver KPM | 功能对标的同类。45 个工具，`gg_*` 命名与我们 26 个重名 |
| [Z1og/ggmcp-kpatch-android16](https://github.com/Z1og/ggmcp-kpatch-android16) | 0 | Shell/Go/C | 上一项的 Android16 / 6.12 GKI 适配 + 自研 KPM(`kpgcmem`) | 内核 6.12 同代，`access_process_vm` 路线 + 22 条踩坑 |
| [ZYPyDoki/Gg_Docking_Kernel](https://github.com/ZYPyDoki/Gg_Docking_Kernel) | 324 | C++ | GG 修改器对接内核驱动（BSD-3） | GG 内核对接路线的上游，我们 KO 驱动思路的同源参考 |
| [miscusi-peek/cheatengine-mcp-bridge](https://github.com/miscusi-peek/cheatengine-mcp-bridge) | 1685 | Lua | MCP → CE Lua → CE 引擎 | 生态内最成熟（PC 平台），MCP 工具面设计的参考 |
| [PdB333/cortex](https://github.com/PdB333/cortex) | 31 | — | Windows 内存扫描/调试 + AI 友好 API | PC 侧 Agent 化扫描器，非 MCP |
| [qpalzm2332933171-droid/GGMCP-Control](https://github.com/qpalzm2332933171-droid/GGMCP-Control) | 1 | Go | ggmcp 的 Android 可视化客户端 | 生态客户端，可看别人怎么做 UI 联动 |

另测：「game memory mcp」「memory editing mcp」「process memory mcp」「KPM android memory」等 10 余组关键词，
除上表外无新增有效结果（`ZiggyBonham/game-memory-mcp`、`Acermax/windows-process-memory-mcp` 均为 0★ 个人项目，功能面小于我们）。

---

## 三、三项待评估项

### 1. 用 `access_process_vm()` 替换手写页表遍历 —— 不做

**现状**（代码事实）：读路径是 `phy_mem.h:50 get_proc_phy_addr`（手写 pgd→pud→pmd→pte）
+ `phy_mem_fast.h:161 fast_read_linear`（`virt_to_pfn` 守卫 → `virt_to_page` → `kmap_local_page` → memcpy）。
2026-10-23 已修掉 `memstart_addr` 少减导致的部分页静默丢弃（AGENTS.md 第 27 条，全量搜索 13681 → 20698）。

**性能基线**（AGENTS.md 第 30 条实测）：驱动内每页 `per_page_walk` 357ns + `per_page_phys` 559ns，
全量扫 341MB 理论 80ms；3.21s 的真实瓶颈是「absent 页逐页 syscall」，已用 `/proc/<pid>/pagemap`
的 present 位批量跳过，降到 0.15~0.22s。**驱动侧不是瓶颈，换什么 API 都省不到时间。**

**对方的适用场景**：Z1og 是从零搭 KPM、不想碰页表遍历，改用
`find_get_task_by_vpid` → `get_task_mm` → `access_process_vm`（内部自带 `mmap_read_lock` + 遍历），
实机 5/5 逐字节对上。它解决的问题是「从无到有」，不是「已有但慢」。

**代价**（推断，未实测）：`access_process_vm` 走 `get_user_pages_remote` 语义 + `copy_to_user`，
对 absent 页会进 fault 路径（可能建页、可被内核 hook 观测），批量扫时每页额外开销高于 kmap + memcpy。

**保留条件**：将来若 GKI 收紧导致页表遍历在某个新内核上失效，它是第二个 fallback。
接入点在 `read_ram_physical_addr` 之前按 `mm` 可用性分支。现在没有失败样本支撑，不写。

### 2. ioctl 通道伪装成 UDP socket fd —— 我们已是更优解

**现状**：`CONFIG_USE_ANON_CHANNEL` 已启用（`ver_control.h:47`）。agentd attach 时经 proc 通道发
`CMD_ANON_GET_FD(14)`，驱动 `fd_install()` 一个 `anon_inode:rwproc`，此后内存命令走 `ioctl(fd, 33, buf)`
（AGENTS.md 第 31 条，实测 fd 表显示 `6 -> anon_inode:rwproc`，耗时可 0.11~0.18s 与 read 路径持平）。

| 维度 | 我们（anon inode） | 对方（UDP socket fd） |
|---|---|---|
| 枚举痕迹 | 无路径可遍历；proc 节点仅作 bootstrap | 无 |
| `/proc/<pid>/fd` 显示 | `anon_inode:rwproc` ← **暴露模块名** | `socket:[12345]` ← 无语义 |
| 命令形态 | `ioctl(fd, 33, buf)` | `ioctl(fd, 0x7e1a/b, ...)` |
| 鉴权 | 编译期 `CONFIG_PROC_NODE_AUTH_KEY` | 无 |

对方唯一值得抄的是 **fd 名伪装**（`anon_inode:rwproc` 里带模块名，反作弊按 fd 名匹配即可命中）。
这一项已在 `docs/engineering/BACKLOG.md` 第 13 条，不重复立项。

### 3. 工具面差集 —— 见 §四

---

## 四、对方独有 19 项，逐项判定

| 工具 | 作用 | 判定 |
|---|---|---|
| `gg_bind_gg` / `gg_unbind_gg` / `gg_switch_gg` / `gg_list_gg_files` | 自主发现并绑定 GG 本体（特征目录 + 进程存活，随机包名也能找到），绑定信息持久化 | **不采纳**，见下注 |
| `gg_read_gg_saved` | 从 GG 进程内存里读「保存列表」（扫描连续两个 long = 地址+值 的 `qx={地址,值,类型,冻结,备注}` 条目并聚簇） | **不采纳**，见下注 |
| `gg_lua_write` / `gg_lua_read_out` | 生成 Lua 到 `/sdcard/ggmcp/`，`am broadcast --es mcp_script` 让 GG 引擎执行，`io.open` 写输出后回读 | **不采纳**，同上 |
| `gg_search_xor` | 加密值搜索：搜明文 → 统计 `K = value ^ plain` 的众数作为最可能密钥 → 只保留 K=众数的地址；变值后 `refine=true` 保留 K 不变者 | **采纳（P1）**。`agentd/search.c` 加一个候选过滤模式即可，复用现有结果集 |
| `gg_freeze_results` | 批量冻结当前结果集（可带 `minValue/maxValue/minAbsValue/excludeNaN`，`pairOnly` 只冻结相邻 8 字节成对的 X/Z 坐标） | **采纳（P0）**，与 BACKLOG 第 2 条「批量写入」合并做 |
| `gg_watch` | 软件轮询一组地址，记录变化次数/起止值/动态判定（判断某地址是否每帧变） | **采纳（P2）**。注意与我们 `gg_watch_*`（硬件断点）**语义完全不同且名字撞车**，落地时必须改名 |
| `gg_snapshot_results` / `gg_list_snapshots` / `gg_delete_snapshot` / `gg_cross_analyze` / `gg_analyze_results` | 结果集命名快照落盘，多个快照做交集/差集分析 | P2。我们是 `gg_search_undo`（逆操作），不是交叉分析 |
| `gg_export_results` | 结果导出 CSV | 不采纳，`gg_ui_command(op=export)` 已覆盖 |
| `gg_config_get` / `gg_config_set` | 运行时可改搜索上限/快照上限/冻结间隔并持久化 | P2，收益中等（现在是改 configs 重启） |
| `gg_server_stop` | 停止 MCP 服务 | 不采纳，我们用 `start_agentd.sh` + adb |

> **GG 绑定类 7 项已实测排除（2026-10-23 16:58，真机 `134d2f8`）**：
> - `pm list packages | grep -i gameguardian` → 空；`su -c 'ls /data/data/*/files/GG-*'` → 空；
>   `ps -A | grep -i gameguardian` → 空。设备上**没有任何 GG 本体**。
> - `agentd/main.c:355` 写明「GG 核心注入链路（lib5_stub + libKernelGg.so）已于 2026-10-21 整条删除 ——
>   该方案实测从未成功」，命令码 28/29 只留占位并返回错误。
> - 结论：这 7 个工具依赖 GG 本体进程与其保存列表，我们的链路里两个前置都不存在。**不采纳，不是优先级问题。**
>
> ⚠️ **同时发现一处文档与代码矛盾**：`AGENTS.md` 开篇仍写「GG 核心采用外挂圈子加载（ELF 桩 + hook syscall 270）」，
> 与 `agentd/main.c:355` 的「已整条删除」冲突。按现状以代码为准；AGENTS.md 那句需修正（未擅自改，待确认）。

> 另外：对方 45 个工具里与我们 **26 个重名**（`gg_attach` / `gg_search` / `gg_get_results` …）。
> MCP 客户端同时挂两套会工具名冲突，不能并行接入。

### 4.2 共有工具的参数差异（比新增工具更容易落地）

| 我们的缺口 | 对方做法 | 落点 | 判定 |
|---|---|---|---|
| `gg_search` 无 `refine` | `refine=true` 只在现有结果集里二次过滤（游戏内变值后调用，秒级收缩） | `server/session.cpp` 的 `tool_search` 加分支 | **采纳（P0）** |
| `region` 只有 `anonymous`/`writable`/`all` | 另有 `heap`/`stack`/`module` 与自定义 `rangeStart-rangeEnd` | `agentd/search.c` 区域筛选已按 maps 解析，加枚举即可 | **采纳（P1）** |
| `gg_set_results` 只能逐条 | 另有 `all:true` + `value` 一键全改 | 与 BACKLOG#2「批量写入」合并 | **采纳（P0）** |
| `gg_freeze` 只按 addr | 支持按搜索结果 `index` 冻结 | 与 `gg_freeze_results` 一起做 | 采纳（P1） |
| `gg_list_processes` 只有 `filter` | `includeKernel` / `appOnly` / `sort`(active/pid/adj/name) / `limit`+`offset` | `cmd_proc.c` | P2 |
| 无 `type=auto` | 一次扫 byte/word/dword/qword/float 全类型（多线程） | — | 不采纳：结果数放大数倍，AI 更难收敛，与我们「先窄后宽」的引导相反 |

---

## 五、我方独有 20 项（对方完全没有）

分析链能力，构成两边的真实差异：

| 类别 | 工具 |
|---|---|
| 硬件断点（CE 式「什么访问了这个地址」） | `gg_watch_set` / `gg_watch_poll` / `gg_watch_clear` / `gg_watch_inject` |
| 静态分析 | `gg_disassemble`（Capstone ARM64）、`gg_resolve_symbol`（ELF `.dynsym` 最近导出符号） |
| 结构化内存视图 | `gg_view_memory`（按 int32/float32/pointer 解析元素）、`gg_pointer_chain`（多级链 + 模块归属） |
| 跨会话记忆 | `gg_annotate_add/list/get/remove/context` |
| 内核虚拟触摸 | `gg_ghost_tap` / `gg_ghost_swipe` |
| 内核/设备状态 | `gg_kernel_status`、`gg_device_info` |
| 搜索可逆性 | `gg_search_undo`（模糊位图 3 级 + 精确地址数组 3 级） |
| UI 联动 | `gg_context`、`gg_ui_command` |

对方没有任何一项涉及反汇编、符号解析、硬件断点或跨会话持久化 —— 它的定位是「AI 版 GG 修改器」，
我们的是「AI 版内存分析工作台」。这决定了两边的工具面不该趋同。

---

## 六、不采纳及理由

- **不分层**：对方把 MCP 服务和 root 内存操作放同一个进程、同一个 HTTP 端口，无鉴权。
  我们的 PC/设备分离 + `AUTH` 握手 + pid/`starttime` 会话校验是安全边界，不为了「像它一样简单」而砍。
- **HTTP 传输**：MCP over HTTP 需要设备侧监听端口（`127.0.0.1:8788`），任何设备内 App 都能连。
  我们走 `adb forward` + stdio，端口不落设备。
- **无密钥 wire**：MemoryDriver 的 ioctl struct 首字段就是 pid，无认证。我们的 `AUTH` 门保留。
