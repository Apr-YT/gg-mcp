# rwProcMem33 驱动接口适配文档

> 来源：https://github.com/abcz316/rwProcMem33
> 用途：替换 QX 驱动，提供无密钥门控的内存读写 + 硬件断点能力
> 适配目标：`agentd/driver.c` 的 `gg_driver_t` 抽象层 + `agentd/watch.c` 的硬件断点

---

## 一、整体架构

rwProcMem33 包含**两个独立的 LKM 内核模块**：

| 模块 | 功能 | 源文件目录 |
|---|---|---|
| rwProcMem_module | 进程内存读写 + 进程枚举 + 内存映射 | `rwProcMem33Module/rwProcMem_module/` |
| hwBreakpointProc_module | 硬件断点/监视点 + 命中回调 | `hwBreakpointProcModule/hwBreakpointProc_module/` |

两个模块各自创建独立的 proc 文件节点，互不依赖，可以只加载其中一个。

---

## 二、调用方式（关键：不是 ioctl，是 proc read）

### 2.1 设备节点

```
/proc/<AUTH_KEY>/<AUTH_KEY>
```

- `<AUTH_KEY>` 是编译时宏 `CONFIG_PROC_NODE_AUTH_KEY`，相当于访问密码，可自定义
- 权限 `S_IRUGO | S_IWUGO`（所有人可读可写）
- 模块加载后自动创建，同时调用 `start_hide_procfs_dir()` 隐藏 proc 目录项

### 2.2 请求格式

用户态 `open()` 节点后，用 **`read()` 系统调用**发送请求（非常规用法：read 的 buf 同时是输入和输出）：

```c
#pragma pack(push, 1)
struct ioctl_request {
    char     cmd;         // 1 字节命令码（见下方枚举）
    uint64_t param1;      // 参数1（含义随 cmd 变化）
    uint64_t param2;      // 参数2
    uint64_t param3;      // 参数3
    uint64_t buf_size;    // 紧随头部的动态数据长度（读/写的数据字节数）
};
#pragma pack(pop)
// 总大小 = 33 字节（packed）
```

**调用流程**：
```c
// 1. 准备缓冲区：头部 + 数据区
uint8_t buf[sizeof(struct ioctl_request) + data_size];
struct ioctl_request *req = (struct ioctl_request *)buf;
req->cmd = CMD_READ_PROCESS_MEMORY;
req->param1 = (uint64_t)proc_handle;   // 进程句柄
req->param2 = addr;                    // 虚拟地址
req->param3 = 0;                       // is_force_read (0=否, 1=是)
req->buf_size = read_len;              // 要读的字节数

// 2. read() 把请求发给内核，内核处理后把结果写回同一个 buf
ssize_t result_len = read(fd, buf, sizeof(buf));
// 返回值 = 实际读取/写入的字节数；负值 = errno
// 结果数据在 buf + sizeof(struct ioctl_request) 处
```

**注意**：`read()` 的返回值就是操作结果长度。读内存时，结果数据紧跟在头部之后（覆盖原数据区）。

---

## 三、内存读写模块（rwProcMem_module）

### 3.1 命令码枚举

```c
enum {
    CMD_INIT_DEVICE_INFO    = 1,   // 初始化设备信息（必须第一个调用！）
    CMD_OPEN_PROCESS        = 2,   // 打开进程，返回句柄
    CMD_READ_PROCESS_MEMORY = 3,   // 读内存
    CMD_WRITE_PROCESS_MEMORY= 4,   // 写内存
    CMD_CLOSE_PROCESS       = 5,   // 关闭进程
    CMD_GET_PROCESS_MAPS_COUNT = 6,// 获取内存块数量
    CMD_GET_PROCESS_MAPS_LIST  = 7,// 获取内存块列表
    CMD_CHECK_PROCESS_ADDR_PHY = 8,// 检查地址是否有物理映射
    CMD_GET_PID_LIST        = 9,   // 获取进程PID列表
    CMD_SET_PROCESS_ROOT    = 10,  // 提升进程权限到root
    CMD_GET_PROCESS_RSS     = 11,  // 获取进程物理内存占用
    CMD_GET_PROCESS_CMDLINE_ADDR = 12, // 获取cmdline内存地址
    CMD_HIDE_KERNEL_MODULE  = 13,  // 隐藏驱动（/proc/modules 中消失）
};
```

### 3.2 各命令参数详解

#### CMD_INIT_DEVICE_INFO（必须首次调用）

初始化内核结构体偏移自动纠正机制。

- **param1/2/3**: 未使用
- **buf_size**: `sizeof(struct init_device_info)`
- **输入数据**：
```c
struct init_device_info {
    int  pid;                          // 当前调用者进程 pid
    int  tgid;                         // 当前调用者进程 tgid
    char my_name[17];                  // 当前进程 comm（task_struct->comm）
    char my_auxv[1024];                // 当前进程 auxv 数据
    int  my_auxv_size;                 // auxv 有效长度
};
```
- **返回**: 0 成功，负值 errno

> 这一步会自动探测 `mm_struct`、`vm_area_struct`、`task_struct` 等内核结构体的字段偏移，支持不同内核版本。

#### CMD_OPEN_PROCESS

- **param1**: 目标进程 pid
- **返回**: 进程句柄（`struct pid *` 指针，作为后续命令的 param1），写回 buf 前 8 字节

#### CMD_READ_PROCESS_MEMORY

- **param1**: 进程句柄（来自 OPEN_PROCESS）
- **param2**: 目标虚拟地址
- **param3**: `is_force_read`（0=检查PTE权限，1=临时改PTE强制读）
- **buf_size**: 要读取的字节数
- **返回**: 实际读取字节数；数据写回 buf + 33 处

