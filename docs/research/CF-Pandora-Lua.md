# CF Pandora SDK Lua 逆向（_G 全局表定位）

> 日期：2026-08-20 | 状态：**_G 定位成功（实锤）**
> 配套脚本：`ai_workspace/scripts/cf_pandora_*.py`（pandora_disasm 反汇编 / globals3 批量读 / globals5 字符串反推 / g_dump 全链 dump）

## 一、为什么是 Pandora

- CF 游戏逻辑跑在腾讯 **Pandora SDK 自带 Lua**（libpandora.so，479KB，585 导出，类型 `pua_State`），**不是** libxlua（影子，方法区全 udf）。
- C# 侧是 xLua 桩（`<>xLuaBaseProxy_*`），真实玩家状态/游戏逻辑全在 Lua 表。
- **铁律不变**：绝不 gg_attach（注入=封号），全程内核只读直读。

## 二、全链（全部运行时实锤，2026-08-20）

```
LuaSvr C++ 实例 (luaState@0x10)
  -> pua_State = 0x7b02dedb50        (主状态, 特征扫描命中)
  -> registry Table = 0x7ab27ed660   (L+0x160 槽)
  -> _G 全局表 = 0x7ab27eded0        (L+0x78 槽, 自引用铁证)
```

| 对象 | 地址 | 验证 |
|---|---|---|
| pua_State | 0x7b02dedb50 | alloc@0xd0=libpandora+0x1bd5c + 自指@0x170 |
| registry | 0x7ab27ed660 | C# 类型注册表（UnityEngine.*/com.tencent.* 256 node）|
| **_G** | **0x7ab27eded0** | `_G._G == 自己` + print/require/pairs + 游戏模块全在 |

## 三、pua_State 布局（0x270B，pua_newstate@0x2e228 反汇编）

| 偏移 | 字段 | 说明 |
|---|---|---|
| +0x00 | GCObject next | GC 链 |
| +0x08 | tt=8 / marked | 头 |
| +0x10 | top | 栈顶（TValue* 或索引）|
| +0x18 | base | 栈底 |
| **+0x20** | **-> L+0xc0** | **l_G 内嵌结构指针（自引用）** |
| +0x28 | ci 数组尾 | CallInfo 数组结束指针（步长 0x28）|
| +0x38 | 当前 func 槽 | 栈相关 |
| +0x40 | stack 基址 | |
| +0x50 | ci 数组基址 | |
| +0x78 | **_G 槽** | `{value=0x7ab27eded0, tt=5}`（getfield -10002 分支）|
| +0x88 | 模块 env 缓存 | `{value=package表0x7ab27eedd0, tt=5}`（getfield -10001 分支）|
| +0xc0 | strt.hash | stringtable 哈希数组 |
| +0xc8/+0xcc | strt.nuse/size | |
| +0xd0/+0xd8 | allocator/ud | pua_newstate 参数 |
| +0x160/+0x168 | **registry TValue** | {value, tt=5} |
| +0x170 | 自指 | = L 自身 |
| +0x1a0 | metatable 区 | 0x48B |

> 修正历史认知：registry 在 **L+0x160**（=[L+0x20]+0xa0=L+0xc0+0xa0），此前一度误判 l_G 是外部对象；[L+0x20] 实际指向 L 内部 L+0xc0。

## 四、Table / Node / TString 布局（Pandora 紧凑魔改版）

### Table（0x40B 头，luaH_get@0x31474 反汇编）

| 偏移 | 字段 | 标准 Lua 5.4 |
|---|---|---|
| +0x0b | lsizenode（1B）| +0x11 |
| **+0x10** | **metatable 指针** | +0x30（前移！）|
| +0x18 | array（TValue*, 16B/元素）| +0x18 ✓ |
| +0x20 | node（Node*, **0x28B/元素**）| +0x20 ✓ |
| +0x28 | lastfree | +0x28 ✓ |
| +0x38 | alimit（u32）| +0x14（后移！）|

### Node（0x28B = 40B，标准 32B）

```
+0x00  value.value (8B)
+0x08  value.tt   (4B)
+0x10  key.value  (8B)   ← 字符串 key 时 = TString 指针
+0x18  key.tt     (4B)   ← 4=string
+0x20  next       (8B)   ← 独立 next 指针（标准版在 tt 高位）
```

### TString（0x18B 头 + data）

