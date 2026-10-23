# 我方 ko 体积剖析 & 取舍报告

> 日期：2026-10-10
> 对象：`rwProcMem_module.ko`（内存引擎 308,272 B）、`hwBreakpointProc_module.ko`（断点引擎 352,312 B）
> 方法：NDK27 `llvm-readelf/-objdump/-strip` 静态剖析 + REDMI K90 Pro Max 真机加载验证
> 一句话：**"无用内容"= 调试信息 + 重定位表，占 90%；可以删，且删除有益。**

---

## 一、先回答"都有哪些内容"

两个 ko 的段按性质可归为 **6 类**。下面是实测全量账目（rwProcMem 为例）：

| 类别 | 合计 | 占比 | 是否装载进内核 | 能不能删 |
|---|---|---|---|---|
| **调试信息（DWARF）** | **108,802 B** | **35.3%** | ❌ 不装载 | ✅ 可删 |
| **重定位表（.rela.*）** | **170,688 B** | **55.4%** | 部分需要 | ⚠️ 部分可删 |
| 功能段（.text/.rodata/…） | 14,526 B | 4.7% | ✅ 装载 | ❌ 不能删 |
| 符号表（.symtab） | 4,728 B | 1.5% | ✅ **必须** | ❌ 不能删 |
| 字符串表（.strtab/.shstrtab） | 2,913 B | 1.0% | ✅ 必须 | ❌ 不能删 |
| 标记（.note.*） | 116 B | 0.0% | ❌ | ✅ 可删 |

**关键结论：真正的"驱动本体"只有 14,526 B（4.7%）；其余 95.3% 全是给调试器和链接器看的。**

hwBreakpointProc 同构：

| 类别 | 合计 | 占比 |
|---|---|---|
| 调试信息 | 121,546 B | 34.5% |
| 重定位表 | 195,600 B | 55.5% |
| 功能段 | 18,342 B | 5.2% |
| 符号表 + 字符串表 + 标记 | 9,668 B | 2.7% |

---

## 二、逐项拆解（每一块到底是什么）

### 2.1 调试信息 108,802 B —— 这是最大的一块，且完全是垃圾

| 段 | 大小 | 是什么 |
|---|---|---|
| `.debug_info` | **69,934 B** | 核心调试信息：**每个结构体成员名、每个变量的类型与作用域、每行源码对应的指令地址** |
| `.debug_str` | 19,912 B | 上面引用的所有字符串（**含源码文件的完整路径**） |
| `.debug_line` | 7,962 B | 行号表：一行源码 ↔ 一条指令的映射 |
| `.debug_loclists` | 6,706 B | 局部变量位置列表 |
| `.debug_rnglists` | 1,420 B | 地址范围列表 |
| `.debug_abbrev` | 1,412 B | `.debug_info` 的编码表 |
| `.debug_line_str` | 1,336 B | 行号表用的字符串 |
| `.debug_str_offsets` / `.debug_addr` | 120 B | 索引表 |

**为什么说它是垃圾（三个理由）：**

1. **内核根本不读它。** 调试段不参与模块加载、不做符号解析、不参与版本校验。加载器看都不看。
2. **它是信息泄漏。** `.debug_info`、`.debug_line_str` 里明文写着你的**源码目录结构**（如 `D:/Kiln/_download/rwProcMem33-master/...`）、**结构体成员名**（`is_hidden_module`、`proc_parent`）、**函数名**。等于把源码骨架送给对手。
3. **它是强指纹。** 反作弊/风控做文件特征扫描时，DWARF 的存在本身就是"这是个自己编译的调试版模块"的标志。

**处理：删。** 成本为零，收益三项（体积、泄漏、指纹）。

### 2.2 重定位表 170,688 B —— 要区分对待