#### CMD_WRITE_PROCESS_MEMORY

- **param1**: 进程句柄
- **param2**: 目标虚拟地址
- **param3**: `is_force_write`（0=检查权限，1=强制写）
- **buf_size**: 要写入的字节数
- **输入数据**: 要写入的原始字节（紧跟头部）
- **返回**: 实际写入字节数

#### CMD_CLOSE_PROCESS

- **param1**: 进程句柄
- **返回**: 0

#### CMD_GET_PROCESS_MAPS_COUNT

- **param1**: 进程句柄
- **返回**: 内存区域数量（int）

#### CMD_GET_PROCESS_MAPS_LIST

- **param1**: 进程句柄
- **buf_size**: 输出缓冲区大小
- **返回**: 实际写入字节数；数据为内存区域数组（格式由 `proc_maps.h` 定义，需参考源码）

#### CMD_CHECK_PROCESS_ADDR_PHY

- **param1**: 进程句柄
- **param2**: 虚拟地址
- **返回**: 1=有物理映射，0=无

#### CMD_GET_PID_LIST

- **buf_size**: 输出缓冲区大小
- **返回**: 实际写入字节数；PID 列表数据

#### CMD_GET_PROCESS_RSS

- **param1**: 进程句柄
- **返回**: RSS 值（uint64），写回 buf 前 8 字节

#### CMD_HIDE_KERNEL_MODULE

- 无参数
- 从 `/proc/modules` 和 sysfs 中移除自身，卸载需重启

### 3.3 内存读写原理

**不是** `process_vm_readv`，而是**手动页表遍历 + 物理内存直读**：

1. `get_proc_phy_addr()`：遍历目标进程页表，将虚拟地址转换为物理地址
2. `read_ram_physical_addr()` / `write_ram_physical_addr()`：直接读写物理 RAM
3. `is_force_read/write`：如果 PTE 没有对应权限，临时修改 PTE 权限位，操作后恢复

这种方式完全不经过 `process_vm_readv` 系统调用，也不附加到目标进程，**最难被检测**。

---

## 四、硬件断点模块（hwBreakpointProc_module）

### 4.1 命令码枚举

```c
enum {
    CMD_OPEN_PROCESS        = 1,   // 打开进程
    CMD_CLOSE_PROCESS       = 2,   // 关闭进程
    CMD_GET_NUM_BRPS        = 3,   // 获取CPU支持的硬件执行断点数量
    CMD_GET_NUM_WRPS        = 4,   // 获取CPU支持的硬件访问断点数量
    CMD_INST_PROCESS_HWBP   = 5,   // 安装硬件断点
    CMD_UNINST_PROCESS_HWBP = 6,   // 卸载硬件断点
    CMD_SUSPEND_PROCESS_HWBP= 7,   // 暂停断点
    CMD_RESUME_PROCESS_HWBP = 8,   // 恢复断点
    CMD_GET_HWBP_HIT_COUNT  = 9,   // 获取命中数量
    CMD_GET_HWBP_HIT_DETAIL = 10,  // 获取命中详情
    CMD_SET_HOOK_PC         = 11,  // 设置无条件Hook跳转
    CMD_HIDE_KERNEL_MODULE  = 12,  // 隐藏驱动
};
```

### 4.2 关键结构体

```c
#pragma pack(1)
struct my_user_pt_regs {
    uint64_t regs[31];     // x0-x30
    uint64_t sp;           // 栈指针
    uint64_t pc;           // 程序计数器（命中地址）
    uint64_t pstate;       // 程序状态寄存器
    uint64_t orig_x0;      // 原始 x0（系统调用时）
    uint64_t syscallno;    // 系统调用号
};

struct HWBP_HIT_ITEM {
    uint64_t task_id;              // 命中线程 tid
    uint64_t hit_addr;             // 命中地址
    uint64_t hit_time;             // 命中时间（内核时间戳）
    struct my_user_pt_regs regs_info; // 命中时刻完整寄存器
};
#pragma pack()
```

### 4.3 各命令参数详解

#### CMD_OPEN_PROCESS

- **param1**: 目标进程 pid
- **返回**: 进程句柄

#### CMD_GET_NUM_BRPS / CMD_GET_NUM_WRPS

- **param1**: 进程句柄
- **返回**: 断点槽位数量（int），ARM64 通常 BRPS=6, WRPS=4

#### CMD_INST_PROCESS_HWBP

- **param1**: 进程句柄
- **param2**: 断点虚拟地址
- **param3**: 断点类型（需参考 `hwbp_proc.c` 确认具体编码，通常：1=执行, 2=写, 3=读写）
- **buf_size**: 可能为 0 或额外配置数据
- **返回**: 断点 ID（int），用于后续 UNINST/SUSPEND/RESUME

#### CMD_UNINST_PROCESS_HWBP

- **param1**: 进程句柄
- **param2**: 断点 ID
- **返回**: 0

#### CMD_SUSPEND_PROCESS_HWBP / CMD_RESUME_PROCESS_HWBP

- **param1**: 进程句柄
- **param2**: 断点 ID
- **返回**: 0

#### CMD_GET_HWBP_HIT_COUNT

- **param1**: 进程句柄
- **param2**: 断点 ID
- **返回**: 命中总数（size_t）

#### CMD_GET_HWBP_HIT_DETAIL

- **param1**: 进程句柄
- **param2**: 断点 ID
- **param3**: 起始索引（从第几条开始取）
- **buf_size**: 输出缓冲区大小（应为 `N * sizeof(HWBP_HIT_ITEM)`）
- **返回**: 实际写入的 `HWBP_HIT_ITEM` 数量 × 结构体大小

