# 三角洲行动 ACE 云下发检测文件分析

> 分析日期：2026-08-22
> 对象：`com.tencent.tmgp.dfm`（三角洲行动，国服 1.201.x）`/data/data/.../files/ano_tmp/`
> 样本：`ai_workspace/samples/delta_re/cloud_dispatch/`（全部下发文件 + 解包产物）
> 触发：启动游戏时 ACE 云端动态下发（16:13 实测，Tenc-ano_tmp 模块开机删 ano_tmp 后重拉）
> 关联：ACE 总体机制见 `CF反作弊检测机制分析.md`；三角洲坐标加密见 `三角洲行动手游ACE分析.md`

---

## 一、结论速览

- **云下发检测点确认存在且活跃**：`files/ano_tmp/` 是 ACE 下发规则文件的落地区，libtersafe 通过 `mrpcs_download/scan/send` 三线程下载→扫描→上报。
- **下发文件全部带版本戳 + RSA 签名 + 内容加密/混淆**——ACE 检测规则可远程更新（移动靶），防篡改（改规则即识破），防静态分析。
- **`a64.dat`（QDF 专属载荷）已破译为明文 TLV 规则表**——非加密文件，是"魔数+字段类型+填充"的结构化数据，规则值明文可读。
- **`comm.dat` / `mrpcs` 结构已破译（修正）**：**双层嵌套加密 TLV** —— 外层 `[u32 flag=1][u32 len][外层加密体]` 加密，内层嵌套 `[类型标签][加密串]`（类别标记 `S_`/`]T_`/`QC_` 为明文，内层串体加密）。解析逻辑在 **`libtprt.so`**（非 libtersafe，libtersafe 只处理 a64.dat）。加密串 comm.dat 与 mrpcs 同源 → 单点破译可全解。
- **检测规则 = 动态武器库**：本地 susfs/TEESimulator-RS 修的是当前规则下的静态面，云端随时可下发新检测点。

---

## 二、下发文件清单与分级

| 文件 | 大小 | 结构 | 加密状态 | 作用 | 级别 |
|---|---|---|---|---|---|
| `ob_x.zip` → `a64.dat` | 252KB | 头部明文(BCD版本+QDF魔数) + TLV 规则表 | 明文结构化 | **三角洲专属检测载荷** | 核心 |
| `ob_x.zip` → `a64.sig` | 256B | RSA-2048 签名 | - | a64.dat 签名 | 核心 |
| `comm.zip` → `comm.dat` | 69.9KB | `tlf` 魔数 + 长度前缀表(0x8000块) | 字符串区单字节XOR | 通用规则包 | 核心 |
| `comm.zip` → `sig4`/`sig5` | 256B×2 | RSA-2048 签名 | - | comm.dat 签名 | 核心 |
| `mrpcs_a.data` | 19KB→60KB | ZIP 壳 + `unzipmrpcs.data` | 解压后仍加密(熵7.99) | 规则库(数据) | 核心 |
| `mrpcs_a_c.data` | 40KB→148KB | 同上 | 同上 | 规则库(config) | 核心 |
| `mrpcs_a_v.data` | 81KB→405KB | 同上 | 同上 | 规则库(version) | 核心 |
| `config2.dat.*` | 1KB | `20250307` + `]CBP` 魔数 | 加密 blob | 策略配置 | 辅助 |
| `config3.dat.*` | 1.9KB | 同上 | 加密 blob | 策略配置(libtersafe 直接引用 `config3`) | 辅助 |
| `atti_obv.dat` | 172B | `20241228` 版本戳 | 部分加密 | 属性混淆标记 | 辅助 |
| `kvcache8.dat` | 81B | `25012520` 版本戳 | 低熵 | key-value 缓存 | 辅助 |
| 小文件(`SpeedUpCCH`/`h_rcd`/`ano_app`/`tersafe.update` 等) | <100B | `20 21 xx xx` BCD 版本戳 | 明文 | 版本/计数标记 | 次要 |

**libtersafe.so 加载引用实锤**（字符串检索）：
```
mrpcs_download_data_thread_start_failed!
mrpcs_single_data_not_match!
mrpcs_data_crc_error
mrpcs_send_data_thread_start_failed!
mrpcs_data_len_error
mrpcs_common_data_not_match!
mrpcs_scan_thread_start_failed!
MRPCS_ANDROID
mrpcs_lib
%s/ano_tmp/mpmc.dat
a64.dat / a64.sig / a64.datPK
config3
```

---

## 三、`a64.dat` 破译（2026-08-22）

### 3.1 文件头部（明文）