| 段 | 大小 | 性质 | 能不能删 |
|---|---|---|---|
| `.rela.debug_str_offsets` | **141,816 B** | 调试信息的重定位，**只为 DWARF 服务** | ✅ 跟着调试段一起删 |
| `.rela.debug_line` | 8,736 B | 同上 | ✅ |
| `.rela.debug_addr` | 8,256 B | 同上 | ✅ |
| `.rela.debug_info` | 336 B | 同上 | ✅ |
| `.rela.text` | 8,328 B | **代码段重定位（函数调用/数据引用）** | ❌ 绝不能删 |
| `.rela.init.text` | 1,680 B | init 段重定位 | ❌ 不能删 |
| `.rela__jump_table` | 432 B | jump label 表 | ❌ |
| `.rela.exit.text` | 336 B | exit 段 | ❌ |
| `.rela.rodata` / `.rela.data` / `.rela__bug_table` 等 | ~570 B | 数据重定位 | ❌ |

**规律：文件名里带 `.debug` 的重定位（共 159,144 B）纯属陪葬，删调试段时必须一起删；其余的（约 11,500 B）是功能必需。**

### 2.3 功能段 14,526 B —— 驱动的真身

| 段 | 大小 | 作用 |
|---|---|---|
| `.text` | 7,188 B | **真实代码**（内核运行时执行的就是这 7 KB） |
| `.gnu.linkonce.this_module` | 1,600 B | `struct module` 本体。★**设备硬门槛：必须恰好 1600 字节**，错一点就 `Exec format error` |
| `__versions` | 1,536 B | 传统 modversions 符号 CRC（本设备不读，但编进去了） |
| `.rodata` | 1,033 B | 只读数据（命令表、字符串） |
| `.init.text` | 816 B | init 函数（加载后释放） |
| `.init.eh_frame` | 656 B | 栈回溯表（×86_64 无关，arm64 上是固定元数据） |
| `.bss` / `.data` | 568 B | 全局变量 |
| `__version_ext_names` | 332 B | ★扩展 modversions：符号名表 |
| `.modinfo` | 222 B | ★模块元信息（name/vermagic/license/依赖） |
| `.comment` | 184 B | 编译器版本字符串 |
| `.exit.text` | 116 B | 卸载函数 |
| `__version_ext_crcs` | 96 B | ★扩展 modversions：符号 CRC 表 |
| `__jump_table` / `__bug_table` / `.codetag` / `.data..percpu` / `.plt` | ~180 B | 内核机制辅助表 |

★ 标记的是**设备强校验**：`this_module`=1600 是结构体大小门槛，`__version_ext_*` 是符号 CRC 校验源，`.modinfo` 的 `vermagic` 必须与设备内核完全一致。**这三个动不得。**

### 2.4 符号表 + 字符串表 7,641 B —— 看似冗余，实则保命

| 段 | 大小 | 作用 |
|---|---|---|
| `.symtab` | 4,728 B | ★**内核加载时按它匹配符号** |
| `.strtab` | 2,083 B | 符号名字符串 |
| `.shstrtab` | 830 B | 段名字符串 |
| `.llvm_addrsig` | 31 B | 链接期辅助 |

**★ 这一块是本报告最重要的一课，详见第三节。**

### 2.5 标记 116 B

`.note.Linux`（ABI 标记）、`.note.gnu.build-id`（构建 ID）、`.note.gnu.property`（BTI/PAC 属性）。可删，但收益仅 116 B，且 `build-id` 删掉可能影响某些内核的模块去重逻辑 —— **建议保留**。

### 2.6 空段 0 B

`.hyp.*` 系列（`.hyp.text` 等）全是 **0 字节**，是 KVM/虚拟化预留的段占位。不占空间，可以无视。

---

## 三、★★ 真机实测：裁剪的"红线"在哪里

我做了逐级退化实验，每一级都在真机上实际 `insmod`，这是**唯一可靠的判据**：

| 级别 | 命令 | 体积（rwProcMem） | 真机结果 |
|---|---|---|---|
| 原始 | 无 | 308,272 B | ✅ 加载正常 |
| **L1** | `llvm-strip --strip-debug` | **38,056 B（-87.7%）** | ✅ **加载正常，四判据全绿** |
| **L2** | `llvm-strip --strip-unneeded` | **35,864 B（-88.4%）** | ✅ **加载正常** |
| **L3** | `llvm-strip --strip-all` | **19,472 B（-93.7%）** | ❌ **加载失败** |

