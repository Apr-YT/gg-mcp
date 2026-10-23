# AFEI 移植改动说明（rwProcMem33 / hwBreakpointProc）

本仓库基于上游 `rwProcMem33`，为移植到 **K90 Pro Max / myron / Android 16 / 内核 6.12.23（GKI，abogki462654244）** 所做的改动。
上游运行环境：CFI 关闭、内核 5.x。目标环境新增 `CONFIG_CFI_CLANG=y`、`CONFIG_STRICT_MODULE_RWX=y`、`CONFIG_EXTENDED_MODVERSIONS=y`。

## 1. CFI 绕过（`*/cfi_bypass.h`）
- 目标：把 `__cfi_check_fail` 等 7 个 CFI 失败处理函数改成 `MOVZ X0,#1; RET`（返回 1 表示"校验通过"）。
- 写 text 需要两个运行期符号（均未导出，走 kallsyms 解析）：
  - **`aarch64_insn_write`** —— 写内核指令，内部走 fixmap 自解只读 + patch_lock（**不需要** `set_memory_rw/ro`，两者 2015 起被 arm64 unexport）
  - **`caches_clean_inval_pou`** —— 同步 icache
    ★ `flush_icache_range` 是 `static inline`，内联后转调 `caches_clean_inval_pou`，**自身无独立符号** ⇒ 必须主选后者。
- 幂等：已经是目标指令则跳过（日志 `patched: 0`），同时也是"补丁确实写进去了"的旁证。

## 2. proc 节点残留修复（`*/*_module.c`）
- 根因：`CONFIG_HIDE_BY_DEFAULT` 下 `list_del_init(&__this_module.list)` ⇒ `rmmod` 永久失败 ⇒ `module_exit` 永不执行 ⇒ `/proc/<KEY>` 永久残留 ⇒ 后续同 KEY 模块 `proc_mkdir` 撞名 `WARN` + 返回 NULL ⇒ **永久恶化**（残留**按 KEY 名跨模块共享**）。
- 修复：① `proc_mkdir` **前**无条件 `remove_proc_subtree(KEY, NULL)`（幂等注册，已 EXPORT_SYMBOL）；② `exit` 里无条件兜底清理。

## 3. 逃生舱命令 `CMD_UNHIDE_KERNEL_MODULE`
- 恢复模块在模块链表 / sysfs 的可见性（`kobject_add` + `list_add` 到由 kallsyms 取到的 `modules` 链表），使 `rmmod` 重新可用。
- ★ 命令号因模块而异，需数 enum：rwProcMem 首项显式 `CMD_INIT_DEVICE_INFO = 1` ⇒ `CMD_UNHIDE_KERNEL_MODULE = 15`；hwBreakpointProc 首项从 0 起 ⇒ `= 12`。

## 4. ko 瘦身
- 去除调试信息 / 未用段，两个模块产物分别压缩到 ~39 KB / ~48 KB（详见 `docs-afei/ko_slimming_analysis.md`）。

## 编译要点
1. **`struct module` 尺寸**必须与设备一致 ⇒ 用设备真值 config（8060 行）先 `cp <device.config> gki-6.12/.config` → `olddefconfig` → `modules_prepare -j8`，否则 `Exec format error`。
2. **符号 CRC**：设备开 `CONFIG_EXTENDED_MODVERSIONS`，内核**只读** `__version_ext_crcs` / `__version_ext_names`，**不看** `__versions`；须做 CRC 补丁（`module_layout = 0xe976b219`）。
3. 设备 `CONFIG_MODULE_SIG_FORCE` **未开** ⇒ **无需签名**即可 insmod。

## prebuilt/ 预编译产物
| 文件 | 模块 | 大小 | 说明 |
|---|---|---|---|
| `rwProcMem_module.ko` | rwProcMem | 311,248 B | 完整版（已 CFI 补丁 + 残留修复 + CRC） |
| `rwProcMem_slim.ko` | rwProcMem | 39,552 B | 瘦身版（分发用） |
| `hwBreakpointProc_module.ko` | hwBreakpointProc | 355,032 B | 完整版 |
| `hwBreakpointProc_slim.ko` | hwBreakpointProc | 47,664 B | 瘦身版 |

> 目标内核 vermagic：`6.12.23-android16-5-g16e473de48a3-abogki462654244-4k`

## docs-afei/
上游 / 目标驱动对比（`driver_comparison.md`）、结构对齐分析（`full_alignment.md`）、瘦身分析（`ko_slimming_analysis.md`）。
