# 驱动通道能力矩阵

> 最后核实：2026-10-21，真机 Redmi myron / 内核 6.12.69 / rwproc33 + hwBreakpointProc。
> 2026-10-21 已删除 hook / dev / qx 三型（见 §6），本文只描述 **rwproc33 单型**的现状。

## 1. 唯一驱动型：rwproc33

驱动型由启动参数决定（`KO_TYPE=rwproc` → `--driver rwproc`），**不是运行时自动探测**。

| 项 | 值 |
|---|---|
| 通道 | `/proc/<AUTH_KEY>/<AUTH_KEY>`，`read()` 走 33 字节头协议（非 ioctl） |
| 两个 LKM | `rwProcMem_module`（内存读写 + GG-TOUCH）/ `hwBreakpointProc_module`（硬件断点） |
| 节点由谁加载 | `install_ko.sh` 预加载；agentd **不做 insmod**（模块 `hide_by_default`，重复 insmod 会 EEXIST） |
| 通道失败时 | **直接报错，不降级**到用户态通道（避免语义不一致 + 避免可被反作弊检测的越界读） |

因为没有降级链，rwproc 出问题就是真出问题，不会"悄悄变慢"。

## 2. 状态值

定义在 `shared/wire_protocol.h: enum gg_engine_state`，判定在 `driver_self_test()`。

| state | 常量 | `state_str` | 判定依据 |
|---|---|---|---|
| 1 | `ST_DRIVER` | `kernel-driver (rwproc33)` | `rwproc_open_process(self)` + `rwproc_read_mem` 回环成功 |
| 4 | `ST_UNAVAILABLE` | `unavailable` | 通道不可用 |

> 2 / 3 / 5 是已删除的降级态（`ST_PROC_VM` / `ST_PROC_MEM` / `ST_KO_HOOK`），
> 枚举里保留数字但不产生。

**自检的局限**（如实记录）：`driver_self_test` 读的是**自己**进程的内存
（`getpid()` + 静态变量回环），这只能证明「`process_vm_readv` / rwproc 节点可用」，
不能证明别的进程也能读。判断真实可用性要看 `attach` + 实际读写。

## 3. 工具 × 依赖

### 3.1 需要驱动通道（读写类）

`gg_read` / `gg_read_bytes` / `gg_hex_dump` / `gg_view_memory` /
`gg_write` / `gg_set_results` /
`gg_search` / `gg_search_unknown_init` / `gg_search_refine` / `gg_search_range` /
`gg_freeze` / `gg_unfreeze` / `gg_pointer_scan` / `gg_pointer_chain`

### 3.2 需要会话（先 `gg_attach`）

`gg_target_info` / `gg_module_list` / `gg_module_base` / `gg_calc_offset` /
`gg_resolve_symbol` / `gg_disassemble` / `gg_pause` / `gg_resume` /
`gg_watch_set` / `gg_watch_poll` / `gg_watch_inject` / `gg_watch_clear` /
`gg_result_count` / `gg_get_results` / `gg_clear_results`

### 3.3 不依赖会话

`gg_kernel_status` / `gg_device_info` / `gg_status` /
`gg_list_processes` / `gg_attach` / `gg_detach` / `gg_shell`

### 3.4 纯用户态（PC 本地）

`gg_annotate_add` / `gg_annotate_list` / `gg_annotate_get` /
`gg_annotate_remove` / `gg_annotate_context`

### 3.5 触控（走 KO 的 `CMD_GGTOUCH_*`）

`gg_ghost_tap` / `gg_ghost_swipe` —— 会真实点击屏幕。

## 4. GG 核心注入链路 —— 已删除

`native/hook/hook.c`（`libKernelGg.so`）与 `native/stub/lib5_stub.c` 已于 2026-10-21 整条删除，
`loader.c` / `loader.h`、`gg_kernel_load_gg` / `gg_core_versions` 工具、对应单测一并移除。

删除理由（三条，任一条都足够）：

1. **从未成功**：GG 守护链自检报 `android-daemon err`（`ACCEPTANCE.md` 记录）。
2. **唯一会改写用户文件的动作**：`loader_load` 备份并覆盖 GG 的 `lib5.so`，
   中途失败可能让用户的核心无法还原。
3. **与 rwproc33 协议不兼容**：它只认 `qx` / `dev` / 直通三种 proto，不支持 rwproc33
   的 33 字节头协议。就算跑通，GG 核心也只能走原生 `process_vm_readv`，
   拿不到内核通道。

现状：agentd 自带引擎直接经 rwproc33 读写，**不需要 GG 本体**。
wire 命令码 28/29 保留占位（见 `PROTOCOL.md`），旧客户端调用得到明确错误。

## 4.5 硬件断点不可用（2026-10-21 实测，会崩内核）

