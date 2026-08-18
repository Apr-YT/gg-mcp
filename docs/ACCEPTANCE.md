# ACCEPTANCE.md — 验收清单（KO 驱动方案，对照开发计划 §1 成功标准）

> 状态: 每项在对应环境中验证后勾选。✅=已通过（本机可验证项），⬜=需真机环境。

## 1. 构建与产物（本机已验证 ✅）

| 产物 | 状态 | 验证方式 |
|---|---|---|
| `build/pc/gg-mcp-server.exe` (1.2MB) | ✅ | MinGW g++ 编译通过；stdio/HTTP 双传输冒烟测试通过 |
| `build/agentd/gg-agentd` (arm64, 83KB) | ✅ | NDK clang 编译通过；ELF Type=DYN (PIE) |
| `build/native/libKernelGg.so` (7.5KB) | ✅ | NDK 编译通过；导出 GLOBAL `syscall` 符号 |
| `build/native/lib5_stub` (静态 PIE) | ✅ | NDK `-static-pie` 编译通过；无 INTERP 段 |

## 2. MCP 服务器（本机已验证 ✅）

- [x] `initialize` 握手：protocolVersion 2024-11-05、capabilities.tools、serverInfo
- [x] `tools/list`：43 个工具全部注册（进程5/搜索7/读写8/分析7/内核3/系统3/监视反汇编5/标注5）
- [x] stdio 传输：换行分隔 JSON 请求/响应
- [x] HTTP 传输：`POST /mcp` + `/healthz`
- [x] 错误路径：无设备时 `gg_device_info` 返回 `isError:true, code=2 (NO_DEVICE)` 与清晰提示
- [x] 配置加载：configs/ggmcp.json + 命令行参数覆盖（参数优先）

## 3. 单测（本机已验证 ✅）

- [x] 版本族匹配（21 例）：6.12.1/6.12.9/6.12.123 同族可加载；6.13.0/7.0.1 拒绝；精确模式；解析容错；**KO 文件名提取**（`driverX_6.12.9.ko` → 6.12.9，跳过 QX114 数字段）
- [x] wire 协议（11 例）：头 20 字节、magic/版本、命令码唯一、JSON 负载约定、u64 字符串精度

## 4. 需真机环境验证（⬜ → 已勾选部分 ✅ 2026-08-16 真机）

- [ ] **KO 加载**：`ko/` 下的 .ko 经 insmod 加载成功，`gg_kernel_status` 显示 `ko_module/ko_file/ko_version`（qx 型为自动发现，不涉及 insmod）
- [ ] **hook 型 KO**：`state=ko-hook`，搜索走内核 hook 的 process_vm_readv
- [x] **dev 型 / qx 型 KO**：`state=kernel-driver`，ioctl 0x801/0x802 读写正常（真机 QX 通道验证 ✅）
- [ ] **版本容错实测**：ko/ 下 6.12.1 与 6.12.9 两个同族 KO 均可选加载；6.13.0 strict 拒绝
- [x] **引擎集成**：`tests/integration/itest`（搜索→读→写→冻结→解冻 全链路）**ALL PASS** ✅
- [x] **64 位进程 attach + 搜索**：登山赛车 `com.fingersoft.hillclimb.noncmcc`（arm64）attach → module_base(libgame.so)
      → hex_dump 读 ELF 头 `7f454c46` → 模糊搜索 init/refine → 精确搜索 `0x464c457f` 命中 5256 处 ✅（修复 process_is_64bit 误判后）
- [ ] **GG 核心对接**：GG 101.1 注入式（lib5_stub+libKernelGg.so）实测失败——GG 守护链自检 `android-daemon err`
      判定失败；**主链路无需 GG 本体**（agentd 自带引擎，见上一条）
- [ ] **降级链**：卸载 KO 后 `state=degraded`，搜索仍可用（process_vm_readv / /proc/pid/mem）
- [ ] **adb 重连**：断线后工具调用报 NO_DEVICE，重连后恢复
- [ ] **真实 AI 客户端**：Claude Code / Cline 完成握手与一次 `gg_search` 调用
- [ ] **自启**：`start_agentd.sh autostart` 后重启设备 agentd 自动拉起

## 真机验证步骤速查

```bat
rem 1) 构建
scripts\build_pc.bat && scripts\build_ndk.bat

rem 2) 把你的 KO 驱动放入 build\ko\（如 driverQX114_6.12.9.ko）

rem 3) 部署（自动 push/insmod/forward/启动 agentd）
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1

rem 4) 设备端引擎集成测试
powershell -ExecutionPolicy Bypass -File tests\integration\run_itest.ps1

rem 5) 启动 PC MCP 服务器（HTTP）
build\pc\gg-mcp-server.exe --config configs\ggmcp.json --transport http --port 8788
rem   然后 curl POST /mcp 调 gg_kernel_status 验证:
rem     hook 型 → state=ko-hook   /   dev 型 → state=kernel-driver
```
