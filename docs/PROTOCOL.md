# PROTOCOL.md — KO 驱动协议与 wire 协议笔记

## 1. wire 协议（PC ↔ gg-agentd）

帧：20 字节头 + JSON 负载（小端）。

```
struct gg_wire_header {           // sizeof = 20
  u32 magic;      // 0x47474D43 "GGMC"
  u16 ver;        // 1
  u16 cmd;        // GGCMD_*（见 shared/wire_protocol.h）
  u32 seq;
  u32 flags;      // GGWF_REQ / RESP / ERR / EVENT
  u32 payload_len;
};
```

负载约定：请求 `{"p":{...}}`、响应 `{"r":{...}}`、错误 `{"e":{"code":N,"message":"..."}}`。

命令全集（36）：PING、SELF_TEST、LIST_PROCESSES、ATTACH、DETACH、TARGET_INFO、MODULE_LIST、
SEARCH、SEARCH_UNKNOWN_INIT、SEARCH_REFINE、SEARCH_RANGE、RESULT_COUNT、GET_RESULTS、CLEAR_RESULTS、
READ、READ_BYTES、HEX_DUMP、WRITE、SET_RESULTS、FREEZE、UNFREEZE、MODULE_BASE、CALC_OFFSET、
POINTER_SCAN、PAUSE、RESUME、KERNEL_STATUS、LOAD_GG、CORE_VERSIONS、SHELL、DEVICE_INFO、
WATCH、DISASSEMBLE、VIEW_MEMORY、RESOLVE_SYMBOL、POINTER_CHAIN。

错误码：OK=0, BAD_REQUEST=1, NO_DEVICE=2, NO_SESSION=3, PERM=4, VERSION_MISMATCH=5,
DEGRADED=6, TIMEOUT=7, NOT_FOUND=8, ARCH_UNSUPPORTED=9, CAP=10, BUSY=11, INTERNAL=12,
SAFE_MODE=13, SHELL_DISABLED=14。

## 2. KO 驱动加载与通道（只加载 KO，不用 KPM）

### 2.1 加载流程（agentd 启动时自动执行）

```
1. 扫描 ko 目录（默认 /data/local/tmp/ggmcp/ko）下的 .ko
2. 版本族匹配（--ko-pattern 6.12.*）:
     ko_version_from_name("driverX_6.12.9.ko") → "6.12.9"
     version_match_family("6.12.9", "6.12.*") → 1（同族, 补丁任意）
     族外且 --ko-strict 1 → GGERR_VERSION_MISMATCH(5)
3. setenforce 0（社区 KO 普遍需要 SELinux permissive）
4. insmod <path>；/proc/modules 验证已加载
5. dev 型: 未显式指定 /dev 路径时按 /proc/misc 找模块名 → mknod /dev/<name> c 10 <minor>
   → open(O_RDWR)
```

### 2.2 hook 型 KO（默认，零协议）

内核直接 hook `process_vm_readv/writev`（syscall 270/271）。用户态**无需任何驱动协议**：
`process_vm_readv` 调用即由 KO 在核内接管（页表级读写）。自检状态 `ST_KO_HOOK`。

### 2.3 dev 型 KO（/dev 节点 + ioctl）

```
通道: fd = open("/dev/xxx", O_RDWR)
读:   ioctl(fd, 0x7e1a, &req)
写:   ioctl(fd, 0x7e1b, &req)

struct ko_ioctl_req {            // packed, 32 字节（agentd/driver.c 与 native/hook/hook.c 一致）
  u32 pid;
  u32 pad;
  u64 addr;                      // 目标进程虚拟地址
  u64 buf;                       // 本地缓冲区指针
  u64 size;
};
返回值 = 实际传输字节数。
```

ioctl 码可用 `--ioctl-read/--ioctl-write`（agentd）与 `GG_KO_IOCTL_READ/WRITE`（hook 库环境变量）覆盖。

### 2.4 QX 型（Aloha 开源 QX9-10，雪花/Paradise 家族）

用户提供 `qx-对接.h` 的协议实现（`--driver qx`）。QX 驱动以**隐藏进程**运行，
设备节点被删除但 fd 3 仍打开：
```
1. 发现: /proc/<pid>/exe -> /data/<6位小写>（目标已删除 access 失败）
2. 主次号: stat("/proc/<pid>/fd/3") → st_rdev（major/minor）
3. 重建节点: mknod /dev/ggqx c <major> <minor>（主次号一致即可, 路径自选）
4. open O_RDWR
5. 认证（可选）: ioctl(fd, 0x800, char key[0x100])  ← --qx-key
```

ioctl 协议（自然对齐, 与 qx-对接.h 的 C++ 结构一致）：

