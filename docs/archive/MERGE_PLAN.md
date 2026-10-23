# worktree 合并方案（2026-10-23）

> 目标：把 `audit/full-review` 分支的 5 个安全审查修复合并进 master。
> 状态：**分析完成，未执行**（master 工作区有活跃改动，等它收工）。

## 一、分叉结构

```
              43396ee  (merge-base)
             /        \
      master              audit/full-review
   我的 12+ 个提交          5 个审查修复提交
   (UE4 分析 + 工具改进)     (freeze/协议/wire/force)
```

**两条线互不包含**：`git merge-base --is-ancestor` 验证，audit 的 5 个提交
**全部不在 master** 上。

## 二、要合进来的 5 个提交

| 提交 | 主题 | 规模 | 涉及文件 |
|---|---|---|---|
| `5e83b59` | wire/server：修 POSIX 重复 connect、协议头自包含、导出文件原子替换 | +50/-10 | wire.cpp / main.cpp / session.cpp / wire_protocol.h |
| `078d9fe` | agentd：事件帧与响应帧同步、搜索跨块重叠、freeze 生命周期 | +112/-29 | freeze.c/h / main.c / search.c |
| `a931436` | tests/docs：补搜索跨块回归 + 外部审查逐条处置 | +202 | test_search.c / EXTERNAL_AUDIT_TRIAGE.md |
| `fb1b0b5` | chore：configs/gg-ghost.json 脱离跟踪（防 wire_key 入库） | +3/-22 | .gitignore / configs/ |
| `2992495` | agentd/server：force 参数接线，对 is_force_read 明确拒绝 | +79/-22 | cmd_view.c / driver.c/h / main.c / schema.cpp / 检查脚本 |

**注意规模**：5 个提交合计约 20 个不同文件，不是 74 个 ——
之前看到的 "74 文件 +4329/-2261" 是整个分支 vs 分叉点的累计差异，
包含了双方各自的改动。

## 三、冲突分析（32 文件 / 85 块）

`git merge-tree` 预演结果。冲突**绝大多数是"两边各加各的"**，不是同逻辑改写。

| 文件 | 冲突块 | 性质 |
|---|---:|---|
| `scripts/install_ko.sh` | 20 | 可能是行尾/整段重写 |
| `agentd/driver.c` | 10 | force 参数 vs master 的改动 |
| `agentd/main.c` | 7 | 两边各加 case（aob_resolve vs freeze/force） |
| `docs/engineering/ARCHITECTURE.md` | 7 | 文档各自更新 |
| `server/device/wire.cpp` | 5 | POSIX connect 修复 vs master |
| `agentd/search.c` | 4 | **master 侧更新**（pagemap 驻留筛选 + aligned 步进），audit 侧是旧代码 |
| `docs/PROJECT_MAP.md` / `README.md` 等 | 各 2-3 | 文档 |
| `server/session.cpp` | 2 | 两边各加 tool |
| `shared/wire_protocol.h` | 2 | 两边各加枚举 |

### 抽样证据：`agentd/search.c` 的 4 块

- 块 1/2：master 有 `if (pmfd >= 0) close(pmfd)`（pagemap 路径的资源释放），audit 没有
  → **保留 master**
- 块 3/4：master 有 aligned 步进策略（`i += step` + 真地址对齐起点），
  audit 是旧的 `i++` → **保留 master**

**结论：search.c 这 4 块一律取 master**，但要确认 audit 在该文件的改动
（搜索跨块重叠）落在**别的 hunk**里，别被一起丢掉。

## 四、推荐策略：cherry-pick 逐个，不用 merge

**为什么不用 `git merge`**：
- 一次性 32 文件 85 块，容易 `-X ours` 一把梭把审查修复丢掉
- 出问题无法单独回退某个修复

**cherry-pick 顺序**（按依赖，从独立到耦合）：

```
1. fb1b0b5  .gitignore + configs 脱跟踪      ← 最独立，先做
2. a931436  测试 + 审计文档                  ← 只加文件，冲突最少
3. 2992495  force 参数接线                   ← driver/main/schema
4. 5e83b59  wire/server 修复                 ← protocol/session
5. 078d9fe  事件帧 + 跨块 + freeze 生命周期   ← 和 master 的 search.c 改动正面相遇，最麻烦
```

