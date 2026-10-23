# 社区成熟工具调研：能抄什么、只能借鉴什么

> 2026-10-23。目标：拿成熟开源项目的实现，改进 gg-mcp 的搜索与定位能力。
> 每条都标了来源和许可证边界 —— **许可证不看清就抄代码，会给项目埋雷**。

## 一、候选项目对比

| 项目 | 平台/形态 | 解决什么问题 | 许可证 | 对我们 |
|---|---|---|---|---|
| **scanmem / libscanmem** | Linux/Android，C | 内存扫描引擎（exact/changed/unchanged/increased/decreased/snapshot/string） | **GPLv3**（libscanmem **LGPLv3**） | 算法借鉴；libscanmem 若动态链接可考虑（LGPL） |
| **Cheat Engine** | Windows，Lazarus | 指针扫描（业界标准）、代码注入、结构体分析 | 混合（核心 GPL，部分专有） | **只能借鉴算法** |
| **ptrscan** | Linux，C++ | CE 指针扫描的 Linux 重实现 | 见仓库 LICENSE | **参数设计最值得抄**（-p lookback / -l 深度 / -q 对齐） |
| **UE4Dumper**（zeroonead/kp7742） | Android，C，NDK | UE4 的 GNames/GUObjectArray/GWorld 遍历，生成 SDK | 见仓库 LICENSE（**需确认**） | UE4 对象枚举思路；**已停止维护** |
| **UE-Dumper**（McDaived） | Windows，C# | UE4/UE5 SDK 生成 + **自动签名查找** + vtable 解析 | 见仓库 | 自动 sig-finder 思路 |
| **UE4-Dumper**（AscarreX） | Win + 独立 Mobile 版 | 同上，更新较活跃 | 见仓库 | Mobile 版可参考 |

## 二、最有价值的三条发现

### 1. CE 的指针扫描是**反向**的，我们是正向的

CE 作者原话（forum.cheatengine.org）：

> "CE's pointerscanner **starts from the destination address and goes the other way** until it finds any base address" —— 从目标地址出发反向追，用 ray tracing 的类比：不从光源发射无限光线，而是从屏幕像素反向追到光源。

实现（`Cheat Engine/pointervaluelist.pas`）：
- 先全内存扫一遍，确认每个"看起来是地址"的值确实指向可读内存
- 建立**反向索引**：`值 -> 哪些地址持有它`
- 查询时按 `[目标-structsize, 目标]` 范围一次取出所有持有者，再对每个持有者递归上追

**我们的 `pointer_scan`**：全内存扫"等于 target 的值" → 只支持**精确相等**、**单级**、**无对齐控制**。

这直接对应两个已知缺口（BACKLOG P1-10/11）：
- 加 `lookback`（目标 ± N），一次抓出"指向对象任意成员"的指针
- 加 `depth`（多级），自动逐级上追到模块段
- 加 `aligned`（对齐），ARM64 天然 8 字节对齐

**ptrscan 的参数设计直接可抄**（不是代码，是接口）：
```
ptrscan -a <addr> -p 0x500 -l 4 -t 4 -q -v -w out.pscan
         目标地址  回看范围  深度  线程 对齐 详细 输出
```

### 2. UE 4.23+ 的 UObject 指针是**加密**的 —— 这解释了今天所有"0 命中"

UE4Dumper 有 `--ptrdec  Use Pointer Decryption Mode`，说明从 UE 4.23 起，
`FUObjectItem::ObjectPtr` 不是明文地址，而是经过引擎 XOR 混淆的。

**这直接解释了本轮枪战特训2（UE4）的三个反常现象**：
1. 精确搜"指向 coins 的指针" → 0
2. 精确搜"对象首址" → 0
3. 区域扫整张指针表 → 0

不是对象孤立，是**指针在内存里根本不是明文**。登山赛车是 Cocos，明文指针，
所以 `pointer_scan` 一抓就中。

> 状态：**推断，未在本机验证**。验证方法见下节。