```
0x00: 20 22 01 18        BCD 版本戳 = 2022-01-18
0x04: 00 00 00 07        计数/标志 = 7
0x08: 00 00 00 17        计数 = 23
0x0C: 51 44 46           魔数 "QDF"（Delta Force）
0x0F: 5c 6b 56 5a ...    后续字段（含编码）
```

### 3.2 TLV 记录结构（每条 15 字节）

```
[85 96] [4b 43 dc] [3B 载荷] [83 83 83] [14] [83 83 83]
 魔数    类型ID      载荷      填充       长度   填充/对齐
```

- `8596` = 记录魔数（明文常量）
- `4b43dc` = 记录类型 ID（明文常量）
- 载荷 3B：前 2B = **字段类型标记**，第 3B = **规则值（明文）**
- `838383` = 明文填充（0x83 占全文件 36.8%，是填充不是密文）
- `14` = 0x14 = 20 = 长度字段

### 3.3 字段类型枚举（每块内固定顺序）

| 字段序 | 类型标记 | 数据取值(明文) | 推测 |
|---|---|---|---|
| 1 | `70f2` | 8f/25/26/98/49/d4 | 规则 ID/哈希 |
| 2 | `b383` | 40/44/49/a0/8f | 特征值 |
| 3 | `b3b3` | 25/64/bb | 特征值 |
| 4 | `a11d` | 0f/b0/08 | 特征值 |
| 5 | `b319` | e6/5a/be/4f | 特征值 |
| 6 | `b31e` | 20/f1/2a/be | 特征值 |

### 3.4 块模板（规则条目）

相同模板在不同偏移完全复现 → **key 只依赖记录内位置**（实际为明文结构，无加密）：

```
块[0x04e3]:  70f28f b38340 b3b325 a11d0f b319e6 b31e20
块[0x364dc]: 70f28f b38340 b3b325 a11d0f b319e6 b31e20  ← 完全一致
块[0xd78c]:  70f28f b38340 b3b325 a11d0f b319e6        ← 同序列少尾字段
```

**结论：a64.dat 是明文 TLV 规则表，非加密文件。** 6 类字段类型 + N 条规则，规则值明文可读。

### 3.5 破译过程要点（可复用）

1. `0x83` 占 36.8% → 初判为加密填充，实为**明文填充符**
2. 高频 3B 密文 `85964b43dc` 间隔 GCD=4 → 误判 4B 周期 key
3. **决定性**：`85964b43dc` 位置间隔恒 15/16 → 识别为**定长记录表**
4. 记录序列模板跨偏移完全复现 → 推翻"流密码"，确认明文结构
5. 坑：单字节 XOR 爆破出的"高可打印率"是假象（头 0x00 XOR 0x60 变 '`'）

---

## 四、`comm.dat` 与 `mrpcs` 结构破译（2026-08-22）

### 4.1 comm.dat —— 双层嵌套加密 TLV（修正 2026-08-22 16:50）

**头部（明文）**：
```
[0x00] u32 = 3          魔数
[0x04] "tlf\0"           魔数
[0x08] 索引/计数表       每 8B: [4B 偏移][4B 长度]（指向字符串池）
[0x200] 字符串池起点      加密数据区
```

**真实结构（嵌套 TLV，非"前缀+控制字节"）**：
```
[00 00 00 01] [00 00 00 LL] [外层加密体] \0
   flag=1         len          密文
```
外层加密体内嵌套：
```
[类型标签] [加密串] ... [S_] [1d] [内层加密串] ...
  08/06/01/02      ^W..DI    类别标记  内层密文
```

**实测证据**（第一条 `len=79` 记录）：
```
08 1f 42 4b 40 40 50 5b 18 ... 53 5e 5f 1d 58 57 53 19 4b 5c 22 37 31 2d 31 3f ...
│                    外层加密体                  │S_│ 内层标签│ 内层加密串
```
- `08`/`06`/`01`/`02` = 外层类型标签（嵌套层标识）
- `53 5e 5f` = `S_`（明文类别标记，嵌套在加密体内）
- `1d` = 内层类型标签
- `58 57 53 19 4b 5c 22 37 31 2d 31 3f` = 内层加密串（流密码）

