# 多级指针链反向搜索（gg_pointer_path）

> 2026-11-04。本机单测全过、双端编译零代码警告；**真机未验证**（设备不在位）。

## 一、为什么加

指针工作流此前缺一环，三个工具拼起来正好是完整的：

| 工具 | 方向 | 回答的问题 |
|---|---|---|
| `gg_pointer_scan` | 目标 ← 一级 | 谁指向这个地址？（含 lookback 对象内偏移） |
| `gg_pointer_chain` | 给定位置 → 逐级解引用 | 这条链走不走得通？基址在哪？ |
| **`gg_pointer_path`（新增）** | 目标 ← N 级 | **自动搜出从模块到目标的整条链** |

缺的正是第三个。真实场景：搜索结果收敛到 1~3 个地址后，堆地址重启就变，
必须换成「模块基址 + 固定偏移」的表达才能复用。此前只能拿
`gg_pointer_scan` 的结果一个个手喂 `gg_pointer_chain` 试，几十个候选就是几十次
人肉迭代——这正是 CE pointer scan 自动化的那部分。

## 二、设计

### 编址约定

`nodes[0]` 最浅（离模块最近），`nodes[depth-1]` 最贴近目标：

```
nodes[0].addr   锚点（模块内的指针位置）
nodes[0].value  锚点读出的值 = 下一级对象首址
nodes[0].offset = nodes[1].addr - nodes[0].value
……
nodes[depth-1].offset = target - nodes[depth-1].value
```

按 CE 写法重算：`expr = [[0x<value0>+o0]+o1]+o2`。用例 15 里三级链输出
`expr = [[0x12200+0x0]+0x18]+0x24`，代回去确实等于目标——单测断言了。

### 算法

逐级回溯，不是一次性建 ptrmap：

1. `cur = {target}`，做一次 `pointer_scan`（lookback 模式）→ `levels[0]`
2. `levels[L]` 的 container 集合去重后当下一级目标，再扫 → `levels[L+1]`
3. `levels[L]` 里 container 落在锚点段的命中 = 一条完整链的浅端，
   沿 `hit.target` 逐级二分反查回原始目标，反序填 `nodes[]`
4. 任一级无命中即停（链断了，或该级被 cap 截断）

每级的 hits 按 container 排序，反查用 `bsearch`——深度 6、每级 2000 条时
反查总量约 2000×6×log(2000)，可忽略。

### 锚点为什么是"路径非空的映射段"

链终点必须在**同版本每次启动都一样**的地址上才有复用价值。文件映射段
（.so / dex / apk）满足；匿名段、`[heap]`、`[stack]` 全不满足——那些段
的地址由内核分配，重启即变。用 `path[0] != '\0'` 判定，比让调用方手填
基址可靠（还能顺带支持 `module` 子串过滤）。

## 三、已知限制

| 限制 | 原因 | 影响 / 处置 |
|---|---|---|
| depth 级 = depth 次全内存扫描 | 未做 ptrmap | 微信量级单级约 10s，depth=3 约 30s。先用 depth=2 |
| 每级候选上限 2000 | 内存 × 噪声控制 | 撑满时该级可能截断 → 返回 `warning`，缩小 `module` 或降 lookback |
| lookback 产生噪声链 | 整数恰好落在目标附近 | 深度越大越多。`module` 过滤 + 用 `expr` 重算核对值 |
| depth 上限 6 | 性能 | 能力上限，不是设计目的 |
| 只支持 dword/qword | ARM64 指针实际就这两种 | dword 留给 32 位目标进程 |

**没做 ptrmap 的理由**：桌面 CE 会把全内存的 (地址, 值) 对落盘成 ptrmap，
一次建图后多级查询是图遍历。移动端可写区动辄 500MB+，7300 万个 qword 槽，
即使只留"值落在已映射区内"的也要 ~58MB 常驻——在 agentd 这种要长期驻留的
进程里不划算。逐级回溯慢但内存恒定（6×2000×~40B ≈ 480KB）。

## 四、验证

本机 `tests/unit/test_search.c` 用例 15（11 条断言）：

- 三级链命中，`offset` 逐个核对 0 / 0x18 / 0x24
- 锚点段不覆盖链首 → 0 条（过滤生效）
- `nanchors=0` 不过滤 → 有输出
- `lookback=0` 精确模式 → 偏移链断，0 条
- 宽度/`target=0`/`depth=1` 三个参数边界

全量：12 个测试 ALL PASS、schema↔dispatch 55↔55、ko 快照同步 185 文件一致、
双端编译零代码警告。

**未验证**：真机上的实际链质量与耗时。设备接上后建议这样试——
《三角洲行动》或任一 UE4 游戏，搜一个稳定 dword（如金币），收敛到个位数后
对其中一个跑 `depth=2, module="libUE4.so"`，看能不能出链，再用
`gg_pointer_chain` 从锚点重走一遍确认 `expr` 可复现。
