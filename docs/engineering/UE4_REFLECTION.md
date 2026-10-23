# UE4 反射定位实测（不装 frida）

> 2026-10-23。目标：用 gg 的内核读写 + AI 现场写的脚本，完成社区工具
> （frida-ue4dump / UE4Dumper）干的活 —— dump UE4 的对象与属性。
>
> 结论先行：**对象系统已打通（能枚举 15003 个 UObject），只差类名（GName）。**

## 为什么不用 frida

- 设备没装 frida-server，本机也装不上（GitHub / pypi 都连不上）
- 更重要的：frida 的三件事我们都能用别的方式做

| frida 能力 | 我们的替代 | 状态 |
|---|---|---|
| `Module.findExportByName` | `gg_module_base` + llvm-readelf 查 .dynsym | ✓ |
| `Memory.scanSync`（带 `??`） | `gg_search(type=bytes)` —— 本轮补的通配 | ✓ |
| `Instruction.parse`（ADRP/ADD） | `scripts/arm64_globals.py` 自己解码 | ✓ |

## 已完成

### 1. GName 的入口函数不是 GName（重要教训）

`_Zeq12FNameEntryId5EName`（0x662CA64，导出符号）反汇编：

```asm
adrp x8, 0xa8f7000
add  x8, x8, #0x940       ; x8 = 0xA8F7940
add  x8, x8, w1, uxtw #2  ; + (id << 2)
movk w9, #0x1, lsl #16    ; w9 = 0x18040
ldr  w8, [x8, x9]         ; 读 [0xA8F7940 + id*4 + 0x18040]
cmp  w8, w0
```

frida-ue4dump 的脚本 hook 这个函数 +0x8 读 x8，把 `0xA8F7940` 当 GName。
**但实测 `0xA8F7940` 读出来是 0**，而 `0xA8F7A00` 有数据。

真实含义：这是 **NameHash 表**（比较 FNameEntryId 的哈希），`+0x18040` 才是数据区。
**它不是 FNamePool 的入口**，frida 那条 hook 只对特定游戏成立。

### 2. GUObjectArray 打通（核心成果）

**签名**（frida Android 默认）：

```
e1 ?? 40 b9 e2 ?? 40 b9 e3 ?? 40 39
```

**离线扫 libUE4.so**：唯一命中，文件偏移 `0x686F80C`
（python 复现 `Memory.scanSync`，见 `scripts/arm64_globals.py` 的兄弟脚本思路）

**运行时验证**（`gg_search` 带通配）：

```
libUE4.so base = 0x6D1647C000
签名命中     = 0x6D1CCEB80C = base + 0x686F80C   ← 完全一致，462ms
```

**算全局地址**（`scripts/arm64_globals.py 0x686F7F0`）：

```
0x686F818  adrp+ldr   0xA76B028   ldr x0,[x0,#0x28] 取指针内容
```

**运行时读取**：

```
base + 0xA76B028  ->  0x6D20DB7C18   (GUObjectArray)
```

**FUObjectArray 结构**（实测）：

| 偏移 | 值 | 含义 |
|---|---|---|
| +0x00 | 15003 | ObjFirstGCIndex（对象总数） |
| +0x04 | 15002 | ObjLastNonGCIndex |
| +0x08 | 15003 | MaxObjectsNotConsideredByGC |
| +0x10 | `0x6D23F39DE0` | ObjObjects.Objects（块指针数组） |

**对象数组**：

```
0x6D23F39DE0 (chunk 指针数组)
  chunk[0] = 0x6CF9000008
    [0] object=0x6CFB601B40  flags=0x40000000   ← CDO，符合 UE4 惯例
    [1] object=0x6D327E0B00  flags=0x42000000
```

`FUObjectItem` = 0x18 字节：`{ UObject* Object; int32 Flags; int32 ClusterRootIndex; int32 SerialNumber; }`

**到这里，"枚举所有 UObject"已经能做了。**

## 未完成：类名（GName）—— 四条路都试过了

有了对象数组只能拿到地址，看不懂"这个对象是什么"。要类名必须解析 FName。

**试过的四条路，全部不成立**（2026-10-23 18:20-18:40）：

| 路线 | 做法 | 结果 |
|---|---|---|
| 1. 查符号 | `.dynsym` 里找 `GName` | **没有**。只有 `GEngine` / `GEngineLoop` / `FNamePool::NumSlots` / `DebugDump` |
| 2. 反汇编成员函数 | `FNamePool::NumSlots`(0x662c5d0) / `DebugDump` | 它们是**成员函数**，`this` 由调用方传入 —— 反汇编只能看布局，拿不到全局 `GName` 的地址 |
| 3. 扫 `.bss` 找块指针数组 | `.bss` = 0xA81A6C0 + 0x3650F0；找"连续 3+ 个 qword 落在堆范围" | **6303 个候选** —— `.bss` 里到处是堆指针，特征太弱，无法区分 |
| 4. "None" 反查 | 搜 `4e 6f 6e 65`，期望找到 FName 池第 0 项，再反查谁指向那个块 | 542 命中：**堆里 519 个全是普通字符串**（`None-Match`、`RSA/None/NoPadding`、`e\0None\0OnD`），没有一个符合 `FNameEntryHeader` 布局 |

第 4 条失败的细节：按 UE 4.23+ 的布局
`{bIsWide:1, LowercaseProbeHash:5, Len:10}`，`"None"` 应该是
`len=4, wide=0`，即 `(header>>6)&0x3FF == 4`。实测 542 个 "None"
**没有一个满足**。说明这个游戏要么用非标准 FName 布局，要么池还没按这个形式建起来。