#### CMD_SET_HOOK_PC

- 设置无条件 Hook 跳转（高级功能，需参考源码确认参数）

### 4.4 硬件断点原理

使用 Linux 内核标准 **`perf_event` 框架**：
- `register_user_hw_breakpoint()` 注册断点
- `perf_overflow_handler_t` 回调中保存寄存器和命中信息
- 支持 `kallsyms` 动态查找（`CONFIG_KALLSYMS_LOOKUP_NAME`），兼容符号被裁剪的内核

> 注意：5.x 以上内核需要**去除 CFI 验证**，否则加载会死机（仓库目录 `5.x内核需去除cfi验证，否则会死机` 中有补丁说明）。

---

## 五、适配到 gg 项目的方案

### 5.1 driver.c 新增后端 `KO_TYPE_RWPROC`

```
gg_driver_t 新增字段:
  int rwproc_mem_fd;      // rwProcMem 模块的 proc 文件 fd
  int rwproc_bp_fd;       // hwBreakpoint 模块的 proc 文件 fd
  uint64_t rwproc_handle; // 当前附加进程的句柄
```

**driver_read 实现**：
```
1. 构造 ioctl_request { cmd=3, param1=handle, param2=addr, param3=force, buf_size=len }
2. read(rwproc_mem_fd, buf, 33+len)
3. 返回值 = 实际读取字节数，数据在 buf+33
```

**driver_write 实现**：
```
1. 构造 ioctl_request { cmd=4, param1=handle, param2=addr, param3=force, buf_size=len }
2. 把待写数据拷贝到 buf+33
3. read(rwproc_mem_fd, buf, 33+len)
4. 返回值 = 实际写入字节数
```

**attach 流程**：
```
1. open("/proc/<key>/<key>") → rwproc_mem_fd
2. CMD_INIT_DEVICE_INFO（传入当前进程 pid/tgid/comm/auxv）
3. CMD_OPEN_PROCESS(param1=目标pid) → 得到 handle
4. 保存 handle 到 gg_driver_t
```

### 5.2 watch.c 适配

当前 watch.c 有两条路径：QX 驱动硬件断点 + ptrace 降级。新增第三条路径 `WATCH_RWPROC`：

```
watch_start:
  1. open 硬件断点模块 proc 文件
  2. CMD_OPEN_PROCESS(pid) → bp_handle
  3. CMD_INST_PROCESS_HWBP(bp_handle, addr, type) → bp_id
  4. 保存 bp_handle + bp_id

watch_poll:
  1. CMD_GET_HWBP_HIT_COUNT(bp_handle, bp_id) → count
  2. CMD_GET_HWBP_HIT_DETAIL(bp_handle, bp_id, 0, buf, count*sizeof(HWBP_HIT_ITEM))
  3. 把 HWBP_HIT_ITEM 转换为 gg_watch_hit_t（tid=task_id, pc=regs_info.pc, r0-r4=regs_info.regs[0-4]）
  4. 补充 fp=regs_info.regs[29], sp=regs_info.sp, lr=regs_info.regs[30]
```

**优势**：命中信息自带**完整 31 个寄存器 + sp + pc + pstate**，比 QX 驱动和 ptrace 都丰富，AI 分析汇编时可用的上下文更多。

### 5.3 加载流程

```
1. insmod rwProcMem_module.ko
2. insmod hwBreakpointProc_module.ko
3. （可选）CMD_HIDE_KERNEL_MODULE 隐藏两个模块
4. agentd 自动发现 /proc/<key>/<key> 节点
```

两个模块的 `CONFIG_PROC_NODE_AUTH_KEY` 建议在编译时改成你自己的字符串，避免和其他使用 rwProcMem33 的工具冲突。

---

## 六、编译注意事项

1. **内核版本适配**：官方支持到 6.1.75，你的设备是 6.12.69，需要：
   - 检查 `linux_kernel_api.h` 中的内核 API 封装是否兼容 6.12
   - `*_auto_offset.h` 自动偏移纠正机制应该能处理大部分差异
   - `ver_control.h` 中的版本判断可能需要扩展

2. **CFI 问题**：5.x+ 内核需要关闭 CFI，或使用仓库提供的 `__cfi_check_fn` / `__cfi_check_fail` hook（代码中已有 `CONFIG_MODULE_GUIDE_ENTRY` 条件编译）

3. **工具链**：需要对应内核版本的 `aarch64-linux-android-` 交叉编译器，NDK r27d 应该可用

4. **proc 文件节点名**：`CONFIG_PROC_NODE_AUTH_KEY` 是编译宏，建议改成自定义值，两个模块用不同的 key 避免冲突

---

## 七、与 QX 驱动的对比

| 特性 | QX 驱动 | rwProcMem33 |
|---|---|---|
| 内存读写 | ioctl 0x801/0x802 | proc read + 页表遍历直读物理内存 |
| 硬件断点 | 0x807-0x811，**需 init_key 门控** | perf_event 框架，**无密钥门控** |
| 隐藏方式 | 隐藏进程 + 已删除节点 | 隐藏 proc 目录 + 隐藏内核模块 |
| 命中寄存器 | r0-r4（有限） | x0-x30 + sp + pc + pstate（完整） |
| 强制读写 | 不支持 | 支持（临时改 PTE 权限） |
| 开源 | 否（二进制分发） | 是（完整源码） |
| 内核版本 | 适配特定版本 | 自动偏移纠正，适配范围更广 |

---

## 八、文件清单（需要从仓库获取的源码）

