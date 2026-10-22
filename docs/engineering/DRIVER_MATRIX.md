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

## 4.5 硬件断点（2026-10-21 打通，自编译模块 + 运行时探测）

**结论：可用**。afei 的 prebuilt 会崩内核，自编译模块（`scripts/build_hwbp_ko.sh`）
加载后 `gg_watch_*` 全链路正常，`deploy.ps1` 会自动用它覆盖 prebuilt。

### 4.5.1 崩内核的原始现象

用 afei 的 `hwBreakpointProc_6.12.69.ko` 时，`rwproc_hwbp_start` 直接让设备重启：
adb `device not found` → 恢复后 `uptime` 归零、`sys.boot.reason=reboot`、`pstore` 空
（无 ramoops）。连该函数内第一行 `printf` 都没输出 —— 崩在 OPEN_PROCESS / INST 内部。

### 4.5.2 四条缺陷（都是 afei prebuilt 里的，源码逐一对应）

源码在 `build/rwProcMem33/hwBreakpointProcModule/hwBreakpointProc_module/`
（`hwbp_proc.c` 是死代码，Makefile 只编主 `.c`）。

| # | 位置 | 缺陷 | 后果 |
|---|---|---|---|
| 1 | `hwBreakpointProc_module.h` 的 `enum` | 首项 `CMD_OPEN_PROCESS` **无赋值 → 从 0 起**，而 `rwProcMem_module.h` 是 `CMD_INIT_DEVICE_INFO = 1`；agentd 的 `RWBP_CMD_*` 按 1 起始发 | `cmd=1`（以为 OPEN）落到 `CLOSE_PROCESS` → `put_pid((struct pid*)1)` 解引用野指针 → **整机重启** |
| 2 | `hwbp_handler` | perf_event 溢出回调（ARM64 调试异常，中断/抢占关闭）里 `mutex_lock` + `cvector_pushback`（内部 vmalloc） | `scheduling while atomic`。修好①之后命中那一刻照样崩 |
| 3 | `OnCmdGetHwbpHitDetail` | 返回**条目数**，而 `driver.c` 按 `rsize / sizeof(item)` 折算 | 条数 < 312 时恒为 0 → **永远取不到命中** |
| 4 | `hwbp_handler` 的执行断点分支 | 命中后只 `toggle_bp_registers_directly(enable=0)`，但 perf 仍认为 event 有效、会把断点写回硬件 | PC 停在断点指令上**无限重复触发**：进程卡在 R 状态、CPU 满载，不崩机但也回不来 |

作者自己在 `AFEI_CHANGES.md` 第 3 条写过枚举基准差异（"rwProcMem 首项显式 `=1` ⇒
UNHIDE=15；hwBreakpointProc 首项从 0 起 ⇒ =12"），但只用来算自己的命令号，没对齐。

### 4.5.3 修复

模块源码的三处（`scripts/patches/hwbp-fix.patch`）：

- 枚举改 `CMD_OPEN_PROCESS = 1`，`UNHIDE` 挪到 14，13 让给批量注册位
- 句柄表 `cvector` → 固定 16 槽数组（UNINST 用 swap-remove）；命中缓冲改 INST 时
  一次性 `vmalloc` 的固定数组，回调内只 `memcpy`
- `struct mutex` → `raw_spinlock_t`；`COUNT`/`DETAIL` 的 `copy_to_user` 挪到锁外；
  `DETAIL` 返回字节数

第 4 条（死循环）不能用 `CONFIG_MODIFY_HIT_NEXT_MODE` 解：它的
`arm64_move_bp_to_next_instruction` 走 perf 的 `modify_user_hw_breakpoint`，
会拿 mutex，在调试异常上下文里必然 `scheduling while atomic`。改为回调内**直接写
BVR** 实现两段式挪移：首次命中记录后把断点挪到 `pc+4` 让原指令执行，在 `pc+4`
再命中时挪回原地址（这一次不记录）。数据断点不需要挪位，只记录。

`CONFIG_MODIFY_HIT_NEXT_MODE` / `CONFIG_ANTI_PTRACE_DETECTION_MODE` 保持关闭；
`SUSPEND/RESUME` 改为句柄 `suspended` 标志。

### 4.5.4 `struct module` 尺寸：必须用完整 GKI 树

| 树 | this_module | 能否加载 |
|---|---|---|
| vanilla 6.12 + 设备 config | 1280 (0x500) | ❌ |
| vanilla + 换成 GKI 的 `module.h`/`android_kabi.h` | 1344 (0x540) | ❌ |
| **完整 GKI (`android16-6.12`) + 设备真值 config** | **1600 (0x640)** | ✅ |

设备真值 1600 是从能正常加载的 `rwProcMem_6.12.69.ko` 的
`.gnu.linkonce.this_module` 段读出的，也与设备 BTF（`/sys/kernel/btf/vmlinux`）
里 `struct module` 的 size 一致 —— 两条独立来源互相印证。

差的那 256 字节不是"换个头文件"能补的：GKI 在末尾加了 `scmversion`、无条件
`sig_ok`、4×`ANDROID_KABI_RESERVE`，而这些字段受 GKI 特有 CONFIG 影响
（`dyndbg_info`、`kunit_*` 等），只有完整 GKI 树让 `olddefconfig` 认全设备 config 才对得上。