**修正结论**：
- `S_`/`]T_`/`QC_` 不是"前缀+控制字节"的明文前缀，而是**嵌套在加密体内的类别标记**（部分明文）
- 加密是**双层**：外层整块加密 + 内层串体加密，类别标记 `S_` 在两层之间是明文缝隙
- 已知明文攻击：`S_` 后第一条内层串 `1c 5e 57 04 18 44 49` 假设明文 `mrpcs` → 推导 key=`33 25 74 7b 37`，解密尾部 `49^33=7a('z')` 可读，但**不同串 key 不同** → 确认是**位置相关流密码**（非固定 key，非单字节 XOR）
- 解析逻辑在 **libtprt.so**（含 `tlf` 魔数 @0x15b83f），**不在 libtersafe.so**（libtersafe 仅处理 a64.dat/QDF 载荷）

### 4.2 mrpcs 三件套 —— 同一加密算法的规则全文库

**外壳**：ZIP（deflate）→ 条目 `unzipmrpcs.data`
**解压后熵**：4.7-4.8（非满熵 → 非强加密，是**低熵压缩/编码数据**，含大量重复结构）

| 文件 | 解压后大小 | 熵 | 说明 |
|---|---|---|---|
| `mrpcs_a.data` | 60KB | 4.764 | 规则库主本 |
| `mrpcs_a_c.data` | 148KB | 4.852 | 与 _a 内容高度雷同 |
| `mrpcs_a_v.data` | 405KB | 4.715 | 与 _a 内容高度雷同 |

**关键发现**：三文件解压后字符串几乎完全相同（`hP8V`/`R!9!Ru!Raq`/`_*+++` 在三文件重复出现）→ **`_c`/`_v` 不是 config/version 双轨，而是同一规则库的冗余副本**（灰度下发容错/分片）。

**头部魔数**：`bf49abcd`（_a）/ `3359b211`（_c）/ `0b0773e0`（_v）—— 自定义魔数（可能含版本+CRC）。

### 4.3 统一加密体系结论（修正）

```
ACE 下发文件加密算法 = 双层嵌套 TLV + 位置相关流密码
  ├─ a64.dat: 明文 TLV (无加密, 规则值直接可读)  ← 例外: 规则表结构本身
  ├─ comm.dat: 双层加密 TLV (外层整块 + 内层串体, 类别标记缝隙明文)  ← 规则摘要
  └─ mrpcs:   加密压缩 (同算法, 规则全文)         ← 规则库
```

**三者的加密串完全同源**（`(3,.=-`/`R!9!Ru!Raq%Q)]R)ym`/`hP8V`/`_*+++` 在 comm.dat 和 mrpcs 同时出现）→ **统一算法，单点破译即可全解**。

**破译路径修正**：解密逻辑在 **libtprt.so**（含 `tlf` @0x15b83f、`a64.dat` @0x166636），**非 libtersafe.so**。逆向 libtprt.so 的 `tlf` 引用函数 → 提取 key 派生逻辑（位置相关流密码）→ 还原明文规则。这是下一步（待办）。

### 4.4 破译过程要点（可复用）

1. comm.dat 初判"字符串区单字节 XOR" → 实为**双层嵌套加密 TLV**（外层整块 + 内层串体，类别标记缝隙明文）
2. comm.dat 初判"前缀+控制字节流密码" → 实为**嵌套在加密体内的类别标记**（`S_` 非独立前缀）
3. mrpcs 初判"熵 7.99 强加密" → 实为 deflate 解压后 4.7 低熵（解压步骤被遗漏）
4. 三文件"config/version 双轨"误判 → 实为**冗余副本**（内容同源）
5. 解密模块初判"libtersafe.so" → 实为 **libtprt.so**（含 `tlf`/`a64.dat` 字符串），libtersafe 仅处理 a64.dat/QDF 载荷
6. 决定性：comm.dat 与 mrpcs 加密串交叉匹配 + libtprt.so 含 `tlf` → 确认统一算法及解析者

### 4.5 解密函数逆向：位置相关流密码 key 派生（2026-08-22 17:30）

**目标**：提取 comm.dat 加密串的 key 派生逻辑（逆向 libtprt.so 中引用 `tlf` 的解密函数）。

**已完成的静态工作**：

1. **ELF 解析与字符串定位**：
   - `tlf` 字符串在 libtprt.so 文件偏移 `0x15b83f` → VA `0x16383f`（同页还有 `DLS_`、`shell_cast2`、`incomplete literal/length tree`(zlib 错误串)）。
   - 全 exec 段扫描 `adrp` 引用该页基址 `0x163000` 的指令，共 **399 处**；按 `ret` 边界切分函数得 **189 个含 tlf 引用的函数**。
   - 含"回边循环 + ldrb + strb"的函数 **44 个**，逐一反汇编后确认它们是 **parser/构造器/PLT 转发**（如 `0x121264`/`0x121470` 调用 `0x12029c`(inflate)、`0x121164`、`0x158290`(PLT→外部导入)），**不是字节级解密核心**。