每个的处理方式：

| 提交 | 预判冲突 | 处理方式 |
|---|---|---|
| fb1b0b5 | `.gitignore` 1 块 | 两边条目都留 |
| a931436 | `test_search.c` 少量 | 保留 master 已有用例 + 追加 audit 的新用例 |
| 2992495 | `main.c` / `schema.cpp` / `driver.c` | 两边各加各的，全部保留；`driver.c` 的 10 块要逐块看（force 语义 vs master 的 read 路径） |
| 5e83b59 | `wire.cpp` / `session.cpp` / `wire_protocol.h` | 全部保留（各自新增）；`wire_protocol.h` 枚举顺序要确保 audit 的新值也追加在末尾 |
| 078d9fe | `search.c` **正面冲突** | **master 的 pagemap/aligned 必须保留**，把 audit 的"跨块重叠"逻辑贴进对应 hunk；`freeze.c` 全量取 audit |

## 五、执行前提

**必须等 master 工作区干净**。分析时的状态：

- 655 行未提交改动，**不是我的**（`filter_results` 那批）
- `server/session.cpp` 在 18:39 还在被改（活跃会话）
- `agentd/screen.c` 18:26 新建（未跟踪）

在脏工作区上 cherry-pick 会直接失败（git 会拒绝）。

**检查命令**：

```bash
git status --short          # 要为空
git worktree list           # 确认 audit 分支的 worktree 还在
```

## 六、预计工作量

- 5 次 cherry-pick
- 约 10-15 个真冲突块需要手工判断（其余是文档/脚本，机械合并）
- **难点只有 1 个**：`078d9fe` 的 `search.c`
- 完成后跑 `tests\unit\run_tests.bat` + 5 道护栏验证

**风险提示**：`2992495` 的 force 参数改动和 master 现在的 `gg_read` 路径有交集，
合完必须验证 `force=1` 仍然被明确拒绝（这是那次审查的核心结论之一）。

---

# 最终结论（19:20 复核后）：**不需要合并，成果已在 master 上**

上面那套 cherry-pick 方案不用执行了。逐项验证后发现 audit 分支的修复
**全部已经以"内容等价、hash 不同"的形式存在于 master**：

| audit 的提交 | master 上的等价提交（不同 hash） | 功能验证 |
|---|---|---|
| `5e83b59` wire POSIX connect + 协议头 | `9f664c0` / `fa6c873` | `ensure_connected_locked()` 存在 |
| `078d9fe` A6/A7/A8/A10/A5 | `046e5e2` | A8 注释在、freeze 有 `thread_started`/`_Atomic`、watch 有 `cJSON_IsString` |
| `a931436` 9z 测试 + 审计文档 | `fcbad56` / `a5691d4` | `test_search.c` 有 4 处 `9z)`，`docs/audit/` 6 个文件都在 |
| `fb1b0b5` configs 脱跟踪 | —— | `configs/` 只剩 `gg-ghost.example.json` |
| `2992495` force 拒绝 | `4ee28e5` | `driver.c:731` 有明确拒绝文案 |
| （大提交）wire 握手鉴权 | 已拆成多个提交 | `wire.cpp` 6 处 wire_key/handshake |

`git cherry -v master audit/full-review` 报 `ee00d6e`（wire 握手鉴权大提交）
为 `+` 未应用，但**这是误判**：它的内容被拆分重组进了 master 的多个提交，
逐功能验证（鉴权、连接上限、参数校验）在 master 上都能找到。

## 那 master 的 32 文件冲突是怎么来的

`git merge-tree` 报的冲突是"**同一个文件的双方各自改动**"，不是"功能缺失"。
典型例子 `agentd/search.c`：master 有 pagemap 驻留筛选 + aligned 步进（后续提交），
audit 停在旧版本 —— 所以对比必然冲突，但**要合的东西 master 已经有了**。

## 建议

1. **不做 merge / cherry-pick**
2. worktree 可以清理（`git worktree remove`），但要等主工作区的活跃会话结束
3. 唯一值得确认的：`git log master` 里 `4ee28e5`（force）和 `ee00d6e` 的鉴权部分
   是否同源 —— 如果确认同源，audit 分支可以直接删