### L3 失败的确切原因（内核原文）

```
rwProcMem_module: module has no symbols (stripped?)
insmod: failed to load /data/local/tmp/rw_tiny.ko: Exec format error
```

**内核明说你"没有符号"。** `--strip-all` 把 `.symtab` 整个删了，而内核加载器**靠 `.symtab` 匹配模块符号**（模块依赖、`__this_module` 定位、modversions 校验都要走它）。**删掉 = 模块废掉。**

### 所以红线是：

```
✅ 可以删：全部 .debug_* 及其 .rela.debug_*     → 省 87.7%
❌ 绝不能删：.symtab / .strtab / .shstrtab      → 删了内核拒绝加载
❌ 绝不能删：.gnu.linkonce.this_module（须1600B）/ __version_ext_* / .modinfo
```

**L1 与 L2 都可用，差距只有 2,192 B。推荐用 L1（`--strip-debug`）** —— 它保留完整 `.symtab`，行为与原始版最接近，风险最低；L2 的 `--strip-unneeded` 会裁掉"非重定位引用的符号"，虽然本机通过，但**跨内核版本可能有依赖 `.symtab` 全量符号的校验路径**，不值得为 2 KB 冒险。

---

## 四、附带发现：一条一直存在、从未被注意的内核告警

做这次剖析时，我在 `dmesg` 里发现我们两个驱动**每次加载都会触发一条内核 WARNING**：

```
WARNING: CPU: 1 PID: 20102 at fs/proc/generic.c:376 proc_register+0x12c/0x1b0
proc_dir_entry '/proc/e84523d7b60d5d341a7c4d1861773ecd' already registered
Call trace:
  proc_register+0x12c/0x1b0
  proc_mkdir+0x60/0x8c
  init_module+0x208/0xf54 [rwProcMem_module]
```

### 我做了什么来定位它

1. **先排除"我的改动引入"**：拿 12:37 构建的、最接近上游原始版的 ko 测试 → **照样告警**。
   ⇒ **这是上游 rwProcMem33 的老问题，与本次移植无关。**
2. **验证是否为临时残留**：`dmesg -c` 清空后首次 `insmod` 就告警，且 `rmmod` 能干净删除节点、连续两次加载告警数恒为 1（不是 2）。
   ⇒ **不是残留，是 init 内部就发生。**
3. **读内核权威源码**（`fs/proc/generic.c:376`）：
   ```c
   if (pde_subdir_insert(dir, dp) == false) {
       WARN(1, "proc_dir_entry '%s/%s' already registered\n", dir->name, dp->name);
   ```
   `pde_subdir_insert` 是**红黑树插入**，同名即返回 false 并 WARN。`dir->name` 实测为 `"/proc"`（内核 `fs/proc/root.c:375` 里 `proc_root.name = "/proc"`），所以打印出 `'/proc/<KEY>'`。
4. **写最小复现模块去撞**（关键一步，避免瞎猜）：
   - probe（只做 `proc_mkdir` + `proc_create` 同名）→ **不告警**
   - probe2（再加 `register_kprobe("proc_root_readdir")`）→ **仍不告警**
   - 编译一版**关掉 CFI bypass** 的我方驱动 → **仍然告警**
   ⇒ 排除了"同名注册"、"kprobe"、"CFI bypass"三个嫌疑。

### ★★★ 根因已 100% 锁定（2026-10-10 追加）

**一句话根因：隐身让模块无法卸载，于是 `/proc` 节点永久残留，此后每次加载都撞名。**

完整因果链（每一步都有真机证据）：

1. 装载带 `CONFIG_HIDE_BY_DEFAULT` 的模块 ⇒ `hide_module_by_default()` 执行
   `list_del_init(&__this_module.list)`，把模块从**内核模块链表**里摘掉。