2. **RC4/PRGA 特征搜索**：全 so 搜 `LDR/STR Wt,[Xn,Wm,UXTW #2]`（S 盒 4 字节缩放访问，RC4 KSA 强特征），仅 11 处，全部落在 `0x164330` 起的混淆主函数（~200KB，含 TPRT 自保护/CRC）。单点特征不足以定位解密叶子。

3. **已知明文攻击（纯数据分析）**：
   - comm.dat 结构 = `[u32 len][cat 2-4B 明文][ctrl 1B][cipher N B]`；ctrl 按类别固定（S^_=0x1d / WP_=0x56 / QC_=0x51 / ]T_=0x50,0x5c / S_ 混合 0x1c,0x67,0x69,0x58,0x5b）。
   - 首条 S_ 串（ctrl=0x1c，cipher=`5e 57 04 18 44 49`）假设明文 `mrpcs` → keystream=`33 25 74 7b 37 49`（与先前会话日志一致）。
   - 该 keystream 套用其他 ctrl=0x1c 条目 → **乱码**，证实 **keystream 非 per-ctrl 固定，而是 per-entry 位置相关**。
   - 全量交叉验证：S^_（118 条，同 ctrl=0x1d）用任意明文前缀假设，keystream 分布高度分散（112 条 → 86 种 keystream[0:8]），**无可辩驳地确认 key 依赖 entry 级 salt（非 ctrl 独占，非固定常量）**。
   - 相同 keystream 组的条目标志性偏移无规律（如 `0x2ff,0x17d1,0x2524,0x427d...` 非等距）→ **key 不依赖简单文件偏移**。

4. **排除项（已实证）**：
   - 固定单字节 XOR：所有候选 key 解密首字符全乱码 → 排除。
   - 标准 PRGA（XORSHIFT32 / LCG(Numerical Recipes, glibc, MSVC) / mulberry32）：以 entry0 keystream 为锚枚举 seed（各 2^20~2^24）**0 命中** → 排除标准算法，为**腾讯自研流密码**。
   - key 常量表：在 libtprt.so 全文搜索 keystream/密文片段 → **NOT FOUND** → key 运行时派生，非查表。

**结论**：comm.dat/mrpcs 加密 = **per-entry 独立密钥的腾讯自研流密码**，key 由 `(ctrl, entry 级 salt)` 派生，salt 来源可能是 comm.dat 头随机数 + per-entry 计数器（非明文偏移）。静态逆向因主逻辑函数混淆、无常量 key、非标准 PRGA 而收益递减。

**剩余工作（动态提取，设备恢复后执行）**：
- frida/gg 钩 libtprt.so 解密函数（特征：入参 cipher+len+ctrl，出参 plaintext），dump `(ctrl, salt/offset, cipher, plain)` 对。
- 用 dump 对反推 `key = f(ctrl, salt)` 闭式；salt 来源通过对比明文对的 salt 差与 comm.dat 结构字段定位。

---

## 五、对规避面评估的影响

1. **检测规则可远程更新**：susfs/TEESimulator-RS 修的是"当前下发规则下的静态检测面"，云端可下发新检测点（含新 so：`a64.dat` 可能是检测 so 的规则表）。
2. **RSA 签名**：改下发文件 = 触发 `mrpcs_data_crc_error` 类检测，**别动它们**。
3. **Tenc-ano_tmp 模块（游戏优化助手）**：开机 `rm -rf /data/data/*/files/ano_tmp`，游戏启动时 ACE 重新下载——删除动作在服务端视角为正常网络行为，无检测痕迹；但它同时执行"设备标记文件锁权限/Kill root 工具进程"，需留意。

---

## 六、样本与待办

- 样本：`ai_workspace/samples/delta_re/cloud_dispatch/`（a64.dat 已解包、comm.zip 已解包）、`ai_workspace/samples/delta_re/so/`（libtprt.so 等）
- 待办：
  1. **[已推进]** 解密函数静态逆向完成：确认位置相关自研流密码、per-entry key、排除固定/标准 PRGA、key 不在常量区。→ **剩余：动态提取 key 派生**（设备 134d2f8 恢复后 frida hook 解密函数 dump 明文对）。
  2. 对照 `a64.dat` 字段类型与 libtersafe 规则解释器 → 还原每条规则语义
  3. 设备重连后抓新下发文件做版本 diff
  3. 下次游戏启动抓取新下发文件 → 与本次基线 diff（观察规则更新）
