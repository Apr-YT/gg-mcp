# BASELINE — gg-mcp 全面审查基线

> 建立时间：2026-10-21 22:40–23:10（GMT+8）
> 执行者：定点审查（阶段 A）
> 原则：**只记录实际执行过的结果**。未执行的项写明原因，不计入通过。

## 1. 仓库状态

| 项 | 值 |
|---|---|
| 仓库根 | `C:\Users\Administrator\WorkBuddy\Worktrees\GG-mcp\master-9ab0ea66` |
| 审查分支 | `audit/full-review`（本轮从 `master` 新建） |
| 基线提交 | `dddda9b docs: 修正 AGENTS 中 run_tests_wsl 不存在的不实描述` |
| 工作区 | 干净（`git status --short` 无输出） |
| 与分支 `fix/mem-io-integrity` 的关系 | 该分支基于 `a29871b`，比 master **落后 20+ 个提交**，且当时修改的 hook/dev/qx 三型驱动已被 `96850ec` 整型删除。**该分支不在本次审查范围内，未合并** |

### 为什么基于 master 而不是当前工作区分支

`master`（`dddda9b`）已包含 20 个提交的演进，其中包括 `96850ec`（删除 hook/dev/qx 三型驱动）、
`ee00d6e`（wire 握手鉴权）、`d399359`（修 rwproc 回写缓冲区越界）。基于旧分支审查会得到过时结论。

## 2. 环境（实测）

| 组件 | 版本 / 路径 | 状态 |
|---|---|---|
| MinGW g++ | 16.2.0（`D:\Kiln\mingw64`） | ✅ |
| Python | 3.13.14 | ✅ |
| NDK | `D:\Kiln\android-ndk-r30`，clang 21.0.0 | ✅ |
| capstone | `D:\Kiln\capstone\capstone-5.0.1\libcapstone.a`（4.6 MB） | ✅ |
| cppcheck | 已安装 | ✅ |
| adb | `D:\Kiln\platform-tools\adb.exe` | ✅ |
| 设备 | Redmi myron `134d2f8`（在线） | ✅ 已连接 |
| 设备内核/root | 6.12.69 / KernelSU | 未在本次复核（沿用 AGENTS.md 记录） |

## 3. 构建矩阵

| 目标 | 命令 | 结果 |
|---|---|---|
| PC server | `scripts\build_pc.bat` | ✅ 成功，产出 `build\pc\gg-ghost-mcp.exe`（仅 `-flto` 串行编译提示，无警告） |
| Android agentd | `scripts\build_ndk.bat` | ✅ 成功，产出 `build\agentd\gg-agentd` |

## 4. 测试矩阵

| 层级 | 命令 | 结果 | 说明 |
|---|---|---|---|
| 单元 + 结构护栏 | `tests\unit\run_tests.bat` | ✅ **ALL PASS** | 含 12 个单测可执行文件、dispatch 一致性（43 工具）、kernel-only 护栏 |
| stdio 端到端冒烟 | `tests\regression\test_stdio_smoke.py`（经 `test_all.bat`） | ⏸ **未单独执行** | 后续在阶段 E 补跑 |
| 集成（真机） | `tests\integration\run_itest.ps1` | ❌ **FAIL** | 见 §4.1 |
| 基准 | `scripts\benchmark.bat` | ⏸ **未执行** | 仓内无基准源码（已知债），脚本会明确报错 |
| lint | `scripts\lint.bat` | ⚠️ **"通过"但不可信** | 脚本恒 `exit /b 0`，见 F-09 |
| 静态分析 | `cppcheck --enable=warning,performance,portability agentd server` | ✅ 自有代码无告警 | 输出噪声全部来自 `third_party/json.hpp`、`httplib.h`；`wire.cpp:460` 的 `directiveAsMacroParameter` 是 cppcheck 对 `GG_LOG` 宏的解析限制，非缺陷 |

### 4.1 真机集成测试结果（真实执行）

```
== 设备端集成测试 ==
FATAL: rwproc_attach failed: open /proc/e84523d7b60d5d341a7c4d1861773ecd/e84523d7b60d5d341a7c4d1861773ecd failed: No such file or directory
== 失败: itest 退出码 1 ==
```

**结论**：设备上不存在 rwproc33 的 `/proc/<key>/<key>` 节点。`driver.c` 已删除 hook/dev/qx 三型，
而设备当前的 KO 是 hook 型（见 `CODE_REVIEW_2026-10-21.md` §8.2），**代码与设备驱动不匹配**。

这不属于代码缺陷，是部署态不一致；但它意味着**所有设备端改动在本机无法功能闭环**。
未执行 `scripts\deploy.ps1`，原因是它会 stop 并覆盖设备上当前可用的 agentd，属于需要用户确认的破坏性操作。

附带现象：itest 的中文输出在 adb 回传后显示为乱码（`寮曟搸闆嗘垚娴嬭瘯`），
即 UTF-8 被按 GBK 解释 —— 显示问题，不影响断言（见 F-12）。

## 5. 测试产物污染（实测）

`build\pc\` 下存在 15 个 `test_*.exe`，其中 **`test_ko_downgrade.exe` 的源文件已随
`test_ko_downgrade.c` 一并删除**（`git ls-files` 无此文件），但二进制仍在：

- `run_tests.bat` 显式列测试，**不受影响**；
- `test_all.bat` 用 `for %%t in ("%ROOT%\build\pc\test_*.exe")` 全量扫描 → **会执行这个陈旧二进制**，
  其结论与当前源码无关（见 F-15）。

## 6. 已有审查文档与本次的关系

`docs/archive/CODE_REVIEW_2026-10-21.md`(392 行) 已完成一轮审查，已修 §1.1–§1.10。
其中 **§3.3「agentd 侧 strtoull 不检查 endptr」已由 `ee00d6e` 的 dispatch 集中校验门覆盖**（`main.c:1545-1567`），
本次不重复修复，仅在 F-05 记录该门的残留漏洞（负数字符串）。

本次审查**不重复**已完成项，只针对当前 master 上仍存在的问题。

## 7. 阻塞项

| 阻塞 | 影响 | 处理 |
|---|---|---|
| 设备 KO 为 hook 型，与 rwproc33-only 代码不匹配 | 设备端改动无法功能验证 | 记录为环境阻塞；不部署以免破坏设备当前可用状态 |
| 无基准源码 | 性能回归无法测 | 已知债，不阻塞功能 |

## 8. 基线结论

- PC 端与 agentd 均可编译，单元测试与结构护栏全绿 —— 作为后续修复的**回归基线**。
- 真机链路不通，设备端修复只能做到"编译 + 逻辑验证"，**不得声称已通过 itest**。
- 静态分析无自有代码告警，缺陷需靠**代码路径推演 + 针对性测试**发现。