### 内存读写模块
```
rwProcMem33Module/rwProcMem_module/
├── rwProcMem_module.c      // 主入口 + 命令分发
├── rwProcMem_module.h      // 接口定义 + 命令枚举
├── api_proxy.h             // 内核 API 封装
├── linux_kernel_api.h      // 内核版本兼容
├── phy_mem.h               // 物理内存读写 + 页表遍历
├── phy_mem_auto_offset.h   // 物理内存偏移自动纠正
├── proc_maps.h             // 内存映射解析（71KB，最大文件）
├── proc_maps_auto_offset.h // 映射偏移自动纠正
├── proc_list.h             // 进程列表
├── proc_list_auto_offset.h
├── proc_root.h             // 进程根目录
├── proc_root_auto_offset.h
├── proc_cmdline.h          // cmdline 解析
├── proc_cmdline_auto_offset.h
├── proc_rss.h              // RSS 统计
├── ver_control.h           // 版本控制
├── hide_procfs_dir.h       // proc 目录隐藏
├── test.h                  // 自测
└── Makefile
```

### 硬件断点模块
```
hwBreakpointProcModule/hwBreakpointProc_module/
├── hwBreakpointProc_module.c  // 主入口 + 命令分发
├── hwBreakpointProc_module.h  // 接口定义 + 命令枚举 + 命中结构体
├── hwbp_proc.c                // 硬件断点核心实现（23KB）
├── hwbp_proc.h
├── api_proxy.h                // perf_event API 封装
├── arm64_register_helper.h    // ARM64 寄存器操作
├── cvector.h                  // 动态数组（命中列表）
├── kallsyms_lookup_api.h      // kallsyms 动态查找
├── proc_pid.h                 // pid 查找
├── ver_control.h
├── hide_procfs_dir.h
├── anti_ptrace_detection.h    // 反 ptrace 检测
└── Makefile
```

---

## 九、实现状态（2026-08-16）

已完成（对照 §5 方案）：

- [x] **driver.c 后端 `KO_TYPE_RWPROC`**：`driver_init_rwproc` / `rwproc_attach`
      （open 两节点 + CMD_INIT_DEVICE_INFO）/ `rwproc_open_process`（句柄缓存, 换 pid
      自动关旧开新）/ `driver_read/write`（proc read 协议, 单次上限 256KB=GG_SEARCH_CHUNK）/
      `driver_self_test`（回环读自身）。
- [x] **watch.c 第三条路径 `WATCH_RWPROC`**（`use_driver=2`）：`rwproc_hwbp_start`
      （CMD_OPEN_PROCESS + CMD_INST_PROCESS_HWBP, param3=`len|(type<<8)`）/
      `rwproc_hwbp_get_hits`（COUNT + DETAIL, 转 gg_watch_hit_t, 补齐 fp/sp/lr）/
      `rwproc_hwbp_stop`（UNINST）。命中累计不清零（模块无清除命令, 上限 1024）,
      poll 按 `rwproc_hit_consumed` 去重只上报新命中。
- [x] **main.c**：`--driver rwproc` + `--rwproc-key`/`--rwproc-bp-key`；start_agentd.sh
      增加 `RWPROC_MEM_KEY`/`RWPROC_BP_KEY`。
- [x] NDK arm64 编译通过，agentd 二进制含 rwproc 后端。

源码核对的协议修正（与上游仓库一致, 比 §2 文档更精确）：

1. **句柄/结果位置**：OPEN_PROCESS/INST_HWBP 的返回值（handle/perf_event 指针）
   写回 **buf+33**（数据区起点），read() 返回值恒为 0（成功）。
2. **INST_PROCESS_HWBP 参数**：param3 = `len | (type << 8)`（低字节 len, 次字节 type）；
   type 用 perf 的 HW_BREAKPOINT_*（R=1/W=2/RW=3/X=4, 与 HW_BP_TYPE_* 一致）。
3. **UNINST/COUNT/DETAIL 的 param1 = 断点 ID（perf_event 指针）**，不是进程句柄。
4. **命中条目** `HWBP_HIT_ITEM`（packed, sizeof 312）：task_id/hit_addr/hit_time +
   my_user_pt_regs（regs[31]+sp+pc+pstate+orig_x0+syscallno, sizeof 288）；`hit_addr` 实现里
   = 命中指令 pc。
5. **GET_HWBP_HIT_COUNT** 返回 `{hit_total_count, hit_item_arr_count}` 16 字节到数据区。
6. **无清除命令**：命中列表累计至 MIN_LEN=1024，需调用方去重。

## 十、真机落地（REDMI K90 Pro Max, 6.12.69-android16-6, 2026-08-16 晚）

### 构建（VM: Linux Mint 22.3, 8 核, clang-18 + NDK 工具链）

1. 克隆 GKI `android16-6.12`（googlesource）→ `SUBLEVEL=69` +
   `CONFIG_LOCALVERSION="-android16-6-4k"` + 关 LOCALVERSION_AUTO
   → vermagic **精确匹配**设备 `6.12.69-android16-6-4k SMP preempt mod_unload modversions aarch64`
   （免二进制补丁）。
2. `make gki_defconfig` → `vmlinux`（生成 vmlinux.symvers → cp 为 Module.symvers）。
   坑：Windows checkout 行尾 CRLF（PowerShell Set-Content 也会引入）→ VM 内 `sed -i 's/\r$//'`；
   大小写撞名文件（xt_DSCP.c 等 13 对）Windows checkout 丢一半 → `git archive` 补回。
3. 模块编译修正：
   - `__kmalloc`→`kmalloc`（6.x 不再对模块暴露）、`strlcpy`→`strscpy`（6.12 移除）
   - `MY_LINUX_VERSION_CODE` = **6.6.30**（作者最新分支, proc_maps.h 有 6.6.30 专属实现；
     6.1.75 分支与 6.12 头不兼容, 6.12.0 无对应门控分支）
   - 禁用 hide_procfs_dir（kprobe hook 崩溃风险, 置空 hide_procfs_dir.h + 注释调用）