vermagic 用 `SUBLEVEL=69` + `CONFIG_LOCALVERSION="-android16-6-4k"` 直接产出，
不做二进制改串。设备 `uname -r` 带 git hash 与 ABO 编号
（`…-g43b5ab7e247d-abogki561813157-4k`），但内核的 vermagic 比较放宽到该后缀 ——
afei 那份能正常加载的模块 vermagic 就是 `6.12.69-android16-6-4k`，与本构建一致。

### 4.5.5 版本表为空不影响加载

内核树没编过 `vmlinux` 就没有 `Module.symvers`，模块引用的内核符号全是 undefined，
`__versions` / `__version_ext_crcs` 为空。实测**仍能 insmod 并正常工作** ——
内核按名字解析未定义符号，版本表为空时只警告不拒绝。

顺带修掉上游 modpost 的空表缺陷：`__version_ext_names` 为空列表时会生成
`static const char x[] = ;`（C 语法错误）。`build_hwbp_ko.sh` 自动打这个兜底补丁。

要拿到真正的 CRC 表则必须完整编译内核树（且需与设备 6.12.69 精确对应的源码），
目前不需要。

### 4.5.6 命令码基准的运行时探测

`agentd` 不再无条件拒绝 `gg_watch_set`，而是首次调用时探测一次：

```
cmd=3 (GET_NUM_BRPS) → 自编译模块返回 6（执行断点槽数）
                     → afei 的 0 起始模块返回 4（那条实际是 GET_NUM_WRPS）
```

只有探到 6 才放行；否则返回 `GGERR_DEGRADED` 并说明原因。**绝不探测 `cmd=4`** ——
0 起始枚举下那是 `INST_PROCESS_HWBP`，参数全零属高危路径。
`agentd --hwbp` 可跳过探测（自担风险）。

### 4.5.7 部署与验证（2026-10-21 实测）

- `deploy.ps1` 用 `build/ko_built/hwbp_selfbuilt_6.12.69.ko` 覆盖
  `build/ko/hwBreakpointProc_6.12.69.ko` 后再推送 —— 两者模块名与 vermagic 相同，
  `install_ko.sh` 的内核版本匹配逻辑不受影响
- 全链路实测（pid 16310 的写监视点，`type=2 len=4`）：
  `watch_set` → `bps=1`；3 秒后 `watch_poll` → `hits=64 list=64`，
  每条带 `pc`、模块归属（`probe_loop + 0x3e5f0`）与 pc±16 反汇编；`watch_clear` → `active=false`
- 自命中断点验证（对自身函数设 X 断点后调用）：首次 `total=1`，再次调用 `total=2`
  —— 说明单步挪位正确、断点不会失效也不会死循环
- `run_itest.ps1` → **ITEST ALL PASS**，含 `hwbp INST / GET_HIT_COUNT / UNINST` 三项
  （itest 先探测，探不到 6 就跳过，不会把设备打崩）

### 4.5.8 一个必须知道的坑：隐藏模块会允许重复加载同名模块

`CONFIG_HIDE_BY_DEFAULT` 会把模块从内核链表里摘除（`list_del_init`），于是：

- `rmmod` 永久失败 —— 换模块只能**重启设备**
- `insmod` 同名模块**不会被拒**，后加载的会 `remove_proc_subtree` 顶掉前一个的
  `/proc/<KEY>` 节点 —— 表现为"明明部署了新版，`cmd=3` 却返回 4"

排查手法：`cmd_probe` 发 `cmd=3` 看返回 6 还是 4。

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
2. ~~§5.1：rwproc 崩溃需 KO 侧定位~~ —— 2026-10-23 定位完成，不在页表遍历
   （dbg 版日志显示 paddr 算得都对），而是 KO 版本选错 + 旧模块并存抢 proc 节点，
   见 §8。
3. `gg_disassemble` 对由 ELF 头起始的地址返回 `count=0`，待确认是 capstone 行为还是缺陷。

## 8. KO 版本谱系（2026-10-23 实测）

`build/ko/_dev/` 下四个自编译 KO 只有一个可用，**别按文件时间戳选**：

| 文件 | 大小 | 读写 | 全量搜索（systemui 341MB） | 结论 |
|---|---|---|---|---|
| `rwNova33_6.12.69.ko` | 307096 | ITEST ALL PASS | 3.3s / 13681 条 / 不崩 | **现役**（模块名 rwNova33，2026-10-23 改名后从当前源码重编） |
| `rwproc_v2.ko` | 307640 | ITEST ALL PASS | 3.6s / 21892 条 / 不崩 | 改名前的等价物，保留备查 |
| `rwProcMem_gki.ko` | 307584 | read 全 0 | 正常 | `virt_addr_valid` 误判合法 pfn，弃用 |
| `rwproc_dbg.ko` | 330712 | — | — | 唯一带 `CONFIG_DEBUG_PRINTK`，抓页表日志用 |
| `prebuilt/rwProcMem_slim.ko` | 39552 | — | — | vermagic 不匹配，insmod 报 `module_layout` 版本不符 |

另有历史遗留：设备上曾长期并存 `apr_dlkm.ko`（rwProcMem 的原始编译名，
`build_rwproc_ko.sh` 的产物就叫这个）与 `rwProcMem_<ver>.ko`。两者模块名不同
（`apr_dlkm` / `rwProcMem_module`）所以都能 insmod，但 proc 节点只有一个 ——
先加载的占 `/proc/<key>/<key>`，后加载的 `proc_mkdir` 失败 fallback 到 /proc 根。
**agentd 永远打开先加载那个**，导致「换 KO 不生效」的假象。
根因（deploy staging 从不清理）与修法见 `AGENTS.md` 第 33 条。
