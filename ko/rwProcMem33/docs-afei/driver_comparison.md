# 我方驱动 vs AFEI 驱动 —— 逐项对标分析

> 对标日期：2026-10-10
> 我方：(a) `rwProcMem_module`（内存引擎）+ `hwBreakpointProc_module`（断点引擎）
> AFEI：(a) `same-sdk36-6.12.ko`（内存引擎）+ `paradise-6.12.ko`（断点/注入引擎）
> 数据来源：真机实测（REDMI K90 Pro Max / Android16 / 6.12.23）+ NDK27 静态分析

---

## 〇、一句话结论

**我方在"内存读写"这条主线上已经和 AFEI 打平甚至局部更优**（真机 900 MB/s vs AFEI 无实测数据，
但路径更短）；**差距集中在三块**：

1. **能力面**：AFEI 多出「swap 回填、PAC 密钥、指令模拟器、PTE UXN 劫持、输入注入、进程/GPU 隐身」六项，
   我方一项都没有。
2. **工程化**：我方 ko 没做 strip，带 109KB 调试信息 + 170KB 重定位（占文件 88%），
   AFEI 全部 strip 干净。
3. **默认姿态**：我方 "隐藏驱动" 是**手动命令触发**、默认站在明处（`lsmod`/`/proc/modules`/
   `/sys/module` 三处全可见）；AFEI 是**默认就藏**。

---

## 一、双方能力面逐项对标

| 能力维度 | 我方 | AFEI | 差距性质 |
|---|---|---|---|
| 进程内存读写 | ✅ 真机四判据全绿 | ✅ | 平 |
| 物理内存直读/写 | ✅ fast 路径 900 MB/s | ✅ 常规 + _fast 双路径 | **我方略优**（路径更短） |
| swap 页回填 | ❌ 无 | ✅ `swap_read/write_page_direct` | 能力缺失 |
| 匿名 inode 通道 | ✅ 真机验证通过（7 条全绿） | ✅ | 平 |
| CFI 绕过 | ✅ 两驱动各自独立生效 | ✅ | 平 |
| 符号解析（kprobe 法） | ✅ `rw_sym_lookup` | ✅ `kallsyms_lookup_name_ex` | 平（我方无缓存） |
| 隐藏驱动（lsmod） | ⚠️ 有，但**手动触发** | ✅ | **默认姿态差距** |
| 隐藏 /proc 目录项 | ✅ filldir 钩子 | ✅ filldir64 钩子 | 平 |
| 隐藏进程 / GPU 痕迹 | ❌ 无 | ✅ `do_hide_process` + kgsl 钩子 | 能力缺失 |
| 隐藏挂载路径 | ❌ 无 | ✅ `do_hide_path` + `hide_umount_path` | 能力缺失 |
| 反 ptrace | ⚠️ 有 `entry_ptrace_handler` | ✅ `hide_task` + ptrace 拦截 | 我方弱 |
| 硬件断点 | ✅ 有 | ✅ 更完整（记录 ring buffer） | **覆盖度差距** |
| 指令模拟器 | ❌ 无 | ✅ 4.2 万行 ARM64 解码/模拟 | 能力缺失 |
| PTE UXN 劫持 | ❌ 无 | ✅ `ptebp_*` 全家桶 | 能力缺失 |
| PAC 密钥读取/绑定 | ❌ 无 | ✅ pacia/pacga + tpidr_el0 | 能力缺失 |
| 陀螺仪钩子 | ❌ 无 | ✅ `gyro_sendmsg` | 能力缺失 |
| 触摸注入 | ❌ 无 | ✅ `touch_emit` | 能力缺失 |
| 版本覆盖 | ⚠️ 仅 6.12 编译验证 | ✅ 5.10~6.18 共六版本 | **分发差距** |

---

## 二、代码规模与工程化（我方的硬伤）

真机 + 静态分析实测：