### 加载（两个关键坑）

1. **MODVERSIONS CRC**：Xiaomi 改动内核结构体 → 与 vanilla GKI 的符号 CRC 大面积不符。
   从设备**全部厂商模块**的 `__versions` 建符号→CRC 映射, 批量修补自编译 .ko
   （`scripts/patch_ko_versions.py`；module_layout 等约 24 个符号被改写,
   `copy_from_kernel_nofault` 无结构体依赖天然匹配）。
2. **KCFI 崩溃（insmod 后 5-7 分钟内随机重启）**：设备内核
   `CONFIG_CFI_ICALL_NORMALIZE_INTEGERS=y`, 但 gki_defconfig 默认**不开** →
   模块 KCFI 类型哈希不匹配 → 内核间接调用模块函数即 panic。
   **必须 `scripts/config --enable CFI_ICALL_NORMALIZE_INTEGERS` + `syncconfig`**
   （注意：仅 olddefconfig 不会同步进 auto.conf！）→ 重建后 `-fsanitize-cfi-icall-experimental-normalize-integers`
   进编译行。另外 agentd 侧 rwproc 读写改 **is_force=0**（不翻转 PTE, 避免与目标进程活跃页竞争）。

### 当前状态

- [x] rwProcMem_module 在 6.12.69 加载成功（Live），`--driver rwproc` 自检
      `kernel-driver (kernel module)`
- [x] 真机读/写/查看/符号解析/指针链/反汇编全链路通过（含 QX 读不了的文件页: libc ELF 头直读）
- [x] 稳定运行（hide 禁用 + force=0 后持续存活）
- [x] **hwBreakpointProc_module 已修复（2026-08-17）**：`gg_watch_set`（INST_PROCESS_HWBP）内核崩溃
      根因定位并修复（见下方 §10.4），重建后 INST/COUNT/DETAIL/UNINST 全流程可安全运行
- [ ] QX 驱动（/data/kagtim 等）随重启消失, 需用户按原流程重新拉起
- [ ] 搜索性能：rwproc 逐页物理读, system_server 全匿名区扫描极慢（>5min）,
      建议目标进程用游戏（足迹小）或缩小 region

## 10.4 hwbp 崩溃根因与修复（2026-08-17，已解决 ✅）

**根因：命令码枚举错位（cmd 整体偏移 +1）**，不是内核 API/命中回调问题！

模块头文件 `hwBreakpointProc_module.h` 原命令枚举**从 0 开始**（`CMD_OPEN_PROCESS=0, CMD_CLOSE_PROCESS=1, ...`），而用户态（agentd `driver.h` 的 `RWBP_CMD_*`、上游协议、bptest）**按 1 起始**发送。结果：

| 用户态发送 | 模块实际执行 | 结果 |
|---|---|---|
| cmd=1（以为 OPEN） | CMD_CLOSE_PROCESS → `put_pid((struct pid*)pid)` | **解引用用户态整数 → 内核数据异常 → 整机静默挂死/看门狗重启**（gg_watch_set 必崩的真因） |
| cmd=4（以为 WRPS） | CMD_INST_PROCESS_HWBP → `pid_nr(坏指针)` | 同上 |
| cmd=2（以为 CLOSE） | CMD_GET_NUM_BRPS | 返回 6（设备 BRPS=6，"意外正常"） |
| cmd=3（以为 BRPS） | CMD_GET_NUM_WRPS | 返回 4（WRPS=4，"意外正常"） |
| cmd=0（裸读全零） | CMD_OPEN_PROCESS → `find_get_pid(0)`=NULL | 返回 -EINVAL（**误导性"正常"**，掩盖了错位） |

**定位过程**：bptest 逐个命令测试发现 cmd=1、cmd=4 必崩而 cmd=2/3 返回异常值（6/4 与预期相反）→ 反汇编确认 cmd=N 分支指向 cmd=N+1 代码 → 对比枚举发现 0 起始 vs 1 起始错位。

**修复**：`hwBreakpointProc_module.h` 枚举显式 `= 1` 起始（`CMD_OPEN_PROCESS=1 ... CMD_HIDE_KERNEL_MODULE=12`），与 agentd/上游一致。**agentd 无需改动**。

**附带防御性修复**（排查中发现并一并处理）：
1. **命中回调原子上下文违规**：原回调在 ARM64 调试异常上下文（中断/抢占关闭）里 `mutex_lock` + `cvector_pushback`(vmalloc) + `modify_user_hw_breakpoint`（perf_event_disable→ctx->mutex）→ "scheduling while atomic" BUG。改为固定槽位数组 + raw_spinlock + INST 时预分配命中缓冲，回调只 memcpy。
2. **命中后必须清硬件槽 E 位**（所有类型 W/R/RW/X）：ARM64 监视点/断点命中返回后指令会重新执行，不清 E 位 → 无限调试异常循环 → 整机挂死。内核在任务切回时按 perf 事件重新武装，故为"每次切回可持续捕获"语义（与上游 #else 一致）。
3. 禁用 `CONFIG_MODIFY_HIT_NEXT_MODE` / `CONFIG_ANTI_PTRACE_DETECTION_MODE` / hide_procfs_dir kprobe。
4. `x_kmalloc` 用 `kmalloc`（6.12 不再暴露 `__kmalloc`）、`MY_LINUX_VERSION_CODE=6.6.30`（作者最新分支，真机验证）。

