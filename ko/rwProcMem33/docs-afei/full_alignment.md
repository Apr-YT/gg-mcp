# 我方驱动 vs AFEI 驱动 —— 全面对标（含已实施改进）

> 对标日期：2026-10-10
> 我方：`rwProcMem_module`（内存引擎）+ `hwBreakpointProc_module`（断点引擎）
> AFEI：`same-sdk36-6.12.ko`（内存引擎）+ `paradise-6.12.ko`（断点/注入引擎）
> 平台：REDMI K90 Pro Max / 25102RKBEC / Android 16 / 内核 6.12.23
> 数据来源：真机实测 + NDK27 静态分析（`llvm-nm/-readelf/-objdump/-strip`）

---

## 〇、本轮对标做了什么

不是只列清单，而是**把两项可以立刻闭环的差距直接改掉并在真机验证**：

| 项 | 改动前 | 改动后 | 状态 |
|---|---|---|---|
| P0-1 ko 体积 | 308 KB / 352 KB | **38 KB / 46 KB** | ✅ 已实施 + 真机验证 |
| P0-2 默认隐身 | `lsmod` 可见 | **三处全不可见** | ✅ 已实施 + 真机验证 |

其余（能力面、性能面）给出逐项对标结论与实施路径。

---

## 一、四维对标总览

| 维度 | 我方 | AFEI | 胜负 |
|---|---|---|---|
| **能力面**（功能覆盖） | 8 项 | 18 项 | **AFEI 领先** |
| **性能面**（核心读写） | 900.6 MB/s | 无公开数据（同款 `_fast` 路径） | 我方至少不输 |
| **隐蔽性**（默认姿态） | 三处全隐藏（本轮修复） | 三处全隐藏 | **已打平** |
| **工程化**（体积/多版本） | 38 KB（本轮修复）/ 仅 6.12 | 157 KB / 六版本 | 体积打平，版本落后 |

---

## 二、能力面逐项对标（完整矩阵）

### 2.1 已对齐（✅ 我方也有）

| 能力 | 我方实现 | AFEI 实现 | 备注 |
|---|---|---|---|
| 进程内存读写 | `rwProcMem_read` + `DispatchCommand` | `same_anon_ioctl` | 真机四判据全绿 |
| 物理内存直读/写 | `do_read/write_physical_memory(_fast)` | 同名 | 我方 900 MB/s |
| 匿名 inode 通道 | `anon_channel.h` | `same_anon_ioctl` + `same_install_fd` | 真机 7 条全绿 |
| CFI 绕过 | `cfi_bypass`（独立移植） | `cfi_bypass` | 两驱动各自独立生效 |
| 符号解析（kprobe 法） | `rw_sym_lookup` | `kallsyms_lookup_name_ex` | 我方**无缓存** |
| 隐藏驱动 | `OnCmdHideKernelModule` + 本轮默认化 | `hide_module()` | 本轮已打平 |
| 隐藏 /proc 目录项 | `my_filldir` 钩子 | `filldir64_hook_work` | 平 |
| 硬件断点 | `hwbp_handler` / `entry_ptrace_handler` | `do_hwbp_*` 全家桶 | 我方**覆盖度弱** |

### 2.2 我方缺失（❌ AFEI 有）

| # | 能力 | AFEI 实现 | 代码量 | 价值 |
|---|---|---|---|---|
| 1 | **swap 页回填** | `swap_read/write_page_direct` | ~1.5 KB | 读被换出页不失败 |
| 2 | **隐藏进程** | `do_hide_process` + `hide_task_*` | ~3 KB | 辅助进程不可见 |
| 3 | **隐藏 GPU 痕迹** | `same_hide_kgsl_remove` / `hide_kgsl_install` | ~2 KB | 反 GPU 侧检测 |
| 4 | **隐藏挂载路径** | `do_hide_path` + `hide_umount_path` | ~2 KB | 隐藏部署目录 |
| 5 | **PAC 密钥读取/绑定** | `do_get_thread_pacia/pacga_key`、`do_bind_thread_pacga_key` | ~3 KB | ARM64 签名场景必需 |
| 6 | **指令模拟器** | `emu_simulate_fp_simd_insn` + 全套 `arm64_decode_*` | **70,944 B** | 断点命中后继续执行 |
| 7 | **PTE UXN 劫持** | `ptebp_*` 全家桶 | ~8 KB | 替代 inline hook 的隐蔽方案 |
| 8 | **陀螺仪钩子** | `gyro_sendmsg` + `do_config_gyro_hook` | ~2 KB | 输入注入 |
| 9 | **触摸注入** | `touch_emit` + `shushu_touch_*` | 4,360 B | 输入注入 |
| 10 | **硬件断点记录环** | `do_hwbp_get_records` / `set_record` / `clear_records` | ~3 KB | 命中轨迹留存 |

### 2.3 实测 AFEI 模拟器规模（我方完全空白的一块）