2. 从此 `rmmod <模块名>` **永远失败**（内核在链表里找不到它，实测返回
   `failed to unload ...: No such file or directory`），于是 `module_exit` /
   `rwProcMem_dev_exit()` **永远不执行**。
3. 于是 `proc_remove()` 永远不跑，`/proc/<KEY>` 目录**永久残留在 `proc_root`
   的红黑树**（`proc_root->subdir`）里。
4. 此后任何模块（**哪怕是不带 hide 的版本**，只要 KEY 相同）加载时：
   `proc_mkdir → proc_register → pde_subdir_insert` 撞上同名节点 ⇒ `WARN(1, ...)`
   ⇒ **返回 NULL**，且残留节点被保留、本次新建的条目被 `pde_free` 丢弃。
5. 因为 `proc_parent == NULL`，代码里的 `if (proc_parent)` 不成立 ⇒
   **`proc_create` 被跳过**；`exit` 里 `proc_entry`/`proc_parent` 也是 NULL ⇒
   **`proc_remove` 全被跳过** ⇒ 节点**永久恶化、再也无人清理**。

**关键实验证据（决定性）：**

| 实验 | 结果 | 说明 |
|---|---|---|
| 重启后**开机首次** auto-load | **零告警** | `proc_root` 干净 |
| `rmmod` 后节点是否消失（无 hide 版本） | **消失** | `proc_remove` 本身没问题 |
| 带 hide 版本加载 → `rmmod` | **失败** | 根因步骤 2 |
| 残留状态下用修复版覆盖加载 | **零告警** | 验证修复有效 |

注意：`rw_earliest.ko`（无 hide）之所以也告警，是因为 `proc_root` 里的残留节点
是**之前带 hide 的版本**留下的 —— 残留是**跨模块、按 KEY 名共享**的。

### 修复方案（已实施 + 真机验证）

**修复 1（核心）· 幂等注册** —— 在 `proc_mkdir` **之前**无条件清理同名残留：

```c
/* rwProcMem_dev_init() 里，proc 注册段开头 */
remove_proc_subtree(CONFIG_PROC_NODE_AUTH_KEY, NULL);   /* 已 EXPORT_SYMBOL */

g_rwProcMem_devp->proc_parent = proc_mkdir(CONFIG_PROC_NODE_AUTH_KEY, NULL);
if (g_rwProcMem_devp->proc_parent) {
    g_rwProcMem_devp->proc_entry = proc_create(..., g_rwProcMem_devp->proc_parent, &ops);
} else {
    /* 双保险：即使 proc_mkdir 仍失败，也把控制文件挂到 /proc 根，保证功能可用 */
    g_rwProcMem_devp->proc_entry = proc_create(..., NULL, &ops);
}
if (g_rwProcMem_devp->proc_entry) start_hide_procfs_dir(KEY);
```

**修复 2（配套）· 对称清理** —— `exit` 里加按名字兜底：

```c
remove_proc_subtree(CONFIG_PROC_NODE_AUTH_KEY, NULL);   /* exit 收尾无条件执行 */
```

**修复 3（根治可用性）· 逃生舱 `CMD_UNHIDE_KERNEL_MODULE`** ——
既然隐身必然让 `rmmod` 失效，就提供一条**用户态可控的卸载出口**：

```c
static int unhide_module(void) {
    if (!hidden) return 0;
    /* ① sysfs 挂回（kobject_add 已导出） */
    kobject_add(&THIS_MODULE->mkobj.kobj, THIS_MODULE->mkobj.kobj.parent,
                "%s", THIS_MODULE->name);
    /* ② 模块链表挂回（`modules` 未导出，走我们的 rw_sym_lookup → kallsyms） */
    unsigned long a = rw_sym_lookup("modules");
    if (a) list_add(&__this_module.list, (struct list_head *)a);
    hidden = false;
    return 0;
}
```

命令号：`rwProcMem = 15`（enum 显式 `=1` 起）、`hwbp = 12`
（enum **从 0 起**，无显式赋值 —— 这一点很容易数错，务必核对各自的头文件）。

