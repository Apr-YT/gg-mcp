# 已关闭的历史条目（从 AGENTS.md 迁出，2026-10-23）

保留为可检索记录。全部已修复/已下线，不再是现行约束；需要细节时看这里的实测结论，
或者直接翻 git 历史。

### 已清（2026-10-15，本机实测）

- `run_tests.bat` 两道结构护栏曾是**死代码**（`if exist "%PYEXE%"` 对 PATH 命令名恒判假 → 静默跳过 + 报假绿，
  本地与 CI 同样中招）；已改为完整路径探测 + 校验确为 python 3 + 缺失时 `exit /b 1`。三条路径（正常/缺失/降级）均已实测。
- `build_all.bat` 汇总检查 `gg-mcp-server.exe`（实际 `gg-ghost-mcp.exe`）→ 恒报 `PC: MISSING`；
  且 `-Target` 参数被文档承诺却从未实现。已修正，并让"请求的产物缺失"导致构建失败。
- `test_all.bat` 在 `tests\unit` 下扫 `test_*.exe`（产物实际在 `build\pc\`）→ 循环体空转却打印 `[OK]`；
  且 `cd` 链相对错位。已改为按 `%ROOT%` 绝对路径、失败即退出。
- `benchmark.bat` 引用的 `tests\benchmark\memory_read.c` **全仓不存在**（无任何基准源码）→ 已改为如实报错。
- `run_itest.ps1` 的 `$Root` 少退一级（指向 `<repo>\tests\tests\...`）→ 已修正；adb 增加回退路径。
- 文档/脚本中残存的 `gg-mcp-server.exe` 全部对齐为 `gg-ghost-mcp.exe`（改名发生在提交 `b953bc5`「命名统一」）。
- **IDA Pro MCP 联动整体移除**（2026-10-15，按用户要求）：删 `server/ida_client.{h,cpp}` 与 `tests/unit/test_ida_client.cpp`；
  移除 `configs/gg-ghost.json` 的 `ida` 段、`build_pc.bat` / CI 的编译项、`main.cpp` 的 ida 配置解析、`session.{h,cpp}` 的
  `IdaConfig` / `ida_resolve`、`schema.cpp` 工具描述里的联动说明。`gg_resolve_symbol` 现为**纯本地静态解析**
  （模块 / 加载基址 / 模块内偏移 + ELF .dynsym 最近导出符号），响应**不再含 `ida` 字段**。旧实现可从 git 历史恢复。

### 已清（2026-08-28）

CI 三 job 重写（v4 + Windows 单测 + NDK 构建）；lint.bat 重写落地；双套模块收敛（simple 版留用，全量版入 archive）；
`run_tests.bat` 修复（ASCII/链接顺序/编码）并全绿；`build_ndk` 补 driver.c + 删 shell_util.c 幽灵引用；`deploy.bat` 产物对齐。

详细评审见 `docs/archive/OPTIMIZATION_SUMMARY.md`（08-22 记录，文首有 08-28 更正声明，**该文档部分声明不实，以更正声明为准**）。