| 指标 | rwProcMem_afei.ko | hwBreakpointProc_module.ko | same-6.12.ko | paradise-6.12.ko |
|---|---|---|---|---|
| 文件大小 | 307,504 B | 351,528 B | 157,056 B | 325,184 B |
| **.text（真实代码）** | **7,188 B** | **8,112 B** | 43,732 B | 151,056 B |
| 函数数 | 12 | 34 | 126 | 168 |
| **调试信息(.debug_*)** | **108,595 B** | 121,316 B | **0 B** | **0 B** |
| **重定位(.rela/.rel)** | **170,256 B** | 195,168 B | 59,712 B | 69,768 B |
| `.symtab` | 4,728 B | 6,024 B | 21,144 B | 34,440 B |
| BTF | 无 | 无 | 无 | 28,305 B |

### ★ 关键发现：我方 ko 的 88% 体积是"垃圾"

实测 `llvm-strip --strip-debug` 之后：

```
rwProcMem_afei.ko:  307,504 B  →  37,688 B     （缩小 87.7%）
```

而 `.text`/`.rodata`/`.gnu.linkonce.this_module`/`__version_ext_*` 等功能段**一个都没丢**。
也就是说，**我方 27 万字节都是编译时没关掉的调试信息与重定位表**。

**为什么这是问题（不只是体积）**：
- 分发时体积大 8 倍，APK 里塞进去更显眼；
- `.debug_info` 里含**源码路径、函数名、结构体成员名**——等于把源码结构白送给分析者。
  AFEI 的 ko 里连 `.symtab` 都只有名字，没有 DWARF。
- 反作弊/风控做文件特征扫描时，DWARF 段是极强的指纹。

**修法**：编译加 `INSTALL_MOD_STRIP=1` 或 `make ... && llvm-strip --strip-debug`。
**这一条是投入产出比最高的改进，半天就能做完。**

---

## 三、真机实测数据（我方）

### 3.1 内存读写四判据（全绿）

```
[ OK ] open 节点拿到 fd
[ OK ] INIT_DEVICE_INFO 握手 -> auxv 336 字节
[ OK ] GET_PID_LIST 取回数据且长度一致
[ OK ] PID 抽样交叉验证 -> 共 966 个进程, 抽样 5/5
[ OK ] OPEN_PROCESS -> handle（内核 struct pid*）
[ OK ] ★判据A：读 ELF 魔数 = 7f 45 4c 46
[ OK ] ★判据B：代码段与 /proc/self/mem 逐字节全等（0/1024 块不一致）
[ OK ] ★判据C：跨进程读 64KB 与 /proc/<pid>/mem 逐字节全等（0/8192 块不一致）
[ OK ] ★判据D：写内存往返（A/B 双魔数独立验证）
```

### 3.2 匿名通道（默认版关闭 → anon 版全绿）

默认版：`CMD_ANON_GET_FD` 返回 -1（命令未编译，符合预期，反证分发器 default 分支正确）
anon 版：

```
[ OK ] A1: CMD_ANON_GET_FD 返回有效 fd -> fd=4
[ OK ] A2: fd 存在于当前进程 /proc/self/fd -> anon_inode:rwproc
[ OK ] C1: 匿名 fd ioctl → GET_PID_LIST -> 966
[ OK ] C2: 匿名 fd ioctl → OPEN_PROCESS -> handle
[ OK ] C3: 匿名 fd ioctl → 读 ELF 魔数 = 7f 45 4c 46
-- 全系统 anon_inode:rwproc 持有者：只有本进程 --
```

### 3.3 物理内存路径性能（对照实验）

| 路径 | 单次耗时 | 吞吐 |
|---|---|---|
| slow（`xlate_dev_mem_ptr`） | 90.0 us | **694.3 MB/s** |
| fast（`__va` + `kmap_local_page`） | 69.4 us | **900.6 MB/s** |

**提速 +29.7%**，200 次 × 64KB 稳定复现。
（AFEI 有同样的 `_fast` 路径，但没有公开的吞吐数据可比。）

### 3.4 当前可见性（默认姿态 — 我方的差距）

