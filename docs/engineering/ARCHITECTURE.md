# ARCHITECTURE.md — GG-MCP 架构与数据流（KO 驱动方案）

## 1. 组件

| 组件 | 位置 | 语言 | 职责 |
|---|---|---|---|
| `gg-ghost-mcp` | PC（Windows） | C++17 | MCP 协议面（stdio/HTTP）、43 个工具、会话、adb forward 管理 |
| `gg-agentd` | 设备（root） | C（NDK arm64） | **rwproc33 通道对接**、进程/maps、搜索/冻结引擎 |

## 2. 通信链路

```
AI agent ──MCP(JSON-RPC)──▶ gg-ghost-mcp ──wire 二进制帧──▶ adb forward ──▶ gg-agentd
                                                                              │ driver_read/write
                                                                              ▼
                                                          KO 内核驱动（insmod 加载）
                                                          ├─ hook 型: 内核 hook process_vm_readv
                                                          └─ dev 型:  /dev 节点 + ioctl
```

- **MCP 层**：JSON-RPC 2.0（initialize / tools/list / tools/call / ping）。stdio 用换行分隔 JSON；HTTP 用 `POST /mcp`。
- **wire 层**：20 字节头 `{magic,ver,cmd,seq,flags,payload_len}` + JSON 负载（请求 `{"p":{}}`，响应 `{"r":{}}`，错误 `{"e":{code,message}}`）。定义见 `shared/wire_protocol.h`（PC/设备共用唯一事实源）。
- **驱动层**：见 docs/engineering/PROTOCOL.md。

## 3. KO 驱动加载与通道（核心）

**只加载 KO，不依赖 KPM/KPatch-Next。** 当前只有 **rwproc33** 一个后端
（hook / dev / qx 三型已于 2026-10-21 删除，原因见 `DRIVER_MATRIX.md` §6）。

### 3.1 加载与 agentd 解耦

insmod 由 `scripts/install_ko.sh` 负责，agentd 启动时只做对接：

```
install_ko.sh（部署时执行）
1. uname -r 取 x.y.z
2. 只 insmod 文件名版本匹配的那份（<module>_<x.y.z>.ko）
   两套产物并存：6.12.23（afei prebuilt）/ 6.12.69（本地改 vermagic + CRC）
3. setenforce 0（临时）→ insmod → 恢复 Enforcing

agentd 启动
1. 按 --rwproc-key / --rwproc-bp-key open /proc/<key>/<key> 两个节点
   （key 为空时扫描 /proc 找「目录名 == 内文件名」的节点）
2. CMD_INIT_DEVICE_INFO（必须首个命令）
3. driver_self_test：open 自身 + 回环读 → 通过则 state=ST_DRIVER
```

### 3.2 通道协议（33 字节头 + 数据区）

```
read(fd, buf, 33 + data_size)   一次完成「提交请求 + 收回结果」
buf[0..32]   请求头（cmd / param1-3 / buf_size），packed
buf[33..]    数据区；读内存时是内核回写的结果
```

**关键**：`read()` 的 count 必须 ≥ `33 + 期望回写量`。请求负载长度（`buf_size`）
与回写长度并不总是相等 —— `OPEN_PROCESS` 发 0 字节负载却要收回 8 字节 handle，
count 只给 33 会让内核写不进去，随后的 `memcpy(buf+33, 8)` 拿到的是上一次请求的
残留数据。`driver.c` 用 `rwproc_call_ex(..., resp_cap)` 把这两个长度分开。

### 3.3 失败即报错，不降级

没有 `process_vm_readv` 回落。通道不可用时 `kernel_status` 返回
`state=4 (unavailable)`，读写直接报错 —— 既避免语义不一致，也避免可被反作弊
检测的越界读取。

### 3.4 `is_force=0` 的代价

不翻转 PTE，只读 PTE 有效的页。因此**已被回收的文件页读不到**（KO 返回 0 字节），
活跃页正常。这是为规避「翻转 PTE 与目标进程活跃页竞争导致内核崩溃」而选的行为，
代价是偶发读空 —— `gg_read_bytes` 在 `bytes: 0` 时返回 `note` 说明原因。
详见 `RWPROC33_ADAPTER.md` §十二。

## 4. 引擎职责划分（关键决策）

**搜索/refine/结果集/冻结全部在 agentd 内完成**。原因：多 GB 内存扫描若跨 wire 流式传输会拖垮链路；设备端做位图 + 快照（模糊搜索内存上限 256MB），PC 只收命令与结果摘要。

- 精确/区间搜索：分块 256KB 读 + 逐字节模式匹配（任意对齐）。
- 模糊搜索：`unknown_init` 全置候选 + 快照；`refine` 逐元素比较（increased/decreased/unchanged/changed）收缩位图；物化时机 = 查询结果时（<= 10 万条）。
- 冻结：agentd 内循环线程每 100ms 写回锁定值；detach/进程退出自动清理；写失败（ESRCH）自动停条目。
- 会话：单目标进程（attach/detach）；引擎操作经互斥锁串行化（含静态搜索缓冲）。

## 5. GG 核心加载（已删除）

该链路（`lib5_stub` ELF 桩 + `libKernelGg.so` LD_PRELOAD 拦截 syscall 270/271）
已于 2026-10-21 整条删除。原因：

- **实测从未成功** —— GG 守护链自检报 `android-daemon err`（见 `ACCEPTANCE.md`）。
- **它是全仓唯一会改写用户文件的动作** —— 备份并覆盖 GG 的 `lib5.so`，
  一旦中途失败，用户的 GG 核心可能无法还原。风险与收益不成比例。
- **主链路不需要它** —— agentd 自带引擎，直接经 rwproc33 通道读写目标进程内存。

对应 MCP 工具 `gg_kernel_load_gg` / `gg_core_versions` 一并删除（对外工具 45 → 43）。
wire 命令码 28/29 保留占位，避免与已部署的 agentd 错位，调用返回明确错误。

## 6. 安全与容错

- **失败即报错，不降级**：只剩 rwproc33 一条通道（hook/dev/qx 三型已删除）。通道不可用时
  `kernel_status` 返回 `state=4 (unavailable)`，读写直接返回错误 —— 既避免语义不一致，
  也避免可被反作弊检测的越界读取。
- safe-mode：只读会话（write/set_results/freeze/shell/pause/resume/watch 本地拒绝 + 设备端双保险）。
- shell 白名单：ps/ls/cat/dumpsys/getprop/id（默认关闭）。
- adb 断连：工具调用返回 GGERR_NO_DEVICE，自动重连一次。
- 架构限制：驱动协议 64 位布局，**仅支持 64 位目标进程**（32 位 attach 明确报错）。
