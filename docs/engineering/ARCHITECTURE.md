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

**只加载 KO，不依赖 KPM/KPatch-Next。** 加载流程（agentd 启动时）：

```
1. 扫描 ko/ 目录的 .ko 文件
2. 版本族匹配（ko_version_pattern，默认 6.12.*）选择文件
   —— 同族补丁号任意均可加载（driverX_6.12.1.ko / 6.12.9.ko / 6.12.123.ko）
3. setenforce 0（社区 KO 普遍需要 SELinux permissive）
4. insmod <path>，/proc/modules 验证
5. dev 型: 按 /proc/misc 自动 mknod /dev/<name>（主 10, 次号查表）→ open + ioctl
```

两类后端：
- **hook 型**（默认）：内核 hook `process_vm_readv/writev` → 用户态零协议，直接调用
  `process_vm_readv` 即由 KO 服务。自检状态 `ST_KO_HOOK`（`state=ko-hook`）。
- **dev 型**：`open /dev/xxx` + `ioctl(0x7e1a读/0x7e1b写, {u32 pid; u32 pad; u64 addr; u64 buf; u64 size})`。
  自检状态 `ST_DRIVER`（`state=kernel-driver`）。
- **qx 型**（Aloha 开源 QX9-10）：隐藏进程持有已删除节点 → `/proc/<pid>/fd/3` 取主次号 →
  `mknod /dev/ggqx` → `open` → `init_key(0x800)`；读写 `0x801/0x802`（COPY_MEMORY 自然对齐），
  模块基址 `0x803`。自检状态 `ST_DRIVER`（`state=kernel-driver`）。协议详见 docs/engineering/PROTOCOL.md。

降级链：KO 通道 → `process_vm_readv` → `/proc/<pid>/mem`，任一层失败自动切换，`kernel_status` 标记状态。

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