```
$ lsmod | grep -iE "rwProcMem|hwBreakpoint"
rwProcMem_module           28672  0          ← 可见
hwBreakpointProc_module    32768  0          ← 可见

$ cat /proc/modules | grep -iE "rwProcMem|hwBreakpoint"
rwProcMem_module 28672 0 - Live 0x... (O)    ← 可见
hwBreakpointProc_module 32768 0 - Live 0x... (O)

$ ls -d /sys/module/rwProcMem_module /sys/module/hwBreakpointProc_module
两个都可见

$ ls -d /proc/e84523d7b60d5d341a7c4d1861773ecd
可见（节点目录未隐藏）
```

**对比 AFEI**：它把 `hide_module()`（`list_del_init(&__this_module.list)`）设计成
**默认在 init 里就调用**，`lsmod` 里从一开始就看不到。我方代码里其实有同样的
`OnCmdHideKernelModule`，但要用户态**主动发命令**才生效——"有武器但没掏出来"。

---

## 四、AFEI 的 ioctl 命令面（我方缺的部分）

从 `same_anon_ioctl` / `paradise_anon_ioctl` 的反汇编 + 重定位表还原（这是权威做法，不是猜）：

### same（内存引擎）命令
| cmd 前缀 | 映射函数 | 我方对应 |
|---|---|---|
| 0xc0285708 | `do_write_physical_memory` | ✅ 有 |
| 0xc0285709 | `do_read_physical_memory` | ✅ 有 |
| 0xc028570a | `do_read_physical_memory_fast` | ✅ 有 |
| 0xc028570b | `do_write_physical_memory_fast` | ✅ 有 |
| 0xc028570c | `do_find_process` | ✅ 有（按名找进程） |
| 0xc028570d | `do_list_processes` | ✅ 有 |
| 0xc028570e | `do_get_module_base` | ✅ 有 |
| 0xc028570f | `do_is_process_alive` | ✅ 有 |
| 0xc0285710 | `do_hide_process` | ❌ **无** |
| 0xc104570a/570b/5713/5714/5715 | ptebp_* + hide_path + cfi_bypass | ❌ **绝大部分无** |
| 0xc010/0xc018/0xc020/0xc138/0xc204 系 | gyro / ptebp / 高级功能 | ❌ **无** |

### paradise（断点/注入引擎）命令
`do_hwbp_set / do_hwbp_remove / do_hwbp_get_config / do_hwbp_get_records /
do_hwbp_set_record / do_hwbp_clear_records / do_hwbp_install`（硬件断点全家桶）
`do_get_main_thread_elf0 / do_get_thread_tpidr_el0 / do_get_thread_pacia_key /
do_get_thread_pacga_key / do_bind_thread_pacga_key`（PAC 五连）
`shushu_touch_init/handle/destroy`（触摸）、`gyro` （陀螺仪）

**我方只有其中"内存 + 基础断点"两小块。**

---

## 五、架构对比

### 通信通道

| | 我方 | AFEI |
|---|---|---|
| 主通道 | `/proc/<KEY>/<KEY>`（32 位 hex 目录名） | 匿名 inode（无 /proc 节点） |
| 通道协议 | `read(fd, 33+size)`，返回值=命令返回值 | `ioctl(fd, 33, buf)` |
| 备用通道 | ✅ 匿名 inode（可选开关，默认关） | 无（只有匿名） |
| 节点可见性 | 目录名混淆，但 `ls /proc` 能看到 | 完全不出现 |

**评价**：我方**双通道**设计其实比 AFEI 更灵活（保留 /proc 便于调试，匿名用于实战），
但**默认姿势不够隐蔽**——AFEI 是"只留一条看不见的线"，我方是"明线 + 可选暗线"。

### 符号解析

| | 我方 | AFEI |
|---|---|---|
| 方法 | 注册 kprobe 偷 `kallsyms_lookup_name` | 同左 + **带全局缓存** `kallsyms_lookup_name_ex` |
| 缓存 | ❌ 每次重查 | ✅ 查一次存全局变量 |

AFEI 的缓存实现在反汇编里看得很清楚：先查全局 `[x19]`，命中就 `blr x8` 直接跳，
未命中才走 `register_kprobe`。**我方每次解析都要注册/注销一个 kprobe**——这是可优化点。