**真机验证（生产版，无埋点）**：cmd=1/3/4 全正常；`gg_attach → gg_watch_set → gg_watch_poll`（64 条命中，含模块归属/FP 链回溯/±32 条预反汇编）`→ gg_watch_clear` 全流程通过，设备持续稳定。

**修复（build/rwProcMem33/hwBreakpointProcModule/hwBreakpointProc_module/）**：

1. **存储重构**：`cvector + mutex` → 固定槽位数组 `g_hwbp_handles[16]` + `raw_spinlock_t`
   （IRQ 安全）。命中缓冲 `hit_arr` 在 INST 时（进程上下文）一次性 `vmalloc` 预分配
   `HWBP_HIT_CAP=1024` 条（`sizeof(HWBP_HIT_ITEM)=312` → 约 312KB），回调里只做 memcpy。
2. **回调重写** `hwbp_handler`：`raw_spin_lock_irqsave` → 找句柄 → `hit_total_count++` →
   写入预分配数组 →（X 执行断点才）`toggle_bp_registers_directly(...,0)` 清 E 位防重入；
   W/R/RW 监视点天然可重复命中，无需任何 hack。锁内零分配、零睡眠。
3. **ver_control.h**：禁用 `CONFIG_MODIFY_HIT_NEXT_MODE`、`CONFIG_ANTI_PTRACE_DETECTION_MODE`；
   开启 `CONFIG_DEBUG_PRINTK`（诊断期看日志）。
4. **hide_procfs_dir**：不再 include/调用（proc 节点会显示在 /proc，key 随机串，可接受）。
5. **SUSPEND/RESUME**：改为句柄 `suspended` 标志（锁保护，回调跳过记录），不再走
   `modify_user_hw_breakpoint`。协议与 agentd 完全兼容，agentd 无需改动。
6. **内存安全细节**：`vmalloc`/`vfree`/`x_unregister_hw_breakpoint` 一律在锁外；
   DETAIL 两段式（锁外分配临时缓冲 → 锁内 memcpy → 锁外 copy_to_user）。
7. 保留 `CONFIG_KALLSYMS_LOOKUP_NAME`（kprobe 取 kallsyms_lookup_name 找
   `register_user_hw_breakpoint`/`unregister_hw_breakpoint`，本设备已确认可用，符号存在）。

**重建 + 部署 + 验证（Linux VM）**：

```bash
# 1) 重建（脚本见 scripts/build_rwproc_ko.sh；KERN_SRC 为 VM 里已配好的 6.12 内核树）
KERN_SRC=/path/to/android16-6.12 DEV_KO_DIR=/path/to/device_vendor_kos \
  bash scripts/build_rwproc_ko.sh
#   产物: build/ko/rwProcMem_module.ko + hwBreakpointProc_module.ko（已 CRC 修补）

# 2) 设备端部署（Windows）
adb push build/ko/hwBreakpointProc_module.ko /data/local/tmp/ggmcp/ko/
adb push build/ko/rwProcMem_module.ko        /data/local/tmp/ggmcp/ko/
adb shell su -c 'sh /data/local/tmp/ggmcp/install_ko.sh'      # insmod 两模块
adb shell su -c 'sh /data/local/tmp/ggmcp/start_agentd.sh start'

# 3) 验证（PC）
#  - gg_kernel_status → state=kernel-driver (kernel module)
#  - 先看内核日志确认 init 正常:
adb shell su -c 'dmesg | grep -E "hwbp:|register_user_hw_breakpoint" | tail -20'
#    期望: hwbp: register_user_hw_breakpoint=0x... unregister=0x...
#          hwbp: init ok (HIT_CAP=1024 MAX_HANDLES=16)
#  - gg_watch_set {addr} → active=1（不再重启）
#  - 触发写 → gg_watch_poll {} → hits 列表（tid/pc/module/stack/disasm）
#  - gg_watch_clear → UNINST 正常
#  - dmesg 每命中一条 "hwbp HIT bp:... pc:..."
```

**如果仍然崩溃**：dmesg 里 `hwbp:` 前缀打印能精确定位到 INST 的哪一步；
重启后从 pstore 取上次 panic 栈：
`adb shell 'ls /sys/fs/pstore/ && cat /sys/fs/pstore/dmesg-ramoops-*'`，
把 panic 调用栈发回来。

**行为变化提醒**：
- SUSPEND/RESUME 语义 = 暂停/恢复"记录"（不断点硬件），返回值 0/成功。
- X 执行断点：**不是一次性** —— 见下方 10.4.1 的纠正。W/R/RW 监视点可重复命中。
- proc 节点不再隐藏（`/proc/dce3771681d4c7a143d5d06b7d32548e/...` 可见）。

### 10.4.1 本机落地（2026-10-21）：补第四条缺陷，纠正「X 断点一次性」

上面 §10.4 是 2026-08-17 在 Linux Mint VM 里做的修复记录。本机（myron / 6.12.69）
的实际情况有两点要补正 —— 完整记录见 `DRIVER_MATRIX.md` §4.5。

**一、仓库里的 `.ko` 不是修好的那份。** `build/ko/hwBreakpointProc_6.12.69.ko`
（afei prebuilt）是上游原版编的：枚举 0 起始、回调用 mutex + cvector、
`DETAIL` 返回条目数而非字节数 —— §10.4 列的三条一条都没修。所以本机必须自编译，
不能直接用它。`scripts/build_hwbp_ko.sh` 负责重建，`deploy.ps1` 自动覆盖部署。

**二、清 E 位不能替代单步挪位 —— 这是第四条缺陷。**
§10.4 的「X 执行断点 = 一次性（命中后清 E 位）」在本机实测站不住：
perf 框架仍认为该 event 有效，命中回调返回后会把断点重新写回 BCR，
于是 PC 停在断点指令上**反复触发**。实测现象是目标进程卡在 `R` 状态、CPU 满载，
不崩机但也回不来（`kill -9` 才停）。