```
指令模拟器(emu_*):        49,748 B  (22 个函数)
  └ 最大单函数 emu_simulate_fp_simd_insn = 41,996 B
arm64 解码器(decode_*):    16,836 B  (10 个函数)
   └ arm64_decode_simd 单函数 = 8,724 B
触摸注入(shushu/touch):     4,360 B  (6 个函数)
──────────────────────────────────────────
三块合计:                  70,944 B
```

**参照系**：我方 rwProcMem 全部 `.text` 只有 **7,188 B**。也就是说，
**AFEI 光"指令模拟器"一块就是我一整个驱动的 10 倍大**。这是它真正的技术护城河。

---

## 三、性能面对标（真机实测）

### 3.1 物理内存路径（对照实验）

| 路径 | 单次耗时 | 吞吐 | 说明 |
|---|---|---|---|
| slow（`xlate_dev_mem_ptr`） | 90.0 us | 694.3 MB/s | 我方基线 |
| **fast（`__va` + `kmap_local_page`）** | **69.4 us** | **900.6 MB/s** | **+29.7%** |

测试条件：200 次 × 64KB = 12.5 MB，8/8 稳定复现。

AFEI 有同名的 `_fast` 路径，但**没有公开吞吐数据**；
从反汇编看它走 `vm_rw` + `alloc_page`，链路与我方 `__va` 直算相当。

### 3.2 其他性能维度

| 维度 | 我方 | AFEI |
|---|---|---|
| 符号解析 | 每次重新注册 kprobe | **全局缓存，命中直接跳** |
| 匿名通道建立 | 一次性 `fd_install` | 一次性 `fd_install` |
| 通信协议 | `read()` / `ioctl()` | `ioctl()` |

**我方唯一可优化点**：符号解析没缓存。AFEI 的 `kallsyms_lookup_name_ex`
先查全局 `[x19]`，命中就 `blr x8` 直接调用，省掉 kprobe 注册/注销的开销。

---

## 四、隐蔽性对标（本轮已打平）

### 4.1 改动前（我方默认站在明处）

```
lsmod          → rwProcMem_module / hwBreakpointProc_module   都可见
/proc/modules  → 两个都可见
/sys/module    → 两个都可见
```

### 4.2 改动后（真机实测）

```
lsmod          → （空）
/proc/modules  → （空）
/sys/module    → No such file or directory
```

dmesg 确认：
```
[1450.705144] [rwProcMem] module hidden by default (init)
[1456.341782] [hwBreakpointProc] module hidden by default (init)
```

**实现方式**（与 AFEI 完全一致）：
```c
list_del_init(&__this_module.list);        // 摘掉模块链表
kobject_del(&THIS_MODULE->mkobj.kobj);     // 删除 sysfs 节点
```

**关键设计**：放在 `rwProcMem_dev_init()` 的**最后一步**——
proc 节点、匿名分发都注册完之后再隐藏，避免隐藏过程中出错不好排查。

### 4.3 功能未受影响（同版本 slim ko 实测）

```
[ OK ] open 节点拿到 fd
[ OK ] INIT_DEVICE_INFO 握手 -> auxv 336 字节
[ OK ] GET_PID_LIST 取回数据且长度一致
[ OK ] PID 抽样交叉验证 -> 共 964 个进程, 抽样 5/5
[ OK ] OPEN_PROCESS -> handle
[ OK ] ★判据A：读 ELF 魔数 = 7f 45 4c 46
[ OK ] ★判据B：代码段与 /proc/self/mem 逐字节全等（0/1024）
[ OK ] ★判据C：跨进程读 64KB 逐字节全等（0/8192）
[ OK ] ★判据D：写内存往返（A/B 双魔数）
```

**隐身 + 功能 同时达成。**

---

## 五、工程化对标（本轮已修复体积）

### 5.1 体积（本轮最大改进）

| | 改动前 | 改动后 | 降幅 |
|---|---|---|---|
| rwProcMem | 308,272 B | **38,056 B** | **-87.7%** |
| hwBreakpointProc | 352,312 B | **46,256 B** | **-86.9%** |
| AFEI same-6.12 | 157,056 B | 157,056 B | （参考） |
| AFEI paradise-6.12 | 325,184 B | 325,184 B | （参考） |

**我方 slim 版比 AFEI 的 same 驱动还小 4 倍。**

### 5.2 为什么原来那么大（根因）

| 段 | 大小 | 性质 |
|---|---|---|
| `.debug_*`（DWARF） | 108,595 B | 调试信息，含源码路径/结构体名 |
| `.rela.*`（重定位） | 170,256 B | 链接期产物 |
| `.text`（真实代码） | 7,188 B | ← **真正的驱动只有这么多** |

**危害**：不只是体积。`.debug_info` 里的**源码路径 + 结构体成员名**
等于把源码结构白送；反作弊做文件特征扫描时 DWARF 是极强指纹。

**修复**：`llvm-strip --strip-debug`（或编译加 `INSTALL_MOD_STRIP=1`）
→ 功能段（`.text` / `.rodata` / `.this_module` / `__version_ext_*`）一个不丢。