---

## 六、差距清单与优先级建议

### P0（低成本高收益，建议立刻做）

1. **ko 加 strip**：`--strip-debug`，文件 307KB→37KB（-87.7%），
   同时抹掉源码路径与结构体名。**半天工作量，风险为零。**

2. **隐藏改默认**：把 `OnCmdHideKernelModule` 的 `list_del_init` + `kobject_del`
   在 `init_module` 末尾直接调用（或加 `CONFIG_HIDE_BY_DEFAULT` 开关默认开）。
   现在等于把武器收在鞘里。**这是"对标 AFEI 默认姿态"最直接的一条。**

### P1（中等成本，能力补齐）

3. **符号解析加缓存**：仿 `kallsyms_lookup_name_ex`，全局存一份，避免反复注册 kprobe。

4. **swap 页回填**：`swap_read/write_page_direct`——AFEI 用它处理被换出的页，
   我方遇到 swap 页会失败。属"读内存完整性"的短板。

5. **PAC 密钥读取**：`get_thread_pacia_key / pacga_key` + `bind_thread_pacga_key`。
   ARM64 上很多函数指针带 PAC 签名，拿不到密钥就没法构造合法调用。

### P2（高成本，看需求决定）

6. **指令模拟器**：paradise 里 `emu_simulate_fp_simd_insn` 单函数就 **41,996 字节**，
   加上全套 `arm64_decode_*` 约 4.2 万行。这是"硬件断点命中后能继续执行"的关键，
   工作量最大。
7. **PTE UXN 劫持**：`ptebp_*` 全家桶，用页表权限位做陷阱替代 inline hook。
8. **输入注入**：陀螺仪 + 触摸。这是"游戏辅助"面向用户的功能层，
   与底层内存能力相对独立。

### P3（分发）

9. **多版本编译**：AFEI 覆盖 5.10~6.18 六版本。我方只验证了 6.12。
   本机只有 6.12 内核树，需要补其他版本的树或 config。

---

## 七、客观评价（哪边更强）

**不是简单的一句话谁强谁弱，要分维度看：**

- **核心内存能力**：**我方不落下风**。真机四判据全绿、900 MB/s、双通道设计、CRC 补丁
  + CFI 绕过双驱动独立生效——这条主线上我方是"能打"的。
- **功能广度**：**AFEI 明显更全**。它是一套"完整商业辅助"的后端：内存 + 断点 + 模拟 +
  隐身 + 输入注入五件套齐全；我方是"两个专项工具"。
- **工程化程度**：**AFEI 更成熟**（strip 干净、六版本并包、命令面 20+、
  全局缓存、默认隐身）。
- **代码质量**：**我方有些地方更干净**。AFEI 的 `paradise` 有 4.2 万行指令模拟器，
  但它连 BTF 都带上了（28KB）；我方代码量小、结构清晰、注释完整
  （`anon_channel.h` 里那条 `fd_install` 的坑注释就是很好的例子）。

**一句话**：**我方是"精悍的专项工具"，AFEI 是"臃肿但完整的商业套件"。**
如果把 P0 两条（strip + 默认隐身）做完，我方在"内存读写"这条线上可以正面对标；
要做到功能全面对标，P1/P2 是绕不过去的工程量。

---

## 八、附：本次对标用到的真机命令

```bash
# 加载顺序（CFI 绕过须最早）
insmod rwProcMem_module.ko && insmod hwBreakpointProc_module.ko

# 四判据自检
su -c /data/local/tmp/rwtest

# 匿名通道自检（需 anon 版 ko）
su -c '/data/local/tmp/anontest'

# 性能对照
su -c '/data/local/tmp/rwbench'

# 可见性检查
su -c 'lsmod | grep -iE "rwProcMem|hwBreakpoint"'
su -c 'cat /proc/modules | grep -iE "rwProcMem|hwBreakpoint"'
su -c 'ls -d /sys/module/rwProcMem_module'
su -c 'ls -d /proc/e84523d7b60d5d341a7c4d1861773ecd'
```