### 真机验证结果（全部通过）

| 验证项 | 结果 |
|---|---|
| 干净状态加载 | ✅ **零告警**，节点正常 |
| **残留状态下覆盖加载**（核心） | ✅ `already registered = 0`、`WARNING = 0`、`Tainted W = 0` |
| 5 轮 insmod（含 unhide + rmmod）循环 | ✅ 全绿，卸载后节点彻底消失 |
| rwProcMem 逃生舱 | ✅ `module unhidden (list restored)` → `rmmod = 0` → `Goodbye` |
| hwBreakpointProc 逃生舱（cmd=12） | ✅ 同上 |
| `rwtest` 功能回归（9 判据） | ✅ 全绿（含跨进程 64KB 全等、写内存往返） |
| 开机 auto-load | ✅ 双模块零告警、隐身生效（`lsmod`/`/proc/modules`/`/sys/module` 全盲） |
| 隐身效果 | ✅ `lsmod=0`、`/proc/modules=0`、`ls /sys/module=0`、`ls /proc/ \| grep=0`，但直接路径可访问 |

### 影响（原记录，保留备查）

1. **告警会泄漏模块名**：WARNING 里含 `Modules linked in: rwProcMem_module(O+)`，
   做风控的人一查 `dmesg` 就看见我们了 —— 与"默认隐身"目标直接冲突。**现已消除。**
2. `WARN` 会给内核打 `Tainted: [W]` 污点标记，某些环境会被审计捕获。**现已消除。**

---

## 五、取舍清单（结论）

| 内容 | 大小 | 处置 | 理由 |
|---|---|---|---|
| `.debug_*`（DWARF） | 108,802 B | **删** | 不装载 + 泄漏源码 + 风控指纹 |
| `.rela.debug_*` | 159,144 B | **删** | 纯属调试段陪葬 |
| `.note.*` | 116 B | 保留 | 收益太小，build-id 可能有副作用 |
| `.symtab` / `.strtab` / `.shstrtab` | 7,641 B | **必须保留** | ★删了内核拒绝加载（L3 实测） |
| `.gnu.linkonce.this_module` | 1,600 B | **必须保留** | ★设备硬门槛恰好 1600 B |
| `__version_ext_crcs` / `__version_ext_names` | 428 B | **必须保留** | ★设备符号 CRC 校验源 |
| `.modinfo` | 222 B | **必须保留** | ★vermagic 必须匹配内核 |
| 其余功能段 | ~9,500 B | 保留 | 驱动本体 |
| `.hyp.*` | 0 B | 忽略 | 空段不占空间 |

**最终成绩：`rwProcMem` 308,272 → 38,056 B（-87.7%）；`hwBreakpointProc` 352,312 → 46,256 B（-86.9%），功能分毫不少。**

**推荐做法（二选一）：**
```bash
# 事后处理
llvm-strip --strip-debug <patched.ko>

# 或编译期（推荐，一步到位）
make ... INSTALL_MOD_STRIP=1 modules
```

---

## 附：本轮真机实验记录

| 测试项 | 结果 |
|---|---|
| L1（strip-debug）加载 | ✅ RC=0，四判据全绿 |
| L2（strip-unneeded）加载 | ✅ RC=0 |
| L3（strip-all）加载 | ❌ `module has no symbols (stripped?)` |
| probe（proc 同名注册最小复现） | ✅ 无告警，反复加载/卸载均正常 |
| probe2（+proc_root_readdir kprobe） | ✅ 无告警 |
| 关闭 CFI bypass 的我方驱动 | ⚠️ 仍告警 ⇒ 排除 CFI bypass |
| 最早期构建（12:37，近上游原始） | ⚠️ 仍告警 ⇒ **确认与本次移植无关** |
| 告警后功能验证 | ✅ `rwtest` 九条判据全绿 |

**结论：体积问题已彻底解决且可安全取舍；发现的 `proc_register` 告警是上游遗留，需单独立项修复，优先级 P0（因它泄漏模块名，抵消隐身价值）。**