`hwBreakpointProc_6.12.69.ko`（afei prebuilt）**不能用**：`rwproc_hwbp_start` 会让设备重启。

**现象**：itest 跑到 `rwproc_hwbp_start` 时设备掉线（adb `device not found`），
恢复后 `uptime` 归零、`sys.boot.reason=reboot`、`pstore` 为空（无 ramoops）。
连该函数内的第一行 `printf` 都没输出 —— 崩在 OPEN_PROCESS 或 INST 内部。

**两条独立证据（静态）**：

| 证据 | 说明 |
|---|---|
| 重定位表含 `put_pid` / `find_get_pid` | OPEN_PROCESS 用 `find_get_pid` 取 `struct pid*`，CLOSE 用 `put_pid` 释放。若模块命令码 **0 起始**而我们按 1 起始发，`cmd=1`（以为 OPEN）会落到 CLOSE_PROCESS → `put_pid((struct pid*)1)` 解引用野指针 → 内核崩。 |
| 只有 `mutex_lock`，**没有** `raw_spinlock` | `RWPROC33_ADAPTER.md` §10.4 记录的修复版是「固定槽位数组 + raw_spinlock」，因为命中回调运行在 ARM64 调试异常上下文（中断/抢占关闭），用 mutex 会 `scheduling while atomic`。这份 prebuilt 用的是 mutex → 是**修复前**版本。 |

即使把命令码改对让 INST 通过，命中回调仍会在原子上下文里睡眠而崩。所以**不要试图
通过改 `RWBP_CMD_*` 去凑**。

**当前处理**：`g_hwbp_allowed` 默认 0，`gg_watch_set` 直接返回 `GGERR_DEGRADED`
并说明原因；要启用需 `agentd --hwbp`（仅在换用自编译模块后）。四个 `gg_watch_*`
因此处于「保留但默认不可用」状态。

**要恢复硬件断点**：按 `RWPROC33_ADAPTER.md` §10 在 VM 里用 6.12 源码树重编
`hwBreakpointProc_module`（枚举改 1 起始 + 回调改 raw_spinlock），再用 §11 的两步
（vermagic + CRC）适配到本机。

## 5. 本机实测（2026-10-21）

| 项 | 结果 |
|---|---|
| 模块加载 | `apr_dlkm`(rwProcMem) / `hwBreakpointProc` 均 RC=0 |
| 节点 | `/proc/<MEM_KEY>/<MEM_KEY>`、`/proc/<BP_KEY>/<BP_KEY>` 就绪 |
| `gg_kernel_status` | `kernel-driver (rwproc33)`，`error` 空 |
| `attach(pid 1)` | `driver_state=1` |
| `read_bytes(当前 init 基址)` | `7f454c46…`（ELF 头） |
| `run_itest.ps1` | ITEST ALL PASS |
| `run_tests.bat` | 534 条断言全绿（Windows 518 + WSL 16） |
| **`gg_search` 全 anonymous** | ⚠️ **崩内核**（见 §5.1） |

### 5.1 已知故障：全量搜索会让设备重启

对某些目标进程执行 `gg_search(region=anonymous)` → agentd 失联 → **设备重启**。
无 tombstone / logcat / pstore（该机未配 ramoops）。

已排除：`is_force`（代码是 0）、用户态越界、句柄泄漏、单次读长度、数据量、
区域数量（pid 1 全扫正常）。**定位：与"读哪个进程"有关，与"读多少"无关。**

根因在 KO 的 ioctl 实现（第三方二进制），不在 agentd。
详见 `RWPROC33_ADAPTER.md §十二`。

**使用建议**：搜索用 `gg_search_range` 限定到确定已映射的块；
不要对真实游戏进程直接全内存搜。

## 6. 已删除的三型（2026-10-21）

删了 hook / dev / qx，理由：

1. **从未在真机执行过** —— 设备上只有 rwproc 的 KO，另外三型没有对应模块；
2. **hook 型的自检是假阳性** —— 用 `process_vm_readv` 读自己必然成功，
   导致 `state=5 / "ko-hook (kernel hooks process_vm_readv)"` 在没有任何 hook 的
   设备上也会出现，误导使用者以为有内核 hook 保护；
3. **四型让状态机复杂四倍**，而只有一个型是真跑过的。

`driver.c` 从 1136 行 → 541 行。需要回退时从提交 `96850ec` 的前一个恢复。

## 7. 待办

1. §4：`libKernelGg.so` 增加 rwproc 协议支持（否则 GG 核心一直走原生 syscall）。
2. §5.1：rwproc 崩溃需 KO 侧定位（逐区域二分 + 对照 KO 源码）。
3. `gg_disassemble` 对由 ELF 头起始的地址返回 `count=0`，待确认是 capstone 行为还是缺陷。