### 3. UE4 游戏不该靠内存搜索，该走引擎反射

UE4Dumper 的核心思路：拿到 `GNames`（FName 池）+ `GUObjectArray`（对象数组）
两个全局指针，就能枚举所有对象、类名、属性偏移，直接生成 SDK。
`UE-Dumper` 更进一步，能**自动签名查找**这两个全局指针（games.ini 里存 sig）。

对比我们现在的做法（黑盒搜数值）：
- 搜数值：要人玩游戏制造变化，只能拿到"值"，不知道字段语义
- 引擎反射：直接得到 `AmmoReserve (int32, offset 0x50)` 这种带类型和名字的信息

**这就是本轮"改 400 不是射速"这个弯路的根因** —— 我们在猜字段含义。

## 三、可落地的优化清单

### P0（今天就能做，改动小）

1. **`search_range` 步进改 `i += sz`**
   - 依据：CE/ptrscan 都按元素宽度对齐扫
   - 现状：`agentd/search.c` mode 1 是 `i++`，一个 8 字节指针被命中 8 次
   - 收益：结果数 ÷8，cap 不再秒满，命中地址是对齐的真实位置

2. **`pointer_scan` 加 `lookback`**
   - 依据：ptrscan 的 `-p 0x500`
   - 做法：命中条件从 `v == target` 改成 `v >= target - lookback && v <= target + lookback`
   - 收益：一次抓出"指向该对象任意成员"的指针，不用每换一个偏移重扫全内存

3. **`pointer_scan` 加 `aligned`**
   - ARM64 指针天然对齐，非对齐命中基本都是二进制数据里的巧合
   - 本轮实测：`900/39` 相邻命中的 2 条里，1 条是非对齐地址（资源数据）

### P1（半天）

4. **`pointer_scan` 多级 `depth`**
   - 对上一级结果递归扫描，输出 `[[module+off]+0x10]+0x8` 形式的链
   - 参考 CE 的剪枝策略：只保留能通过偏移范围指回上一级的候选

5. **UE4 指针加密探测（实验性质）**
   - 做法：读一个已知对象槽位的 8 字节，检查它指向的地址是否 `mapped` 且首 8 字节像一个 vtable 指针
   - 如果大部分"指针"指向无效地址 ⇒ 确认加密 ⇒ 后续要么找 XOR key，要么放弃指针链改走 AoB

### P2（大工程，需评估）

6. **UE4 引擎支持模块**
   - 定位 `GNames` / `GUObjectArray`：签名扫描（参考 UE-Dumper 的 sig-finder）
   - 遍历 `FUObjectArray` → 按类名过滤 → 输出属性偏移
   - 收益：把"猜字段"变成"查表"，射速这类属性一次到位
   - 成本：UE 版本差异大（4.20~5.3 偏移全不同），需要版本识别 + 偏移表

## 四、许可证边界（重要）

| 项目 | 能否复制代码 | 说明 |
|---|---|---|
| scanmem（GPLv3） | **不能** | 传染性 GPL。算法/数据结构可以自己重写 |
| libscanmem（LGPLv3） | 可动态链接 | 若只在设备端以 .so 形式调用，不污染主工程；但静态链接不行 |
| Cheat Engine | **不能** | 混合许可，核心 GPL |
| ptrscan | 看 LICENSE | **参数设计和算法思路可以直接用**（属于思想，不受版权保护） |
| UE4Dumper / UE-Dumper | 看各自 LICENSE | 思路可借鉴；若真要移植代码必须先读 LICENSE |

**结论**：这三个方向（lookback、depth、对齐、反向索引）都是**算法思想**，
自己实现完全没有许可证问题。这也是为什么调研的重点放在"它们怎么设计接口和数据结构"，
而不是"把代码搬过来"。

## 五、下一步

优先做 P0 的 1、2、3（都在 `agentd/search.c`，一次提交能完成，且有真机可验证）。
P1-5 的指针加密实验建议紧接着做 —— 它能决定 UE4 这条线是继续追指针还是转 AoB。
