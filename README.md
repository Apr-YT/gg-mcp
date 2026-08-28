# GG-MCP · GameGuardian MCP Server（C++）

内核驱动式内存修改协议服务，让 AI agent 直接读写游戏进程内存。

## 快速开始

构建:
  cd scripts && build_all.bat

部署:
  cd scripts && deploy.bat

使用:
  build\pc\gg-mcp-server.exe --config configs\gg-gghost.json

## 项目结构
- agentd/ - Android端agentd
- server/ - PC端MCP服务
- native/ - 内核模块
- tests/ - 测试

## CI/CD
GitHub Actions 自动构建。

## 优化模块
- log_enhance.h - 日志增强
- atomic_simple.h - 原子操作
- perf_simple.h/c - 性能统计


## 测试

单元测试:
```powershell
cd tests/unit
for %%t in (test_*.exe) do "%%t.exe"
```

集成测试:
```powershell
cd tests/integration
.\run_itest.ps1
```

性能基准:
```powershell
cd scripts
.\benchmark.bat
```

全部测试:
```powershell
cd scripts
.\test_all.bat
```


## 代码质量

代码检查:
```powershell
cd scripts
.\lint.bat
```

重构指南:
详见 `docs/CODE_RECODE_REFACTOR.md`