**结论：这个游戏的 GName 不能靠"通用套路"拿，得逐游戏摸。**

## 替代路线（不依赖类名）

拿不到类名不等于做不了事。三条可行的替代：

1. **行为特征识别玩家**：玩家 Pawn 的坐标会随移动变化。
   模糊搜索 `unknown_init` → 让人走两步 → `refine changed`，收敛出来的就是坐标；
   再从这个地址反推所属对象（`pointer_scan lookback=对象大小`）。
   **不需要知道类名，只需要知道"它在动"。**

2. **走 GEngine（有导出符号 `0xaaaed68`）→ UWorld → PlayerArray**
   UE4 的对象图是版本固定的，偏移要试但结构确定：
   `UEngine::GameViewport → UWorld → GameState → PlayerArray → Pawn`。
   比啃 FName 池更直接。

3. **按 Class 聚类 + 数量特征**：玩家类实例数很少（训练场 1 个、对战几十个），
   而道具/资源类成百上千。78 个类里实例数 ≤ 10 的那批就是候选。

## 今天实际拿到的数据（枪战特训2）

| 项 | 值 |
|---|---|
| UObject 总数 | 15003 |
| 类数量（抽样 323 个） | 78 |
| 类分布 | 134 / 48 / 33 / 18 / 13 / 4 / 2 / 1×N |
| Class 指针在数组内 | 78/78（可顺着 Class 追 FName） |
| 坐标实例 | 对象 `0x6D101ACA00` 的 `+0x28C` = `(-2439.7, -7797.0, 984.3)` |
| 属性结构 | 备弹 `+0x14`、上限 `+0x50`、`{id,type,value,-1}` 属性槽 |

**偏移按类不同**（抽样 80 个对象扫 `+0x28..+0x400`，每个偏移都只出现 1 次），
不存在"一个全局坐标偏移通吃"这种事。

## 工具侧产出（本轮）

| 产出 | 说明 |
|---|---|
| `gg_search` 支持 `??` / `?X` / `X?` | 社区签名全部可用（commit d42c0a9） |
| `scripts/arm64_globals.py` | ADRP+ADD/LDR 解码，两个案例验证通过 |

`arm64_globals.py` 两个验证：

```
$ python scripts/arm64_globals.py build/libUE4.so 0x662ca64 --count 10
0x662CA68  adrp+add  0xA8F7940     ← 与 objdump 手工分析一致

$ python scripts/arm64_globals.py build/libUE4.so 0x686F7F0 --count 14
0x686F818  adrp+ldr  0xA76B028     ← 同上，且运行时验证到了真实指针
```

## 方法论沉淀

1. **签名 → 命中 → 反汇编 → 全局地址 → 解引用**，这条链不需要 hook，
   不需要注入器，只需要"读内存" + "解码指令"两个原语。
2. **社区脚本里的偏移不可信**：frida 那条 `0xA8F7940` 在这个游戏上就是错的，
   必须自己验证（读出来是不是合理值）。
3. **`dyn-syms` 里没有的符号，不代表不存在**，可能只是没导出 ——
   这时用签名，别用符号名猜。

---

## ✅ GWorld 路线打通（19:10 实测，枪战特训2）

前面那套 GName 全是弯路。看用户自己的 `esp-android` 仓库文档才发现，
UE4 拿实体走的是 **GWorld → ULevel → Actor**，根本不需要 FName。

### 实测偏移（只对本版本成立）

```
GWorld        = libUE4.so + 0xAAB2D50      （.dynsym 有导出，直接查）
UWorld + 0x30 = ULevel
ULevel + 0x98 = TArray<AActor*> data
ULevel + 0xA0 = TArray count
AActor + 0x2D0 = FVector 世界坐标
```

### 实测结果

```
libUE4.so   0x6D47A5A000
GWorld      0x6D3179B890
ULevel      0x6D2A248440
Actors      data=0x6D10CC9B10  count=66
其中 13 个 Actor 有有效世界坐标：
  0x6D13164C00   ( 1405.4, -3824.9,  650.1)
  0x6D13165100   (-3395.9, -5342.9, 1329.7)
  0x6D1B02C900   (-2545.0, -7296.3,  913.7)
  ...
坐标分布 X∈[-4421,1546] Y∈[-7681,1728] Z∈[576,2179]，符合地图尺度
```

### 怎么重扫这些偏移（换游戏必做）

| 目标 | 方法 |
|---|---|
| GWorld | `.dynsym` 搜 `GWorld`（有导出就一步到位），没有就 Pattern 扫 |
| UWorld→ULevel | 扫 UWorld 前 0x100 的指针，挑"内部含 TArray、data[0] 指向堆"的那个 |
| Actors | 在 ULevel 前 0x200 找 `(data_ptr, count)` 相邻组合，且 data[0] 有效 |
| 坐标 | 遍历所有 Actor，统计哪个偏移在最多 Actor 里出现"合理三元组"（实测 +0x2D0 命中 13/66，次高只有 7） |

### 关键教训

**文档里暗区突围的偏移（GWorld 0xf429248、坐标 0x128、血量 0xD18）在这个游戏上全不适用。**
换游戏必须重扫，但**链路结构是通用的**。

工具：`scripts/ue4_esp_probe.py`（一条命令输出 Actor 列表 + 坐标）。

### 还差

- **ViewProjection 矩阵**（W2S 必需）：文档给的是 `libUE4 + 0xf3dac88 → +0x20 → +0x2A0`，本游戏要重找
- 血量/阵营：属性偏移逐个逆