```
+0x00  next（strt 链）
+0x08  tt=4 / +0x09 marked
+0x0c  hash（u32）← 标准 luaS_hash(seed=0)
+0x10  len（u64）
+0x18  data[]
```

### 字符串哈希（重要，2026-08-20 验证）

**标准 Lua 公式，seed=0**：`h ^= ((h<<5) + (h>>2) + byte)`，尾部每 `(len>>5)+1` 步取 1 字节，初值 h=len。
（曾误读 0x2e690 反汇编为 `h=(h*33+b)^h`，用存储 hash 对比后纠正——**务必用 TString+0xc 存储值验证**。）

## 五、伪索引体系（pua_getfield@0x18c04 反汇编）

| idx | 含义 | 取址逻辑 |
|---|---|---|
| -10000 (0x2710) | registry | `[L+0x20]+0xa0` = L+0x160 |
| **-10001 (0x2711)** | 模块 env（L+0x88 缓存）| `[L+0x28]->+8->[0]->+0x18` 存 L+0x88 |
| **-10002 (0x2712)** | **_G 槽** | `L+0x78` |

> 注：`pua_getglobal`/`pua_getregistry` 未导出；内部无代码调 pua_getfield（VM 解释器直访表）。_G 定位最终走**字符串反推法**（见下）而非伪索引。

## 六、_G 定位方法（字符串反推法，可复用）

```
1. 哈希定位 TString: 本地算 "print" 哈希 -> strt.hash[slot] 链上找到 TString=0x7b12e5c380
2. 搜引用: SEARCH_RANGE(min=max=TString, type=4) -> 3 个引用
3. 找 node: 0x7b429bcf50 = {value=print函数0x7b93095b50, tt=6, key=print TS, tt=4}
4. 反推表: node 桶索引 = [TS+0xc] & ((1<<lsizenode)-1) -> 枚举 lsizenode
   -> 桶=70 -> node 基址候选=0x7b429bc460 -> 搜引用 -> 命中 Table+0x20 字段
   -> Table = 0x7ab27eded0（=_G！lsizenode=8 n=256）
5. 验证: 表含 "_G" 自引用键 + print/require/pairs + 游戏模块
```

## 七、_G 内容（174 键，dump 见 cf_g_dump.tsv）

- **标准库**：os/debug/string/table/math/io/coroutine/bit/package + print/require/pairs/ipairs/type/tostring/tonumber/assert/pcall/xpcall/load/loadstring/loadfile/collectgarbage/gcinfo/getfenv/setfenv/module/unpack/next/rawequal/rawset/newproxy
- **游戏全局 `gb_*`**：gb_account(账号str)/gb_myCur(2.0)/gb_myMax(1.0)/gb_total(7.0)/gb_whole(2.0)/gb_curre(0.0|1.0|表)/gb_redPo/gb_share/gb_pageD/gb_shopH/gb_today/gb_subPa...
- **游戏模块表**：APICtrl/GameBook/HallLoop/MainPage/UserCent/CardCtrl/RecordDe/Logger/JsonMana/FlowData/StatsRep/LimitedT/ActSetPa/FullVide/WatchGam/LifeCirc/TableToo/SingleCh/MultiChi/Common/SetAdapt/BaseExt*/Yingyong/cfm2024M...
- **Pandora 特殊**：com/UnityEng/System/this/userData/import/include/__LuaDel/__main_s(=pua_State)/_VERSION

## 八、搜索技术栈（本轮踩坑固化）

- **SEARCH_RANGE**: `{min: 值, max: 值, type: 4, region: 3}` 搜精确 qword 引用（十进制！type=4 必需，type=0 报错）。GET_RESULTS 大分页 + CLEAR_RESULTS。
- **批量读**：一次 READ 拉 0x8000（server 自动分段），比逐字段小读快 ~100 倍（9 分钟 → 数秒）。**严禁逐 node/逐字段小读**。
- pua_State 特征扫描（alloc@0xd0∈libpandora+0x1000 起 + 自指@0x170 + registry@0x160 tt==5）约 8 分钟，可用已知地址跳过。

## 九、下一步

- [ ] `gb_*` 变量语义确认（gb_myCur/gb_myMax/gb_total 疑似局内状态）
- [ ] 游戏模块表深挖：APICtrl/GameBook/HallLoop/MainPage/UserCent 内部结构
- [ ] 定位实时玩家数据（坐标/视角）→ 实体表可能挂在某模块表上
- [ ] Pandora VM 解释器（OP_GETTABUP）反汇编，摸清 getglobal 直接路径