不能用 `CONFIG_MODIFY_HIT_NEXT_MODE` 解：它的
`arm64_move_bp_to_next_instruction` 调 perf 的 `modify_user_hw_breakpoint`，
会拿 mutex，在调试异常上下文里必然 `scheduling while atomic`。

正解是在回调里**直接写 BVR**：

```c
/* 首命中：记录后把断点挪到 pc+4，让原指令能执行 */
set_bp_addr_directly(&hi->original_attr, hi->is_32bit_task, regs->pc, regs->pc + 4);
/* 在 pc+4 再命中：挪回原地址，这一次不记录 */
set_bp_addr_directly(&hi->original_attr, hi->is_32bit_task, ng->bp_addr, hi->original_attr.bp_addr);
```

`set_bp_addr_directly` 只做 `read_wb_reg` / `write_wb_reg` 找槽位改地址，
不碰 perf 框架。数据断点命中时 PC 会正常前进，不需要挪位，只记录。

**三、`struct module` 必须是 1600 字节，只有完整 GKI 树能做到。**
vanilla 6.12 = 1280、vanilla 换 GKI 的 `module.h` = 1344，都不够。
设备真值 1600 从两处独立获得：能正常加载的 `rwProcMem_6.12.69.ko` 的
`.gnu.linkonce.this_module` 段，以及设备 BTF 里 `struct module` 的 size。
用完整 `android16-6.12` 源码 + 设备真值 config 才能对上（差的那 256 字节受
GKI 特有 CONFIG 影响，不是换头文件能补的）。

**四、版本表为空不影响加载。** 没编过 `vmlinux` 就没有 `Module.symvers`，
`__versions` / `__version_ext_crcs` 为空，实测仍能 insmod 并正常工作 ——
内核按名字解析未定义符号，空版本表只警告不拒绝。附带修掉上游 modpost 的
空表缺陷（会生成 `static const char x[] = ;`）。

**五、部署时的隐藏坑。** `CONFIG_HIDE_BY_DEFAULT` 把模块从内核链表摘除，于是
`rmmod` 永久失败（换模块只能重启），且 `insmod` 同名模块**不会被拒** ——
后加载的会顶掉前一个的 `/proc/<KEY>` 节点。判断当前跑的是哪版：
发 `cmd=3`，返回 6 是自编译版，返回 4 是 afei 的 0 起始版。

---


## 十一、跨内核复用 prebuilt（6.12.23 → 6.12.69，2026-10-21 验证）

**场景**：`gitee.com/wei-yetao/rwProcMem33-afei` 的 `prebuilt/` 是给 **6.12.23（K90）** 编的，
本机 **myron 是 6.12.69**。直接 insmod 报 `Exec format error`。
在**没有内核源码树**的 Windows 上，可以不重编就让它跑起来，只需两步。

### 第一步：vermagic 对齐

设备的**精确** vermagic 不要从 `uname -r` 抄 —— 两者不一致：

```
uname -r                 6.12.69-android16-6-g43b5ab7e247d-abogki561813157-4k
厂商模块 vermagic=        6.12.69-android16-6-4k SMP preempt mod_unload modversions aarch64
```

内核 `check_modinfo()` 比的是**编译期 UTS_RELEASE**（`-android16-6-4k`，关了 LOCALVERSION_AUTO），
所以必须从设备自带的厂商模块读真值：

```bash
adb exec-out "su -c 'cat /vendor/lib/modules/adsp_loader_dlkm.ko'" > vendor_sample.ko
strings vendor_sample.ko | grep vermagic=
```

替换方法（**新串与旧串长度通常不等**，用 NUL 填充到原长度，`strcmp` 在首个 NUL 结束）：

```python
i = data.find(b'vermagic='); j = data.find(b'\0', i)
old = data[i+9:j]
new = b'6.12.69-android16-6-4k SMP preempt mod_unload modversions aarch64'
out = data[:i+9] + new + b'\0'*(len(old)-len(new)) + data[j:]   # 总长不变
```

### 第二步：CRC 修补

设备开 `CONFIG_EXTENDED_MODVERSIONS`，Xiaomi 又改过内核结构体，符号 CRC 与 vanilla GKI 大面积不符。
拉设备全部厂商模块作为映射源：

```bash
adb exec-out "su -c 'tar cf - -C /vendor/lib/modules .'" > build/devmod/modules.tar
cd build/devmod/all && tar xf ../modules.tar
python scripts/patch_ko_versions.py build/devmod/all build/ko/apr_dlkm.ko
```

本机实测：402 个模块 → 4547 个符号映射；`rwProcMem_slim` 修补 19 处、`module_layout`
`0xe976b219 → 0x797f2b3e`，0 missing。

★ **只改 `__versions` 是没用的**：内核 `check_version()` 优先读
`__version_ext_names` + `__version_ext_crcs`，旧式段只作回退。原脚本只处理前者，
在 ext 内核上仍报 `disagrees about version of symbol module_layout`。
`patch_ko_versions.py` 已扩展为两种都改（ext 优先建映射）。

### 结果（本机 6.12.69）

| 步骤 | 结果 |
|---|---|
| `apr_dlkm.ko`（rwProcMem slim）insmod | RC=0，dmesg `module hidden by default (init)` |
| `hwBreakpointProc_module.ko` insmod | RC=0 |
| `/proc/<MEM_KEY>/<MEM_KEY>`、`/proc/<BP_KEY>/<BP_KEY>` | 均就绪 |
| `gg_kernel_status` | `kernel-driver (kernel module)` / `ko_module=rwproc33` / `error` 空 |
| `attach(pid 1)` + `read_bytes` | `7f454c46…`（init 的 ELF 头） |
| `run_itest.ps1` | ITEST ALL PASS |

