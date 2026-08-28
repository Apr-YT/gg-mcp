# AGENTS.md — GG-MCP 工作区指令（每个会话自动加载）

## 项目一句话

内核驱动式内存修改 MCP 服务：PC 上的 AI agent 通过 MCP 直接读写安卓游戏进程内存。
**只走 KO 内核驱动通道**（QX / rwproc33 / hook / dev 四型），GG 核心采用外挂圈子加载（ELF 桩 + hook syscall 270）。

## 项目结构

```
server/    PC 端 gg-mcp-server（C++17）：MCP 协议 + 会话 + wire 客户端 + IDA 联动
agentd/    设备端 gg-agentd（C, NDK arm64, root）：搜索/读写/冻结/监视 + 驱动对接
native/    内核侧：hook.c（hook 型 KO）、lib5_stub.c（GG 核心加载桩）
shared/    wire_protocol.h — PC/设备双端协议唯一事实源（20B 头 + JSON 负载）
tests/     unit（posix_stub 可 PC 跑）/ integration / regression 三层
scripts/   构建（build_pc.bat / build_ndk.bat / build_all.bat）、部署、测试、基准
configs/   gg-ghost.json（serial/端口/KO 类型/IDA 联动）
tools/     辅助脚本（cfm_coord_read.py 等）
docs/      工程文档 + 逆向研究笔记混放（待分目录）
```

## 构建与验证

```powershell
# PC server（MinGW g++ 14.2，-O3 -flto）
scripts\build_pc.bat          # 或 scripts\build_all.bat 全量

# 设备端（NDK r27d，D:\Kiln\android-ndk-r27d-windows）
scripts\build_ndk.bat

# 运行
build\pc\gg-mcp-server.exe --config configs\gg-ghost.json

# 测试
scripts\test_all.bat          # 全部
tests\unit\run_tests.bat      # 单元
scripts\benchmark.bat         # 性能基准

# 结构守护（拆分/改分发后必跑）
python tests\unit\check_dispatch_consistency.py
python tests\unit\check_kernel_only.py
```

设备：Redmi myron（serial `134d2f8`，KernelSU root，内核 6.12.69，SELinux Permissive）。
adb 在 `D:\Kiln\ADB\adb.exe`。服务/窗口排查：`dumpsys window windows | grep ggmcp`；崩溃：`logcat -s AndroidRuntime:E`。

## 已知债（2026-08-28 评审，同日 P0/P1 已清，剩余如下）

1. **main.c 巨型文件**（102KB / ~70 函数）：按域拆 cmd_search / cmd_mem / cmd_proc / dispatch，拆前先跑 check_dispatch_consistency.py 留基线
2. wire 协议 1MiB 上限无分片；XOR seed 固定 0xA5；serial 硬编码在 config
3. docs/ 工程与逆向研究混放待分目录
4. archive/framework_v1/ 里的全量版框架（perf/atomic/error/mempool/conn_pool/io_batch）未接线，后续要么启用要么删除

已清（2026-08-28）：CI 三 job 重写（v4 + Windows 单测 + NDK 构建）；lint.bat 重写落地；双套模块收敛（simple 版留用，全量版入 archive）；run_tests.bat 修复（ASCII/链接顺序/编码）并全绿；build_ndk 补 driver.c + 删 shell_util.c 幽灵引用；deploy.bat 产物对齐。

详细评审见 `docs/OPTIMIZATION_SUMMARY.md`（08-22 优化记录）。

## 约定

- 提交信息：`<域>: <动词开头的一句话>`（域 = agentd / server / wire / scripts / docs / tests / chore）
- 敏感研究笔记（ACE/反作弊对抗）只放 docs/，不外发；驱动密钥、QX_KEY 等不入库不入对话
- 设备端改动必须在真机过 itest（scripts/deploy.bat 部署后跑 tests/integration/run_itest.ps1）
- 特征字符串纪律：agentd 二进制 strings 不得出现 GG-Ghost / gg-touch / GG_KO 等特征（历史提交 88d83cc、21b246d 有验证方法）

## 历史

apk/ 悬浮窗 APK 与 ggtool/ UI Demo 线已于 2026-08-27 下线（commit bcdcf1f，可从 git 历史恢复）。
旧版工作区指令（apk/ggtool 时代）归档在 `docs/AGENTD_HISTORY.md`。