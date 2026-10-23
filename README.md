# GG-MCP · GameGuardian MCP Server（C++）

内核驱动式内存修改协议服务，让 AI agent 直接读写游戏进程内存。

## 快速开始

构建:
  cd scripts && build_all.bat                  :: 仅 PC: build_all.bat -Target pc

部署:
  cd scripts && deploy.bat

使用:
  build\pc\gg-ghost-mcp.exe --config configs\gg-ghost.json

> 产物名是 `gg-ghost-mcp.exe`（提交 b953bc5 起统一命名，旧文档里的
> `gg-mcp-server.exe` 已不存在）；配置文件名是 `configs\gg-ghost.json`。
> 本机若未装 NDK，`build_all.bat` 走到 Android 阶段会明确失败——用
> `-Target pc` 只建 PC 端。

## 项目结构
- agentd/ - Android端agentd
- server/ - PC端MCP服务
- ko/     - 内核模块源码快照（rwNova33 内存模块 / rwNova 断点模块）
- shared/ - wire 协议唯一事实源 (wire_protocol.h)
- tests/  - unit / integration / regression
- scripts/- 构建、部署、测试、基准、KO 源码同步

> 内核模块的**工作副本**在 `build/rwProcMem33/`（上游克隆，不入库），
> 入库的是 `ko/rwProcMem33/` 快照。改完工作副本要 `python scripts/sync_ko_src.py .`
> 回写快照，否则 `run_tests.bat` 的快照护栏会失败。

## CI/CD
GitHub Actions 自动构建（`.github/workflows/build-and-test.yml`），三个 job：
PC 编译检查、Windows 单元测试 + 结构护栏、NDK 交叉编译。


## 测试

单元测试 + 结构护栏（PC 上可全跑，推荐先跑这个）:
```powershell
tests\unit\run_tests.bat
```
> 产物在 `build\pc\`（**不是** `tests\unit\`）。找不到 python 时脚本会直接失败，
> 因为两道结构护栏不允许静默跳过；确需跳过请显式 `set GG_SKIP_GUARDRAILS=1`。

集成测试（需已 root 设备 + NDK + adb）:
```powershell
tests\integration\run_itest.ps1
```

全部测试:
```powershell
scripts\test_all.bat
```
> 集成阶段缺 NDK/设备时、基准阶段（仓内无基准源码）时会打印明确的 WARN/SKIP，
> 不再假装通过。

性能基准:
```powershell
scripts\benchmark.bat
```
> ⚠️ 仓内目前**没有**基准测试源码（`tests\benchmark\memory_read.c` 不存在），
> 该脚本会明确报错退出。要跑基准请先补源码。


## 代码质量

代码检查:
```powershell
scripts\lint.bat
```

重构指南:
详见 `docs/archive/CODE_REFACTOR.md`
