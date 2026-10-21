# hwBreakpointProc_module 静默挂死问题排查（寻求外援）

> ✅ **已解决（2026-08-17）**：根因是**命令枚举错位**（模块枚举从 0 开始 vs 用户态按 1 起始 → 全部命令错位 +1）。
> `gg_watch_set` 发 cmd=1（以为 OPEN）实际触发 CMD_CLOSE_PROCESS → `put_pid((struct pid*)pid)` 解引用用户态整数 → 内核数据异常 → 整机静默挂死/看门狗重启。
> 修复：枚举显式 `=1` 起始；真机 attach/set/poll(64 命中含反汇编)/clear 全流程通过。
> 本文档保留作为排查记录，供同类问题参考。

> 状态：2026-08-17 凌晨，设备端真机复现，已做多轮二分定位，卡在"最简单命令都挂死且零内核日志"。
> 设备：Redmi myron（K90 Pro Max），内核 `6.12.69-android16-6-gb1493ec68d4a-abogki514973465-4k`，KernelSU root。

---

## 一、现象（一句话）

**hwBreakpointProc_module.ko（硬件断点 proc 节点模块）加载正常，但向它的 proc 节点发送"任何带命令码的 read()"（哪怕是最简单的 CMD_OPEN_PROCESS=1），设备立即静默挂死，dmesg 无任何输出，约 40~70 秒后看门狗重启。**

对照：同仓库的 rwProcMem_module.ko（内存读写模块，同样的 33 字节 packed 头 + proc read 协议 + 同样的 `__arch_copy_from_user` 用法）**完全正常**，attach/读/写/搜索全链路可用。

## 二、最小复现（独立于 agentd/MCP，直接操作节点）

```c
// bptest mode 1（用户态，root）
int fd = open("/proc/dce3771681d4c7a143d5d06b7d32548e/dce3771681d4c7a143d5d06b7d32548e", O_RDWR);
uint8_t buf[512] = {0};
struct req { char cmd; uint64_t p1,p2,p3,bs; } __attribute__((packed)) *r = (void*)buf;
r->cmd = 1;                 // CMD_OPEN_PROCESS
r->p1 = getpid();
read(fd, buf, 33);          // ← 设备挂死（无任何输出，~2 秒内 adb 掉线，~40s 看门狗重启）
```

- `mode 0`（同一二进制，全零头 → cmd=0 → 模块 switch default → 返回 -EINVAL）：**正常**，设备存活，read 返回 -1。
- `mode 1`（cmd=1 → `OnCmdOpenProcess`）：**必挂**。
- 通过 agentd/MCP 的 `gg_watch_set`（内部就是先 OPEN 后 INST）同样必挂。

## 三、已排查/已排除（重要）

| # | 假设 | 结论 |
|---|---|---|
| 1 | 命中回调（perf overflow handler）原子上下文违规（mutex/vmalloc/modify_user_hw_breakpoint） | ❌ 排除：连 CMD_OPEN_PROCESS 都崩，根本没走到装断点/命中 |
| 2 | proc_ops 函数指针的 KCFI 检查 | ❌ 排除：裸 read（mode 0）走同一条 proc_read 间接调用，正常返回 |
| 3 | `__arch_copy_from_user`（无 access_ok 的裸拷贝） | ❌ 排除：mode 0 同样调用它，正常；且崩溃点看起来在更深处 |
| 4 | pstore 崩溃日志 | ❌ 无法用：设备内核 `CONFIG_PSTORE_RAM` 未开（只有 CONFIG_PSTORE=y + CONFIG_PSTORE_CONSOLE=y），`/sys/fs/pstore` 为空 |
| 5 | 内核 oops→panic 消息 | ❌ 未出现：dmesg 全程零输出。且 `/proc/sys/kernel/panic = -1`（panic 不自动重启）→ 观察到的重启是 **MSM 看门狗**，即"挂死"而非"oops/panic" |
| 6 | 模块加载/CRC/vermagic | ✅ 正常：insmod OK，init 打印正常，kallsyms 解析的 register_user_hw_breakpoint 地址有效；MODVERSIONS CRC 已用设备厂商模块批量修补（10 改写 0 missing） |

## 四、最关键的矛盾点（请外援重点看这里）

模块内已在关键路径加了 **KERN_EMERG printk + mdelay(500)** 埋点（崩溃前有 ~2 秒窗口）：

- `hwBreakpointProc_read` 入口（`copy_from_user` 之前，与 cmd 无关）
- `OnCmdOpenProcess` 各步骤（entry / find_get_pid / copy_to_user / done）
- `OnCmdInstProcessHwbp` 各步骤

**结果：mode 1 挂死时，以上 printk 一个都没有出现在 dmesg 里**（崩溃前用 `adb shell dmesg | tail` 每 250ms 轮询，也确认了 dmesg 里没有 `hwbp:` 前缀的新行）。

即：**read() 进入内核 → proc 层 → 模块 hwBreakpointProc_read 的第一个 printk 之前，系统就静默挂死了**。但 mode 0 走完全相同的调用链（proc_reg_read → 模块 read → copy_from_user → dispatch）能正常返回。

唯一的输入差异：33 字节头里 `cmd` 字节 = 1（而非 0）。这导致 switch 走 `case CMD_OPEN_PROCESS` 而非 `default`。

## 五、环境与构建参数