### 风险与遗留

- **未经内核源码重编**，`struct module` 布局只靠 CRC 对齐 + 运行验证，没有形式化保证。
  §十 提过 KCFI 不匹配会导致 **insmod 后 5–7 分钟随机重启**，本次加载后需持续观察。
- 复用前提是**同 major.minor 分支**（都 android16-6.x）；跨分支（5.x ↔ 6.x）不要尝试。
- 这套流程只解决"能用"，不等于"可维护"：真出问题仍要回到 §十 的源码树重编路径。

---

## 十二、⚠️ 已知故障：rwproc 全量搜索会导致内核崩溃（2026-10-21 实测）

> **2026-10-22 更新：根因已定位，本节末尾的「下一步排查」不必再做。**
> 四条原因（物理地址上限用 `__pa(字节数)` 算错、不判 pmd 大页、fast 路径缺 `virt_addr_valid`、
> 页表遍历在 `mmap_lock` 外）与修复见 `docs/audit/RWPROC33_2026-10-22.md`。
>
> **2026-10-22 晚间再更新（真机已验）**：修复版已在设备 `134d2f8` / 6.12.69 上跑起来
> —— `insmod` RC=0、加载后 6h16m 未重启、dmesg 零 Oops，`[rwfast] phys range invalid`
> 确实拦下了 39.3GB 的非法物理地址（旧代码会越界 vmemmap 崩机）。**崩机这条故障已止住。**
> 但读写链路尚未打通：`driver_self_test` 回环读返回 0，算出的 39GB 来自页表项本身
> （VA_BITS=39 / 3 级页表 / BTF 偏移已逐项核对一致），不是偏移错。
> 所以本节「全量搜索会崩」的结论失效，但**通道仍不可用于实际读写**，
> 在下一条结论出来前不要对游戏目标做全量搜索。

**现象**：rwproc 通道下对某些目标进程执行 `gg_search`（`region=anonymous` 全扫），
**设备直接重启**（不是 agentd 崩溃）。agentd 日志停在收到 `cmd=27` 那一刻，
无 tombstone、无 logcat、`pstore` 为空（该机未配置 ramoops，拿不到 panic 栈）。

### 已排除

| 假设 | 实验 | 结果 |
|---|---|---|
| `is_force=1` 翻转 PTE 与活跃页竞争 | 查代码 | `driver.c` 传的是 `0 /* is_force */`，不是这个 |
| 用户态段错误/越界 | 查 tombstone / logcat | 无新 tombstone，无 crash 记录 → 不是用户态崩 |
| 句柄泄漏 | 查 `rwproc_open_process` | 有缓存（`if (!d->rwproc_handle)` 才 open），不泄漏 |
| 单次读长度过大 | 读 libc 前 4KB | 正常 |
| 数据量过大 | 读整个 libc（1MB） | 正常，设备存活 |
| 区域数量多 | pid 1 全 anonymous 搜索 | **正常**，设备存活 |

### 当前定位

**与"读哪个进程"有关，与"读多少"无关**：

```
pid 1   + region=anonymous 全扫 → count=0，设备存活（uptime 持续增长）
sleep   + region=anonymous 全扫 → agentd 失联 → 设备重启（uptime 归零）
```

两者都是 rwproc 通道（`driver_state=1`）。差别只在目标进程的内存布局。

### 高风险使用场景（务必注意）

**不要用 rwproc 通道对真实游戏进程做全内存搜索**——真实进程的内存布局比 init 复杂得多，
很可能命中同样的崩溃点。当前若要搜索，用 `gg_search_range` 把范围限定在**确定已映射且活跃**的地方，
逐个区域推进。

### 补充：`bytes: 0` 不等于通道坏了（2026-10-21 实测）

`is_force=0` 的语义是「只读 PTE 有效的页，不翻转权限」。因此**已被回收的文件页读不到**，
KO 返回 0 字节（不是 -1）。实测同一进程内：

| 地址 | 映射 | 结果 |
|---|---|---|
| `0x55655e0000` | init 代码段（正在执行） | ✅ `9f2403d5…`（ARM64 指令） |
| `0x5565804000` | init 只读数据段 | ✅ 全 0 |
| `0x5565560000` | init ELF 头（文件页，已回收） | ⚠️ `bytes: 0` |
| `0x7ffc00000000` | 未映射 | ❌ `Bad address`（EFAULT） |

之前"attach 后读不到 ELF 头"被误判成通道故障，实际是页不在内存。`gg_read_bytes` 现在会在
`bytes: 0` 时返回 `note` 字段说明原因，不再静默返回空。

**这也修正了上文的一个判断**：§十二 里"读整个 libc 1MB 正常"能成立，是因为 libc 当时活跃；
不能据此推断"只要长度够小就一定读得到"。

### 下一步排查（需要 KO 源码）

根因在 KO 的 ioctl 实现里（`gitee.com/wei-yetao/rwProcMem33-afei` 的 `rwProcMem_module`），
不在 agentd 侧。方向：

1. 用 `gg_search_range` 遍历 sleep 进程 maps 里的每个区域，二分定位到崩在哪一类
   （`[vvar]` / `[vdso]` / `[stack]` / guard page / 设备映射 / `[anon:dalvik-*]`）；
2. 拿到区域后对照 KO 源码看 `CMD_READ_PROCESS_MEMORY` 对该类 VMA 的处理；
3. 若确认是 KO 缺陷且上游已修，换新版本 KO 重做 §十一 的两步适配。




