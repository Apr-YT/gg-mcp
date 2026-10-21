# 驱动通道能力矩阵（全量梳理）

> 最后核实：2026-10-21，真机 Redmi myron / 内核 6.12.69 / rwproc33 + hwBreakpointProc。
> 本文只描述**当前代码的实际行为**，不描述设计愿望；凡与注释/文档冲突处以本文为准并注明。

## 1. 四种驱动型

驱动型由启动参数决定（`start_agentd.sh` 的 `KO_TYPE` → `--driver`），**不是运行时自动探测**。

| ko_type | `--driver` | 实际通道 | 通道失败时 |
|---|---|---|---|
| `KO_TYPE_HOOK` (1) | `hook` | `process_vm_readv` → `/proc/pid/mem` | **降级**到下一条 |
| `KO_TYPE_DEV` (2) | `dev` | `/dev/<node>` + ioctl | **不降级**，直接报错 |
| `KO_TYPE_QX` (3) | `qx` | Aloha QX 节点 ioctl | **不降级**，直接报错 |
| `KO_TYPE_RWPROC` (4) | `rwproc` | `/proc/<KEY>/<KEY>`，33B 头协议 | **不降级**，直接报错 |

设计意图（`driver_read` 注释原文）：专用 KO 型 ioctl 失败就报错，**不回落用户态**
—— 避免语义不一致，也避免可被反作弊检测的越界读取。

这解释了一个容易误会的现象：rwproc 模式下 `gg_search` 崩了就真的崩了，
不会"悄悄退化成慢慢读"；而 hook 模式看着"稳"，是因为它从一开始就走原生 syscall。

## 2. 状态值对照

定义在 `shared/wire_protocol.h: enum gg_engine_state`，判定在 `driver_self_test()`。

| state | 常量 | `state_str` | 判定依据 |
|---|---|---|---|
| 1 | `ST_DRIVER` | kernel-driver (kernel module) | dev/qx/rwproc 的 ioctl/节点读回环成功 |
| 2 | `ST_PROC_VM` | process_vm_readv (degraded) | hook 型：`vm_read(self)` 成功 |
| 3 | `ST_PROC_MEM` | /proc/pid/mem (degraded) | hook 型：`mem_io(self)` 成功 |
| 4 | `ST_UNAVAILABLE` | unavailable | 全部失败 |
| 5 | `ST_KO_HOOK` | ko-hook (kernel hooks process_vm_readv) | hook 型：`vm_read(self)` 成功 ← ⚠️ 见 §3 |

## 3. ⚠️ `ST_KO_HOOK` 是假阳性

```c
/* driver_self_test() 里 hook 型的分支 */
static volatile uint32_t s_magic = 0x12345678u;
pid_t self = (pid_t)getpid();
ssize_t rv = vm_read(self, &back, sizeof(back), (uint64_t)(uintptr_t)&s_magic);
if (rv == (ssize_t)sizeof(back) && back == 0x12345678u) {
    d->state = ST_KO_HOOK;   /* ← 就凭这个判定成 ko-hook */
```

`self` 是 `getpid()` —— 它读的是**自己的**内存。读自己永远成功（原生 `process_vm_readv`
就够），**不需要任何 hook**。

**本机实测**：设备上只有 `rwProcMem` 与 `hwBreakpointProc` 两个 **ioctl 型**模块
（dmesg 只有这两条 init 记录），**没有任何 hook 型 KO**，但 `gg_kernel_status`
仍然返回 `state=5 / "ko-hook (kernel hooks process_vm_readv)"`。

推导出的结论：

1. `ST_KO_HOOK` 只证明「`process_vm_readv` 通道可用」，**不能证明内核 hook 了 syscall**。
2. hook 模式下能读别的进程，靠的是 **agentd 以 root 运行、有 `CAP_SYS_PTRACE`**，
   不是靠内核 hook。
3. 因此「`state=ko-hook` 表示 hook 型 KO 已生效」这一说法**不成立**，
   凡是据此判断安全性的逻辑都不可信。

**为什么修不干净**：用户态无法区分下面两种情况，结果完全一样。

