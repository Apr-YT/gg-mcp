# PROTOCOL.md — KO 驱动协议与 wire 协议笔记

## 1. wire 协议（PC ↔ gg-agentd）

帧：20 字节头 + JSON 负载（小端）。

```
struct gg_wire_header {           // sizeof = 20
  u32 magic;      // 0x47474D43 "GGMC"
  u16 ver;        // 1
  u16 cmd;        // GGCMD_*（见 shared/wire_protocol.h）
  u32 seq;
  u32 flags;      // GGWF_REQ / RESP / ERR / EVENT / ENC
  u32 payload_len;
};
```

标志位全集：`REQ=0x1`、`RESP=0x2`、`ERR=0x4`、`EVENT=0x8`、`ENC=0x10`（负载已 XOR 混淆，滚动 key，
每字节异或 `GG_XOR_SEED + 位置`，seed 见 `shared/wire_protocol.h`；仅防被动嗅探一眼看穿，非加密安全）。

负载约定：请求 `{"p":{...}}`、响应 `{"r":{...}}`、错误 `{"e":{"code":N,"message":"..."}}`。

命令全集：`GGCMD_PING = 1` … `GGCMD_AUTH = 42`，含 `GGCMD_MAX` 共 43 项枚举；
其中 28/29 已废弃，可用命令 40 条。PING、SELF_TEST、LIST_PROCESSES、ATTACH、DETACH、TARGET_INFO、
MODULE_LIST、SEARCH、SEARCH_UNKNOWN_INIT、SEARCH_REFINE、SEARCH_RANGE、RESULT_COUNT、GET_RESULTS、
CLEAR_RESULTS、READ、READ_BYTES、HEX_DUMP、WRITE、SET_RESULTS、FREEZE、UNFREEZE、MODULE_BASE、
CALC_OFFSET、POINTER_SCAN、PAUSE、RESUME、KERNEL_STATUS、SHELL、DEVICE_INFO、
WATCH、DISASSEMBLE、VIEW_MEMORY、RESOLVE_SYMBOL、POINTER_CHAIN、SUBSCRIBE_EVENTS、UNSUBSCRIBE_EVENTS、
RELAY_EVENT、GHOST_TAP、GHOST_SWIPE、AUTH。

> 命令码 28（`LOAD_GG`）/ 29（`CORE_VERSIONS`）已废弃但**保留占位**：
> 删除会让后续命令码整体前移，与已部署的 agentd 错位。
> 这两个命令的实现在 2026-10-21 随 GG 核心注入链路一并删除，调用返回 `BAD_REQUEST`。
> 新命令一律追加在枚举末尾 —— C 枚举允许重复值且不报错，插进中间会与硬编码的 28/29 撞车
> 而编译器不拦。

### 1.1 握手鉴权（`GGCMD_AUTH = 42`）

```
请求 {"p":{"key":"<hex>"}}
成功 {"r":{"authed":true}}
失败 {"e":{"code":4,"message":"wire auth failed"}}
```

- 未认证连接的请求只放行 `PING` / `AUTH`，其余一律 `GGERR_PERM`。目的是堵住「设备上任意本地
  App 连 127.0.0.1:34127 就能拿到 root 能力」这个口子。
- 密钥在设备端 `/data/adb/GG-Ghost/wire.key`（`0600`，首启自动生成 32 字节随机 hex），
  经 `--wire-key-file` 传给 agentd；PC 端 `configs/gg-ghost.json` 的 `wire_key` 必须一致，
  `deploy.ps1` 部署时自动拉取回写。
- **key 为空时 `AUTH` 无条件成功**（返回 `authed:true`）：为了不打断既有部署。等于这层防护
  只在真正配了 key 的部署上生效；`print_usage` 给的示例本身不带 key，照抄示例等于没开。
- 认证失败在 `dispatch` 里优先于 safe_mode / 驱动状态判定，失败响应不泄露引擎信息。

错误码：OK=0, BAD_REQUEST=1, NO_DEVICE=2, NO_SESSION=3, PERM=4, VERSION_MISMATCH=5,
DEGRADED=6, TIMEOUT=7, NOT_FOUND=8, ARCH_UNSUPPORTED=9, CAP=10, BUSY=11, INTERNAL=12,
SAFE_MODE=13, SHELL_DISABLED=14。

## 2. KO 驱动（只加载 KO，不用 KPM）

