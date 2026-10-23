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

## 未完成：类名（GName）

有了对象数组只能拿到地址，看不懂"这个对象是什么"。要类名必须解析 FName，
而 FName 依赖 `FNamePool` 全局对象。

现状：
- `FNamePool::NumSlots` / `DebugDump` 有导出符号，但它们是**成员函数**，
  `this` 由调用方传入，静态拿不到全局 `GName` 的地址
- `.dynsym` 里**没有** `GName` 符号（只有 `GEngine`/`GEngineLoop` 等）

下一步可选：
1. **反汇编 `FNamePool::NumSlots`**，看它读 `this` 的哪些字段 → 得到 FNamePool 布局 →
   在 `.bss` 里按"8192 块指针数组 + 游标"的特征搜
2. **走 GEngine**（有导出符号 `0xaaaed68`）→ UEngine → World → 逐层拿对象，
   用 UObject 的 `Class` 指针 + `FName` 索引反查（仍然需要 FName 池）
3. 搜字符串 `"None"`（FNameEntry 的第 0 项）→ 反查谁引用它

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