### 5.3 关键校验（strip 后仍满足设备门槛）

```
.gnu.linkonce.this_module   size = 0x640 (1600)   ← 设备硬要求，命中
__version_ext_crcs          size = 0x60
__version_ext_names         size = 0x14c
```

### 5.4 版本覆盖（仍落后）

| | 我方 | AFEI |
|---|---|---|
| 已编译验证 | 6.12 | 5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18 |
| 内核树 | 仅 `/home/king/gki-6.12` | 六套 |

---

## 六、架构对标

| 维度 | 我方 | AFEI | 评价 |
|---|---|---|---|
| 通道数量 | **双通道**（/proc + 匿名，可切） | 单通道（只有匿名） | **我方更灵活** |
| 主通道协议 | `read(fd, 33+size)` | `ioctl(fd, 33, buf)` | 等价 |
| 节点暴露面 | 目录名混淆可枚举 | 完全不出现 | AFEI 更隐蔽 |
| 符号解析 | kprobe 法，无缓存 | kprobe 法 + 缓存 | AFEI 更优 |
| 驱动划分 | 内存 / 断点 两个独立 ko | 内存 / 断点+注入 两个独立 ko | 平 |
| 日志级别 | `printk_debug` 宏可控 | 同款 | 平 |

---

## 七、改进清单（按优先级）

### ✅ 已完成（本轮）

| # | 项 | 成果 |
|---|---|---|
| P0-1 | ko strip | 308KB→**38KB**（-87.7%），功能段完整，真机加载 + 四判据全绿 |
| P0-2 | 默认隐身 | `lsmod`/`/proc/modules`/`/sys/module` 三处全隐藏，功能不受影响 |

### 📋 待实施

| # | 项 | 成本 | 收益 |
|---|---|---|---|
| P1-1 | 符号解析加全局缓存 | 低（~30 行） | 省 kprobe 注册开销 |
| P1-2 | swap 页回填 | 中（~150 行） | 读换出页不失败 |
| P1-3 | PAC 密钥读取/绑定 | 中（~200 行） | ARM64 签名场景 |
| P1-4 | 隐藏进程 / GPU / 挂载路径 | 中（~400 行） | 完整隐身 |
| P2-1 | 硬件断点记录环 | 中（~200 行） | 命中轨迹留存 |
| P2-2 | 陀螺仪 + 触摸注入 | 中（~500 行） | 输入注入能力 |
| P3-1 | PTE UXN 劫持 | 高（~800 行） | 隐蔽 hook 方案 |
| P3-2 | 指令模拟器 | **极高（70,944 B）** | 断点后继续执行 |
| P4 | 多版本编译（5.10~6.18） | 高（需六套内核树） | 分发覆盖 |

---

## 八、结论

### 打平 / 领先的维度

- **核心内存读写**：真机四判据全绿、900.6 MB/s、双通道设计 —— **不落下风**
- **体积**：38 KB vs AFEI 157 KB —— **本轮实现反超**
- **隐蔽性**：三处全隐藏 —— **本轮已打平**
- **通道灵活性**：双通道可切 —— **优于 AFEI 的单通道**

### 仍落后的维度

- **功能广度**：8 项 vs 18 项，缺 10 项能力
- **技术深度**：指令模拟器 70,944 B 的护城河，我方完全空白
- **工程化覆盖**：1 个内核版本 vs 6 个

### 一句话定性

> **本轮把"根本不该输的两项"（体积、默认隐蔽）补上了，核心内存能力本就不输。**
> **剩下的差距集中在"功能广度"和"指令模拟"这一块硬骨头** ——
> 前者是堆工作量的活，后者是需要长期投入的深水区。

**当前状态：内存引擎这条线已可正面对标 AFEI；完整对标需补齐 P1~P3。**

---

## 附：本轮产物与真机命令

### 产物

| 文件 | 大小 | 说明 |
|---|---|---|
| `rwProcMem_default_patched.ko` | 308,272 B | 带调试信息 + CRC 已补 |
| **`rwProcMem_slim.ko`** | **38,056 B** | **strip 版，推荐分发** |
| `hwbp_patched.ko` | 352,312 B | bp 带调试信息 + CRC 已补 |
| **`hwbp_slim.ko`** | **46,256 B** | **strip 版，推荐分发** |

### 命令

```bash
# 源码同步 → 编译
python vm_putrw33.py
python build_rwp.py

# CRC 补丁（真机加载必需）
python ext_crc.py patch <in.ko> ext_crc_index.json <out.ko>

# strip（体积优化）
llvm-strip --strip-debug <patched.ko>

# 真机验证
adb push rwProcMem_slim.ko /data/local/tmp/rw_slim.ko
su -c 'insmod /data/local/tmp/rw_slim.ko'
su -c 'lsmod | grep -i rwProcMem'        # 应为空（已隐藏）
su -c /data/local/tmp/rwtest             # 四判据应全绿
```
