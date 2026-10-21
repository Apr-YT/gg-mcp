# ARCHITECTURE.md — GG-MCP 架构与数据流（KO 驱动方案）

## 1. 组件

| 组件 | 位置 | 语言 | 职责 |
|---|---|---|---|
| `gg-ghost-mcp` | PC（Windows） | C++17 | MCP 协议面（stdio/HTTP）、45 个工具、会话、adb forward 管理 |
| `gg-agentd` | 设备（root） | C（NDK arm64） | **KO 驱动加载与通道**、进程/maps、搜索/冻结引擎、GG 核心加载编排 |
| `libKernelGg.so` | 设备（GG 进程内） | C（NDK） | LD_PRELOAD hook：拦截 syscall 270/271 → KO 驱动（或直通内核 hook） |
| `lib5_stub` | 设备（GG 私有目录） | C（NDK 静态 PIE） | 替换 GG 的 lib5.so，保持合法 ELF 身份，设 LD_PRELOAD 后 exec 原库 |

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

## 5. GG 核心加载（外挂圈子方式）

GG 守护链：`GG主程序 → su → lib05.so(守护进程) → exec <ggdir>/lib5.so`

```
<ggdir>/lib5.so          ← 被替换为 lib5_stub（静态 PIE，合法 ELF）
<ggdir>/lib5.so.orig     ← 原库备份（桩通过 /proc/self/exe 推断路径）
<ggdir>/libKernelGg.so   ← hook 库
桩启动: setenv LD_PRELOAD=<ggdir>/libKernelGg.so → execv(lib5.so.orig, argv)
hook:   syscall(270/271) → 解析 iovec → dev 型 KO: ioctl(/dev, 0x7e1a/0x7e1b)
                                      → hook 型 KO: 直通真实 syscall（内核已接管）
```

关键约束（社区踩坑记录）：
- 守护进程校验 `lib5.so` 必须是合法 ELF —— 用静态 PIE 桩而非 shell 脚本，否则报 "already run"。
- LD_PRELOAD 必须先于 lib5.so 加载（RTLD_NEXT 作用域）—— 由桩的 exec 顺序保证。
- 文件权限：hook/桩 777（社区实践），属主 root 或目标应用。

## 6. 安全与容错

- 降级链：KO → process_vm_readv → /proc/pid/mem；`kernel_status` 标记 state（kernel-driver / ko-hook / degraded）。
- safe-mode：只读会话（write/freeze/shell/load 本地拒绝 + 设备端双保险）。
- shell 白名单：ps/ls/cat/dumpsys/getprop/id（默认关闭）。
- adb 断连：工具调用返回 GGERR_NO_DEVICE，自动重连一次。
- 架构限制：驱动协议 64 位布局，**仅支持 64 位目标进程**（32 位 attach 明确报错）。