### 2.1 加载流程

部署侧 `scripts/install_ko.sh` 加载，**agentd 不做 insmod**（`main.c:2030` 原话：模块 `hide_by_default`，
重复 insmod 会 EEXIST），启动时只做节点发现 + 打开：

```
install_ko.sh（部署时执行，su -c 运行）
1. uname -r 截出 x.y.z
2. 只 insmod 文件名后缀版本号与之**完全相等**的那份
   （build/ko/ 下四份并存：rwProcMem / hwBreakpointProc × 6.12.23 / 6.12.69）
3. SELinux 仅加载期间临时 permissive，结束立即恢复 enforcing
4. /proc/modules 里 grep 不到属正常（模块可 hide）；重复 insmod 的 EEXIST 视为已加载

agentd 启动
1. open /proc/<mem_key>/<mem_key> 与 /proc/<bp_key>/<bp_key>
   （key 为空时扫描 /proc 找「目录名 == 内文件名」的节点；节点被隐藏时扫不到，必须显式给 key）
2. CMD_INIT_DEVICE_INFO
3. driver_self_test 回环 → 决定 state
```

驱动改过名（`apr_dlkm` 之类）时按名字匹配必然误判，最终判据只看 agentd 的节点自检。

### 2.2 唯一后端：rwproc33（abcz316/rwProcMem33）

两个独立 LKM，各自创建 `/proc/<AUTH_KEY>/<AUTH_KEY>` 节点，**用 read() 走 33 字节头协议**（非 ioctl）：

```
struct rwproc_request {          // packed, sizeof 33
  char     cmd;                  // 1 字节命令码
  uint64_t param1, param2, param3, buf_size;
};
read(fd, buf, 33 + data_size)   // 头在 buf[0..32], 数据区在 buf[33..]
                                // read() 返回值 = 结果长度；结果/句柄写回 buf+33
```

**`read()` 的 count 必须 ≥ `33 + 期望回写量`**，请求负载长度与回写长度并不总是相等 ——
`OPEN_PROCESS` 发 0 字节负载却要收回 8 字节句柄。`driver.c` 用
`rwproc_call_ex(..., resp_cap)` 把两个长度分开。

`rwProcMem_module` 命令码（`agentd/driver.h`）：

| 码 | 命令 | 说明 |
|---:|---|---|
| 1 | INIT_DEVICE_INFO | 必须首个执行；回写 1053 字节设备信息 |
| 2 | OPEN_PROCESS | → 进程句柄（后续命令的 param1） |
| 3 / 4 | READ / WRITE_PROCESS_MEMORY | param1=句柄, param2=地址, param3=is_force, buf_size=长度 |
| 5 | CLOSE_PROCESS | |
| 6 / 7 | GET_PROCESS_MAPS_COUNT / LIST | 本项目未接入（maps 走 `/proc/<pid>/maps` 自行解析） |
| 8 | CHECK_PROCESS_ADDR_PHY | 未接入 |
| 9–12 | GET_PID_LIST / SET_PROCESS_ROOT / GET_PROCESS_RSS / GET_PROCESS_CMDLINE_ADDR | 未接入 |
| 13 | HIDE_KERNEL_MODULE | |
| 15 / 16 | GGTOUCH_TAP / GGTOUCH_SWIPE | display 坐标，驱动内换算 raw 后注入 input；结构 12 / 20 字节 |

`hwBreakpointProc_module` 命令码 1–13：`OPEN/CLOSE_PROCESS`、`GET_NUM_BRPS(3)`、
`GET_NUM_WRPS(4)`、`INST_PROCESS_HWBP(5)`、`UNINST(6)`、`SUSPEND(7)`、`RESUME(8)`、
`GET_HWBP_HIT_COUNT(9)`、`GET_HWBP_HIT_DETAIL(10)`、`SET_HOOK_PC(11)`、
`HIDE_KERNEL_MODULE(12)`、`INST_PROCESS_HWBP_ALL(13)`。

- 断点参数 `param3 = len | (type << 8)`，type：R=1 / W=2 / RW=3 / X=4。
- 命中条目 `sizeof == 312`：`task_id` / `hit_addr`（模块实现里是命中指令 pc）/ `hit_time` +
  288 字节寄存器块（`regs[31]` + sp + pc + pstate + orig_x0 + syscallno）。