- 内核：GKI android16-6.12 + SUBLEVEL=69，`CONFIG_LOCALVERSION="-android16-6-4k"`（设备 uname 显示更长串，但 `6.12.69-android16-6-4k` 短 vermagic 的模块能加载，rwProcMem 已证明）
- 关键内核配置（`/proc/config.gz`）：`CONFIG_CFI_CLANG=y`、`CONFIG_CFI_ICALL_NORMALIZE_INTEGERS=y`、`# CONFIG_CFI_PERMISSIVE is not set`（CFI 失败=硬 panic）、`CONFIG_MODVERSIONS=y`、`CONFIG_STRICT_MODULE_RWX=y`、`CONFIG_PANIC_ON_OOPS=y`、`CONFIG_ARM64_PAN=y`、`CONFIG_ARM64_BTI=y`、`CONFIG_ARM64_MTE=y`
- 模块编译：Ubuntu clang 18.1.3，`-fsanitize=kcfi -fsanitize-cfi-icall-experimental-normalize-integers -mgeneral-regs-only -Wa,-march=armv8.5-a -O2 -fno-PIE`（与内核同参数）
- 模块动态解析符号（CONFIG_KALLSYMS_LOOKUP_NAME，kprobe 取 kallsyms_lookup_name）→ `register_user_hw_breakpoint` / `unregister_hw_breakpoint` 地址有效
- 原上游：https://github.com/abcz316/rwProcMem33（hwBreakpointProcModule，作者适配到 6.1.75）

## 六、外援可参考的源码位置

- 模块源码（已加埋点）：`build/rwProcMem33/hwBreakpointProcModule/hwBreakpointProc_module/hwBreakpointProc_module.c`
- 关键函数：`hwBreakpointProc_read`（33 字节头拷贝 + dispatch）、`OnCmdOpenProcess`（find_get_pid + copy_to_user）、`DispatchCommand`（switch）
- 内核对口源码：`build/gki-6.12/fs/proc/inode.c`（proc_reg_read）、`arch/arm64/kernel/hw_breakpoint.c`
- 用户态最小复现：`tests/bptest.c`

## 六.5 最新补充排查（2026-08-17 深夜，已用反汇编/配置对比排除）

1. **BTI/PAC 配置核对**：模块编译参数 `-mbranch-protection=pac`（应为 pac-ret，属正则匹配截断）；VM 构建 `out/.config` 与设备 `/proc/config.gz` 的
   `CONFIG_ARM64_BTI=y` / `CONFIG_ARM64_PTR_AUTH_KERNEL=y` / `# CONFIG_ARM64_BTI_KERNEL is not set` **完全一致** →
   内核自身不开 BTI，模块也不强制 BTI 落地垫 → **排除 BTI 违规**（且 mode 0 的 proc_read 间接调用若受 BTI 约束也会崩）。
2. **反汇编 .ko**（clang 18, -O2）：
   - `DispatchCommand`/`OnCmdOpenProcess` 等 static 函数**全部内联进 `hwBreakpointProc_read`**（单函数 0x195c 字节）。
   - 12 路命令 switch 被编译成**决策树（级联条件分支），没有跳转表**（全 .ko 仅 `read_wb_reg` 有两处跳转表）→ 排除"跳转表条目/间接分支"问题。
   - 重定位检查：`find_get_pid`/`_printk`/`__arch_copy_from_user`/`__arch_copy_to_user`/`_raw_spin_lock_irqsave` 等 CALL26/ADR_PREL 重定位**全部正常**，目标符号正确。
3. **printk 排除**：原版模块（`CONFIG_DEBUG_PRINTK` 关闭，printk_debug=空函数）同样崩溃 → 与 printk 是否输出无关。
4. **协议/布局排除**：同一份 packed 33 字节头 + 相同 `__arch_copy_from_user` 用法在 rwProcMem_module（内存模块）上完全正常（agentd attach/读/写/搜索全链路 OK）→ 崩溃与协议封装、用户态结构布局无关，是 hwbp 模块 OPEN 处理路径特有的。

## 七、悬而未决的问题（求方向）

1. 什么会导致 proc 节点 `read()` 在**模块函数体第一条指令（printk）之前**、且 **dmesg 零输出**地挂死整机？——注意 CPU 卡死（看门狗重启）而非 oops。
2. 是否可能 **read() 其实没进到模块代码**（崩溃在 proc_fs 层/`proc_reg_read`）？——但 mode 0（cmd=0）走同一调用链正常返回，唯一输入差异是 33 字节头里 `cmd=1`。
   ⚠️ 此点可用"READ 入口埋点"（`hwBreakpointProc_read` 第一条指令处 printk）验证，该版本**已编译未测**（构建被打断）。
3. 是否可能是 **`find_get_pid` / `x_copy_to_user` 本身**在该模块上下文挂死（如 idr 锁/RCU 状态异常）？可在模块里把 OPEN 简化为"只返回固定值、不调任何内核函数"做二分验证（未做）。
4. 是否有**无 pstore、无串口**的 Android 设备上捕获"静默挂死"现场的手段？（Qualcomm ramdump/`/dev/msm_watchdog`/panic 前日志替代方案？）

---
*环境备注：设备端 adb 可用；PC 可 SSH 到构建 VM（Linux Mint，clang 18）；模块每次改动可在 VM 重建（~1 分钟）+ 设备热替换 + 复现（每次复现 = 一次看门狗重启，约 1.5 分钟）。*