| 场景 | 读自己 | 读别人 |
|---|---|---|
| 有 hook | 成功 | 成功（KO 接管） |
| 无 hook + root | 成功 | 成功（原生权限） |

可选处理：
- **A（建议）**：`state_str` 改成语义诚实的表述，如 `process_vm_readv (channel ok)`，
  不再断言 "kernel hooks"；`itest` 里对应期望值同步改。
- B：自检改读非 self 的目标，能区分「能否读别人」，但会把「权限不足」与「无 hook」混在一起。

## 4. 工具 × 驱动型 依赖

### 4.1 需要驱动通道（读写类）

这些工具最终都落到 `driver_read` / `driver_write`，因此继承 §1 的降级行为：

| 工具 | 说明 |
|---|---|
| `gg_read` / `gg_read_bytes` / `gg_hex_dump` / `gg_view_memory` | 读内存 |
| `gg_write` / `gg_set_results` | 写内存 |
| `gg_search` / `gg_search_unknown_init` / `gg_search_refine` / `gg_search_range` | 搜索（内部大量读） |
| `gg_freeze` / `gg_unfreeze` | 冻结线程周期写 |
| `gg_pointer_scan` / `gg_pointer_chain` | 指针扫描（链式读） |

> `gg_search*` 是**唯一在真机上把设备搞崩的**（rwproc + 特定目标进程，见 §5）。

### 4.2 需要会话（先 `gg_attach`）

`gg_target_info` / `gg_module_list` / `gg_module_base` / `gg_calc_offset` /
`gg_resolve_symbol` / `gg_disassemble` / `gg_pause` / `gg_resume` /
`gg_watch_set` / `gg_watch_poll` / `gg_watch_inject` / `gg_watch_clear` /
`gg_result_count` / `gg_get_results` / `gg_clear_results`

### 4.3 不依赖会话

`gg_kernel_status` / `gg_device_info` / `gg_core_versions` / `gg_status` /
`gg_list_processes` / `gg_attach` / `gg_detach` / `gg_kernel_load_gg` / `gg_shell`

### 4.4 纯用户态（PC 本地，不碰设备）

`gg_annotate_add` / `gg_annotate_list` / `gg_annotate_get` /
`gg_annotate_remove` / `gg_annotate_context`

### 4.5 触控（需 rwproc33 型）

`gg_ghost_tap` / `gg_ghost_swipe` —— 走 KO 的 `CMD_GGTOUCH_*`，**只 rwproc 支持**，
会真实点击屏幕。

## 5. 本机实测结论（2026-10-21）

| 驱动型 | state | 读写 | 搜索 | 备注 |
|---|---|---|---|---|
| `rwproc` | 1 (`kernel-driver`) | ✅ | ⚠️ **崩内核** | 全 anonymous 扫 sleep 进程 → 设备重启；pid 1 全扫正常；4KB/1MB 范围正常。详见 `RWPROC33_ADAPTER.md §十二` |
| `hook` | 5 (`ko-hook`) | ✅ | ✅ 正常 | 实际走原生 `process_vm_readv`（root），无内核 hook |

### 使用建议

- **搜索**：用 `hook` 型，或用 `gg_search_range` 把范围限定到确定已映射的块；
  不要用 rwproc 对真实游戏进程全内存搜 —— 真实进程内存布局比 init 复杂得多。
- **读写**：rwproc 正常（4KB / 1MB 实测通过）。
- **判断"驱动好不好"**：不要只看 `state`，rwproc 型应看 `state=1`，
  hook 型不能用 `state=5` 证明有 hook。

## 6. 待办

1. §3 的 `state_str` 语义修正（等确认文案）。
2. §5 的 rwproc 崩溃需 KO 侧定位：用 `gg_search_range` 逐区域二分，
   找出崩在哪类 VMA，再对照 KO 源码看 `CMD_READ_PROCESS_MEMORY`。
3. `gg_disassemble` 对由 ELF 头起始的地址返回 `count=0`，待单独确认是 capstone 行为还是缺陷。