- 命中列表在断点生命周期内**累计不清零**（上限 1024，模块无清除命令），agentd 侧按已消费数去重。
- `INST_PROCESS_HWBP_ALL` 在 agentd 侧是**死常量**：只定义、无调用。当前默认关闭硬件断点
  （探测不过就报错），见 `ARCHITECTURE.md` §6。
- 断点模块**没有密钥门控**，与内存模块的 AUTH_KEY 不同。
- `rwproc_buf` 是进程级共享缓冲（搜索线程池 + 冻结线程都用），请求/结果拷贝必须整段在
  `rwproc_mtx` 内，`check_rwproc_locking.py` 守这条。
- `is_force=1` 已由 agentd 硬拒（2026-10-22 实测改 PTE 触发内核崩溃→设备重启），
  不要绕过。适配细节与结构契约见 `RWPROC33_ADAPTER.md`。

### 2.3 已删除的三型后端（hook / dev / qx）

2026-10-21 整条删除，只剩 rwproc33。原因见 `DRIVER_MATRIX.md` §6，摘要：

- 三型都**没有对应可用的 KO**，代码从未在真机执行过。
- hook 型的自检用 `process_vm_readv` 读自己，必然成功 → `state=ko-hook` 是假阳性，
  看起来「驱动就绪」实际什么都没接管。
- 维护成本：四套通道语义（ioctl / 设备节点重建 / fd 3 复用）各写一遍，实际只有一套在跑。

协议细节（dev 型 `0x7e1a/0x7e1b` ioctl、qx 型 `0x800–0x803` 与 `COPY_MEMORY`/`MODULE_BASE`
自然对齐布局）只在 git 历史与 `DRIVER_MATRIX.md` 里可查，现行代码不再支持。
`agentd/driver.c` 里原有 `ko_ioctl_req_t` / `qx_copy_memory_t` / `qx_module_base_t`
三个无调用结构体，**已于 2026-10-23 删除**（需要时从 git 历史恢复）。

### 2.4 自检标准（gg_kernel_status）

| 场景 | state | 值 |
|---|---|---|
| 节点可打开 + 回环读通过 | `kernel-driver (rwproc33)` | 1 |
| 其它一切（未 root / 模块未加载 / 节点名不对 / 读写失败） | `unavailable` | 4 |

自检方法：`OPEN_PROCESS(getpid())` + 读自身静态变量 `0x12345678` 回环校验。
`2 / 3 / 5`（`ST_PROC_VM` / `ST_PROC_MEM` / `ST_KO_HOOK`）是已删除的降级态，
枚举里保留数字但不产生。

## 3. GG 核心对接协议（已删除）

`lib5_stub` ELF 桩 + `libKernelGg.so` LD_PRELOAD 拦截 syscall 270/271 这条链路已于 2026-10-21
整条删除：

- **实测从未成功** —— GG 守护链自检报 `android-daemon err`（见 `ACCEPTANCE.md`）。
- **它是全仓唯一会改写用户文件的动作** —— 备份并覆盖 GG 的 `lib5.so`，中途失败可能无法还原。
- **主链路不需要它** —— agentd 自带搜索/冻结引擎，直接经 rwproc33 读写目标进程内存。

对应 MCP 工具 `gg_kernel_load_gg` / `gg_core_versions` 一并删除（对外工具 45 → 43），
wire 命令码 28/29 保留占位。历史实现（`syscall()` 覆盖、iovec 解析、`GG_KO_DEV` /
`GG_KO_IOCTL_*` 环境变量）见 `b4cf7ae` 之前的提交。

## 4. 兼容性注意

- 仅 arm64（`__aarch64__`）；32 位设备/进程不支持（驱动结构 64 位布局）。
- KO 必须与设备内核**完全同版本**（`install_ko.sh` 按 `uname -r` 的 x.y.z 精确匹配）。
  库内没有版本族豁免 —— agentd 早先的 `--ko-pattern` / `--ko-strict` 路径已随三型后端一起删除。
- **无降级**：驱动不可用（内核升级、模块没加载、AUTH_KEY 不对）时读写直接报错，
  不会回落到 `process_vm_readv` / `/proc/pid/mem`。内核一升级就得重编 KO。
- 模糊搜索快照内存上限 256MB（`GG_FUZZY_MAX_BYTES`），超出部分不纳入扫描。