```
0x800  init_key     ioctl(fd, 0x800, char key[0x100])           // 密钥认证
0x801  读内存       ioctl(fd, 0x801, &COPY_MEMORY)              // 成功返回 0
0x802  写内存       ioctl(fd, 0x802, &COPY_MEMORY)
0x803  模块基址     ioctl(fd, 0x803, &MODULE_BASE) → base

struct COPY_MEMORY {          // 自然对齐, sizeof 32（非 packed!）
    pid_t     pid;            // offset 0
    uintptr_t addr;           // offset 8（4 字节填充）
    void     *buffer;         // offset 16
    size_t    size;           // offset 24
};
struct MODULE_BASE {          // 自然对齐, sizeof 24
    pid_t     pid;            // offset 0
    char     *name;           // offset 8
    uintptr_t base;           // offset 16
};
```

注意与通用 dev 型的区别：**请求结构非 packed、成功返回值是 0（不是字节数）**。
0x804-0x812（隐藏进程/陀螺仪/硬件断点）为驱动附加能力，本项目未接入，文档记录。

agentd 自检：qx 回环读自身静态变量（0x801 成功）→ `state=kernel-driver`。

### 2.5 rwproc33 型（abcz316/rwProcMem33，开源，`--driver rwproc`）

两个独立 LKM（`rwProcMem_module` 内存读写 + `hwBreakpointProc_module` 硬件断点），
各自创建 proc 节点 `/proc/<AUTH_KEY>/<AUTH_KEY>`，**用 read() 走 33 字节头协议**（非 ioctl）：

```
struct rwproc_request {          // packed, sizeof 33
  char     cmd;                  // 1 字节命令码
  uint64_t param1, param2, param3, buf_size;
};
read(fd, buf, 33 + data_size)   // 头在 buf[0..32], 数据区在 buf[33..]
                                // read() 返回值 = 结果长度；结果/句柄写回 buf+33
```

内存读写命令（CMD_READ/WRITE_PROCESS_MEMORY=3/4）：param1=进程句柄（CMD_OPEN_PROCESS=2
返回）、param2=地址、param3=is_force（1=临时改 PTE 强制读写）、buf_size=长度。
页表遍历 + 物理内存直读，不经过 process_vm_readv、不附加目标。

硬件断点（CMD_INST_PROCESS_HWBP=5）：param1=句柄、param2=地址、param3=`len | (type<<8)`；
返回 perf_event 指针作断点 ID。命中经 perf_event 回调收集，**完整寄存器
x0-x30/sp/pc/pstate**（比 QX 的 r0-r4 丰富）。命中列表累计不清零（上限 1024），
agentd 侧按已消费数去重。默认 AUTH_KEY 见两模块 `ver_control.h`
（`e84523d7...` / `dce37716...`，建议编译时改掉），适配实现见
`docs/RWPROC33_ADAPTER.md`。

### 2.6 自检标准（gg_kernel_status）

| 场景 | 期望 state |
|---|---|
| hook 型 KO 生效 | `ko-hook` |
| dev 型 / qx 型 KO 生效 | `kernel-driver` |
| KO 未加载/失效, 走 process_vm_readv | `process_vm_readv (degraded)` |
| 仅 /proc/pid/mem 可用 | `/proc/pid/mem (degraded)` |
| 全不可用 | `unavailable` |

自检方法：读/写自身进程内静态变量地址回环校验；dev 型先试 ioctl 回环，失败落降级链。

## 3. GG 核心对接协议（hook syscall 270）

GG 的 lib5.so 用 `process_vm_readv`（syscall 270）读目标进程内存。hook 库经 LD_PRELOAD 抢先加载：

```
long syscall(long n, ...)          // 覆盖 libc
  n == 270/271 (process_vm_readv/writev):
     解析 iovec 对（local/remote, 计数取小）
     逐 iovec:
       GG_KO_DEV 已设置（dev 型 KO）→ ioctl(/dev, 0x7e1a/0x7e1b, {pid,0,remote_base,local_base,len})
       未设置（hook 型 KO）        → svc 直通真实 syscall（内核已接管）
  其他 n: svc 内联汇编直通（aarch64），避免递归
```

环境变量：`GG_KO_DEV`（dev 节点路径）、`GG_KO_IOCTL_READ` / `GG_KO_IOCTL_WRITE`（十六进制 ioctl 码）。

## 4. 兼容性注意

- 仅 arm64（`__aarch64__`）；32 位设备/进程不支持（驱动结构 64 位布局）。
- KO 必须与设备内核版本匹配（社区驱动按内核编译）；insmod 失败时 `ko_load` 返回 GGERR_PERM 并提示。
- 降级链保证无 KO 时（如内核升级后驱动失效）搜索仍可用（process_vm_readv / /proc/pid/mem）。
- 模糊搜索快照内存上限 256MB（`GG_FUZZY_MAX_BYTES`），超出部分不纳入扫描。
