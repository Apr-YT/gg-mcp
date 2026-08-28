# CF（穿越火线手游）反作弊检测机制逆向分析

> 分析日期：2026-08-19
> 对象：CF 手游 Android 版（com.tencent.tmgp.cf，版本 860+，base.apk 2.1GB，98 个 so）
> 方法：静态逆向（radare2/llvm-readelf/字符串分析）+ 设备动态验证（驱动无痕读取/进程观测）
> 目的：理解腾讯 ACE 检测机制，为工具侧反检测提供依据

---

## 一、总体架构（4 层检测）

```
┌─ 用户态进程层 ──────────────────────────────┐
│  libturinggame.so   TuringFD 风控（root 检测） │
│  libtersafe.so      TP2 核心（采集/上报）       │
│  libtprt.so         TPRT 运行时（hook 机制）    │
│  libgamemaster.so   QPP + SubaoProxy（hook 引擎）│
│  estPlugin 进程     用户态反作弊服务            │
├─ 内核层 ────────────────────────────────────┤
│  TP2 内核模块 adt.s3.t（服务端签名下发才加载）   │
│  apr_dlkm（本工具驱动，已伪装）                │
├─ 服务端 ────────────────────────────────────┤
│  风控判定 + 封号执行 + 检测规则动态下发          │
└─────────────────────────────────────────────┘
```

## 二、检测组件清单

| so | 角色 | 关键证据 | 检测内容 |
|---|---|---|---|
| libturinggame.so (240KB) | TuringFD 风控 | root 正则全文（见下）、`/sys/fs/selinux/enforce` | root/模拟器/SELinux/挂载/路径 |
| libtersafe.so (6.3MB) | TP2 核心 | `tp2_sdk_init/ioctl/setuserinfo`、`tss_get_report_data` | 采集上报、环境指纹 |
| libtprt.so (2.1MB) | TPRT 运行时 | `g_tprt_ori/pfn_array` 定义、`TssSDKIoctl` 实现、mprotect/strcasestr | hook 替换机制 |
| libgamemaster.so (685KB) | QPP + 迅游 | `is_hook_stable@0x32d50`、`is_hook_module@0x31abc`、`ProtectFD@0x1e658` | 进程内模块 hook 检测、fd 保护、节点检测 |
| libil2cpp.so (202MB) | 游戏主库（内嵌 TssSDK） | `TssSDKInit/InitEx/SetUserInfo` P/Invoke 明文绑定、`tp2_sdk_ioctl` | TP2 客户端集成 |
| libckguard.so (18KB) | CKeyGuard | `/proc/%u/cmdline` | 进程名读取 |
| libqimei/libtdmqimei.so | 设备指纹 SDK | — | 设备唯一标识采集 |
| estPlugin 进程 | 用户态反作弊 | — | 进程/环境监测 |

**排除项**：libsaf.so = 音频效果/采集库（safEffect*/safcCapture*）；libtgpa.so = MGPA 振动 SDK（Haptic*）；libPxKit3 = PIX 容器/JS 引擎。

## 三、核心检测机制详析

### 3.1 TuringFD root 检测正则（完整提取 @0x31bdb）

```
^/system/xbin/ku[.]sud$              KernelSU su 守护
|^daemonsu:                          SuperSU
|^k_worker/[1-9][0-9]*:[0-9]+$       KernelSU 内核线程 k_worker
|^kr_worker/[1-9][0-9]*:[0-9]+$      KernelSU 内核线程 kr_worker
|^km_worker/[1-9][0-9]*:[0-9]+$      KernelSU 内核线程 km_worker
|^tworker/[1-9][0-9]*:[0-9]+$        其他 root 工具线程
|^tu_worker/...|^tq_worker/...
|^kworker/[1-9][0-9]{2}$             伪装 kworker 的 root 线程
|^permmgrd$|^360sguard$              权限管理/360 安全
|^/data/data/[alnum_.-]+/            /data/data 可疑进程
|^/system/bin/[.]|^/system/xbin/[.]  隐藏文件
|^[.]                                点开头隐藏名
```

**遍历方式**：枚举 /proc 全部进程/线程，匹配 cmdline/comm。

### 3.2 TssSDK/TP2 架构

- **客户端**：libtersafe.so（tp2_sdk_init → 采集 → tss_get_report_data 上报）
- **内核模块（ACE 唯一内核态检测组件）**：`adt.s3.t`
  - 释放路径：字符串实锤 `/data/data/%s/files/adt.s3.t`（libtersafe @0x495860，fopen 函数 `fcn.0016a814`，加载链 `fcn.00168dbc` 混淆控制流）。
  - 后缀 `.t` **非标准 .ko**——疑似加密/压缩格式，需专用 loader 解密后再 insmod（解释"加载后自删"）。
  - 用户态桥：`tp2_sdk_ioctl` @0xdb094（4184B）/ `TssSDKIoctl` @0x17f3c0（libtprt）——向内核模块发 ioctl；**设备节点名未在字符串硬编码（动态拼接）**。
  - 加载机制实证（2026-08-19）：libtersafe **不自行 insmod**——全串检索 `finit_module`/`init_module`/`request_module`/`insmod` **均 0 命中**，且**无硬编码 /dev/ 节点**。真正 insmod 由更上层（Java/独立 loader）完成，**当前版本未触发**。
  - 协议：ioctl + magic（`MagicCodeWrong`）+ 签名（`TSS_SDK_SIGDATA`/`tss_recv_sec_signature`）+ `time_magic`。
  - **服务端签名下发才加载**（`TssSDKOnRecvSignature`）。
- **实测结论（2026-08-19，两份 lsmod 抓包）**：大号/小号/全新进程/设备重启后，**adt.s3.t 均不加载**。tp2_watch3.sh（1800s×1s 监控，grep `tp2|adt|tss|tprt`）**零命中**；`lsmod_full_now.txt` 663 模块集合大号 vs 小号 **100% 一致**。设备上内核态与"检测"相关的仅有 `kernelsu`（你的 root 方案，被 TuringFD 检测但已规避）与 `apr_dlkm`（本工具驱动，ACE 无内核端故读不到）。当前 CF 版本在 root/KernelSU 设备上**纯用户态检测**。

### 3.3 libgamemaster.so hook 引擎（QPP）

- `is_hook_stable`：`getpid()` → `enum_process_module()` 枚举自身进程模块 → 与基线（hook_init 时缓存 g_lm）对比**数量 + 每模块 +0x200 指针**，不一致=被 hook
- `is_hook_module(name)`：遍历已 hook 模块清单 `is_same_module` 比对
- `ProtectFD`：JNI 调 Java 层 protectFD（fd 保护）
- `start_node_detect`：VPN/代理节点检测

**局限**：只查**进程内模块与 hook**——内核驱动 + 独立进程 + 零注入 = 天然规避。

### 3.4 TPRT 运行时自保护（libtprt.so，2.1MB，ARM64）—— 2026-08-19 静态逆向

- **定位**：TPRT = TssSDK 的**运行时（Runtime）**，是 ACE 的"反篡改层"。导出两张表：
  - `g_tprt_pfn_array` @0x2040d0，376B = **47 项函数指针**（TPRT 内部处理函数/替换后入口，全落在 libtprt .text 内）。
  - `g_tprt_ori_array` @0x205e70，120B = **15 项，静态全零**，运行时 inline hook 后填入被替换函数的原始入口（基线）。
- **控制流混淆**：JNI_OnLoad（0x87014，1056B）为**控制流平坦化**——`cmp w8,...; csel x9,...; ldr x9,[x24,x9]; br x9` 跳表分发器（跳表基址 0x1da080），手工完整还原成本高；属反静态分析保护。
- **inline hook 自保护**：字符串实锤 `mprotect`/`mmap`/`munmap`/`dlsym`/`dlopen`——运行时经 dlsym 解析目标、mprotect 改 .text 权限、写入 trampoline 做 inline hook；`g_tprt_ori_array` 保存被替换函数原始入口作基线。
- **完整性自校验**：`memcmp` + **`CrcEr`**（格式 `f:%s s=%d:v=%d`，strings line 3933）+ **`wild scan`**（通配符内存扫描，line 4359，上下文 `insufficient lookahead / no future / more < 2`）——回读自身代码页/关键资源做 CRC 与字节比对，异常记 `CrcEr`（行为/签名扫描，非外部读取检测）。
- **内核通信桥**：`TssSDKIoctl` / `TssSDKIoctlOld`（line 3399-3402）——向 TP2 内核模块 `adt.s3.t` 发 ioctl 的入口；模块当前不加载 → 走 `Old` 回退（与 3.2 结论一致）。
- **线程/进程监控**：`fork` / `pthread_create`（自检/watchdog 线程）；`connect` / `socket`（自身通信或 C2 检测）。
- **关键缺失（威胁判定核心）**：**全串检索 `ptrace`=0、`process_vm`=0、`pread64`/`pwrite64`=0、`read`/`write`/裸 `open`=0**。TPRT **不 hook 任何读取路径系统调用，也无内核组件**。

## 四、采集数据全景（上报服务端）

| 类别 | 内容 | 来源 |
|---|---|---|
| 设备指纹 | IMEI/AndroidID/序列号/机型/ROM/系统版本/分辨率/语言 | libqimei/libtdmqimei |
| 环境标记 | root 特征（k_worker/su 路径）/模拟器/SELinux 状态/挂载 | TuringFD |
| 进程线程 | 全进程遍历 /proc/%d/task/%d/status（TracerPid 反调试） | tersafe |
| 完整性 | libc/linker64 比对、inline_hook opcode 比对、模块列表 | tersafe/tprt |
| 侧信道 | /dev/kgsl-3d0（GPU）、/dev/xr_fbt（帧缓冲）、帧率统计 | 动态 fd 实锤 |
| 平台风控 | QQ/微信登录态、TDataMaster 数据、pandora 上报 | MSDK PIX 系 |
| 应用信息 | APK 签名（cert）、包名/版本、adb 调试状态 | TP2 Java 层 |

TP2 通用采集字段（资料交叉验证）：Language/cpu_model/simulator_name/adb/apk_name/ver/model/api_level/sys_ver/sdcard/ip/dev_cpuname/debugger/cert。

## 五、封号机制

### 5.1 判定链路

```
客户端采集 → 加密上报（senddatatosvr）→ 服务端风控判定 → 封号状态下发 → 客户端弹窗
```

### 5.2 处罚分级（官方规则）

| 来源 | 分级 |
|---|---|
| CF 端游 | 首犯 7 天 → 二犯 30 天 → 三犯 180 天 → 情节严重加重 |
| CFM 手游 | 3/7/15/60 天/一年；安全周用挂 120 天 |
| 本设备实测 | 首次 7 天 |

### 5.3 判定输入（服务端拿什么）

1. **设备指纹**（qimei）——历史行为全关联
2. **环境标记**：TEE/KeyMint 完整性（**最敏感**）、root 特征、模拟器
3. **完整性状态**：libc/linker 被改、模块异常
4. **行为数据**：帧率异常、修改痕迹、举报

**关键判断**：封号 = 设备级环境判定（TEE 伪造被识破 + 设备指纹关联），**不是行为级**。只读 dump 不触发行为检测，但设备环境异常会被服务端拉黑。

## 六、工具规避面分析（gg 套件）

| 检测点 | 规避状态 | 依据 |
|---|---|---|
| is_hook_stable/is_hook_module | ✅ 天然规避 | 只查进程内模块，我们零注入零 hook |
| ptrace 反调试（TracerPid） | ✅ | 驱动直读物理内存，不走 ptrace |
| TuringFD root 正则 | ✅ | k_worker 线程不可见、路径不命中 |
| libc/linker 完整性 | ✅ | 不改系统库 |
| GPU/帧缓冲侧信道 | ✅ | 不改帧率/GPU |
| 内核模块列表（black_module） | ✅ 伪装 | apr_dlkm 厂商风格名 + 288KB 正常大小 |
| /dev 节点扫描 | ✅ 伪装 | /proc/aqmem（隐藏 + fd 伪装） |
| TPRT inline hook 自保护 | ✅ 天然规避 | 只 hook CF 进程内 libc，我们的读取走内核驱动，不经过 CF 用户态 libc |
| TPRT 完整性自校验（CRC / wild scan） | ✅ | 只读不改 CF 内存，基线无变化；我们读游戏数据页、不碰 TPRT 自身页 |
| TPRT 读取路径 hook（ptrace / process_vm_readv） | ✅ 不存在 | 全串检索 0 命中，TPRT 无此能力，无内核组件 |
| ACE 内核检测模块（adt.s3.t） | ✅ 未激活 | 当前版本 lsmod 零命中不加载；无内核态组件观测我们的驱动；即使未来激活，读取已无痕化且 TPRT 不 hook 读取路径 |

**设备端已部署形态**：apr_dlkm（288KB 厂商大小）+ dspbridge（进程伪装）+ /proc/aqmem 节点 + 无痕 xarray 读取 + 零注入。

## 七、关键偏移/符号（复用索引）

```
libturinggame.so:
  root 正则 @0x31bdb     selinux enforce @0x31bc3
  /proc/ 遍历 @0x31e02    debuggerd @0x31e8a
libtersafe.so:
  adt.s3.t 路径 @0x495860（fopen 函数 fcn.0016a814，加载链 fcn.00168dbc）
  /proc/%d/task/%d/status @0x5de750
  libc.so @0x496bbc    linker64 @0x5dc440
  tp2_sdk_ioctl @0xdb094（4184B，ioctl 桥）  tp2_sdk_init @0xdc54c  tp2_sdk_init_ex @0xdc914
  tss_get_report_data @0x2aa3
libgamemaster.so:
  is_hook_stable @0x32d50   is_hook_module @0x31abc
  ProtectFD @0x1e658         start_node_detect @0x403a4
libtprt.so:
  g_tprt_pfn_array @0x2040d0 (376B/47项)  g_tprt_ori_array @0x205e70 (120B/15项,静态零)
  JNI_OnLoad @0x87014 (控制流平坦化)  TssSDKIoctl @0x17f3c0  TssSDKIoctlOld @0x17f400
  CrcEr 字符串 @0x1a7cfa  wild scan 字符串 @0x1b0de7
libil2cpp.so（运行时锚点，均为文件 vaddr = 运行时 so基址 + 偏移，2026-08-19 实测）:
  CodeRegistration       @0xc3b09a0  （静态明文，运行时完整可读）
  MetadataRegistration   @0xba4b510  （静态/运行时均全 0 —— 腾讯清零，真数据走 CodeRegistration 链）
  metadataRegistrations  @0xc37cde8  （指针数组 → 0xc32b028 魔改紧凑结构）
  导出 API（il2cpp_init @0x2662aec 等）静态为垃圾字节、运行时解密为正常指令
```

### 7.1 libil2cpp.so 运行时结构链与字段偏移（2026-08-19 实测）

**腾讯加固策略：函数指针不加密（运行时必需），类型布局数据加密/清零（防逆向）**。静态提取字段偏移不可行，运行时数据按需解密。

**运行时解密行为**：
- 导出 API 区（il2cpp_init@0x2662aec 等）文件里是垃圾字节（`90 53 0f 86`），**运行时是正常指令**（`stp x29, x30, [sp, #-0x10]!` 标准序言）→ 腾讯按需解密，静态反汇编导出 API 无意义
- `.rodata` 类型相关数据静态为密文（`0x50505050`/`0xafaf5448` 重复模式），运行时解密为明文（如 0x9931900 → 0x7141336900）

**CodeRegistration（v24.4 布局，运行时 0x7143db59a0 实测）**—— 全部明文：
```
+0x00  methodPointersCount=368369,  methodPointers=.text 函数地址数组
+0x10  reversePInvokeWrapperCount=2751, +0x18 wrappers
+0x20  genericMethodPointersCount=81946
+0x30  genericAdjustorThunks/invokerPointersCount=18628
+0x40  unresolvedVirtualCallCount=69757
+0x50  interopDataCount=2583
+0x60  windowsRuntimeFactoryCount=1155
+0x70  codeGenModulesCount(魔改=82724, 疑字节数), codeGenModules→0x7143c8e708
+0x80  metadataRegistrationCount(魔改=20956), metadataRegistrations→0x7143d81de8
+0x90  metadataUsagesCount=121655, metadataUsages
```
（开头 16 字节 methodPointers 是 v24.4 特有，Il2CppDumper AutoPlusInit 会 `codeRegistration -= 16`）

**MetadataRegistration 魔改紧凑结构（腾讯替换标准 128B 结构）**：
```
metadataRegistrations → 0x7143d81de8（指针数组，20956 个）
  → 元素 0x7143d30028 = {u64=1, u64=ptr}×8（每字段一个 {count=1, ptr}）
  → ptr 指向数据表 0x71442fed18（8 个数据指针，差 8B）
  → 数据表元素：
      [0]=0x7141336900 genericClasses?   [1]=0x7141336a20 genericInsts?
      [2]=0x7141336a40                   [3]=0x7141336a60 types?
      [4]=0x7141336c50 methodSpecs?      [5]=0x7141336d30 fieldOffsets?
      [6]=0x7141336e00 typeDefSizes?     [7]=0x7141336f60 metadataUsages?
```

**卡点：`{tag, X}` 紧凑编码未破译**。fieldOffsets 数据（0x7141336d30）是 16 字节记录流：
```
{tag=21(0x15), X=0x00070000}, {21, 0x07400000}, {21, 0x07400002}, {21, 0x07800056},
{21, 0x00070003}, {21, 0x00070001}, ...  (tag=21 块, X 高16位多为 0x0007)
{tag=14(0x0E), X=0x000A0000}, {14, 0x000A4000}, ...                    (tag=14 块, X 高16位=0x000A)
```
- tag 在块边界变化（21→14），X = `(高16 << 16) | 低16`
- **已排除**：低 16 位≠字段名索引（查字符串表 0x606/0x1010/0x5180 无对应字段名，0x5180='DeriveMAC' 方法名）；高 16 位≠typedef（7=IConvertible/10=SerializableAttribute 均 0 字段接口/属性类，classes.tsv 与 dump.cs 一致）
- **结论：`{tag,X}` 是腾讯加密的另一套紧凑存储，非运行时的真实字段偏移来源**

### 7.2 破译成功：fieldOffsets 走标准结构（2026-08-19，定制 dump 完成）

**突破口：反汇编 `il2cpp_class_get_fields` 跳板链**（导出 API 运行时已解密）：
```
il2cpp_class_get_fields (导出 0x2662c58) → b 0x2643c64 (真实实现)
  实现: ldr x9, [klass, #0x68]    ← Il2CppClass.fields (FieldInfo 数组)
        ldrh w10, [klass, #0xf8]  ← 字段数
  类初始化 0x2643cd8 → 字段构造循环:
    偏移计算: bl 0x264380c (从 typeDefinition 指针反推 typedef 索引, 104B 记录) → bl 0x26437f4:
      adrp x8, #0x71443ea000; ldr x8, [x8, #0xfd0]  ← 全局 0x71443eafd0 = 0x7143db5a10
      ldr x8, [x8, #0x58]                           ← fieldOffsets 指针数组
      ldr x8, [x8, w0, sxtw #3]                     ← fieldOffsets[typedefIdx]
      ldr w0, [x8, w1, sxtw #2]                     ← [typedefIdx][fieldIdx] = 字段偏移
```

**完整链路（运行时，实测地址）**：
```
0x71443eafd0 (.bss 全局) → 0x7143db5a10 (= CodeRegistration+0x70 处魔改结构)
  → +0x58: fieldOffsets 数组 = 0x714433bc90 (40559 个指针, 按 typedef 索引)
  → [typedefIdx]: 偏移数组指针 (0x7141978070 ~ 0x7141b3f278, 数据区 ~1.9MB)
  → [typedefIdx][fieldIdx]: int32 字段偏移 (实例字段=实际偏移; 静态/const=0 或 -1)
```

**验证（铁律命中）**：
- UnityEngine.Object(1870): m_CachedPtr@0x10 ✓, OffsetOfInstanceID@0(const) ✓
- WNEngine.Decal.ProjectorDecal: size@0x18, volumeHeight@0x1c, volumeStartOffset@0x20 ✓
- WNEngine.CarryWeapon: m_WeaponID@0x18, m_Owner@0x50, Attachments@0x58...（完美 il2cpp 布局）✓
- FieldInfo 结构确认: {name@0, type@8, parent@0x10, offset@0x18, token@0x1c} = 0x28 字节

**定制 dump 工具链**（`gg/tools/`，单一事实源）：
1. `cf_parse_foffsets.py` — 解析 gg_read_bytes 结果文件 → fieldoffsets.json（typedef→偏移数组指针）
2. `cf_merge_offsets_data.py` — 按地址拼接偏移数组数据区段 → cf_offsets_full.bin（**坑：段地址非 0x10000 对齐时须按地址写入，勿顺序 append**）
3. `cf_build_runtime_offsets.py` — metadata 字段名 + 运行时偏移数组合并 → cf_runtime_offsets.tsv（302515 字段，99.9% 覆盖）

**产物**：`ai_workspace/samples/cf_re/cf_runtime_offsets.tsv`（typedef|class|field|field_idx|offset）
**注**：CF 魔改后 UnityEngine.Transform/Component 无实例字段（引擎字段被剥离，m_Parent 等在 WNEngine 游戏类），非解析 bug。

## 八、待验证项（解封后）

- [x] TP2 内核模块在正常账号+对局中是否加载（监控脚本 `/data/local/tmp/tp2_watch3.sh` 已留）——**实测未加载**：1800s 监控零命中 + `lsmod_full_now.txt` 663 模块无 adt/tp2/tss/tprt；当前版本纯用户态
- [x] TPRT 自保护机制（3.4）：确认 inline hook + CRC/wild scan 自校验，**无读取路径 hook、无内核组件** → 驱动天然规避（静态已实锤，待解封后动态确认 `wild scan` 是否扫描游戏内存区域）
- [x] gg 只读通道受控读（2026-08-19）：system_server 读 libcameraopt ELF 头 `7f454c46` 完整返回，apr_dlkm ioctl 直读可用；driver_state:5 误报（agentd 按旧名 rwProcMem_module 查 ko）不影响读取
- [x] CF 运行时 libil2cpp.so 解密行为（2026-08-19）：导出 API 按需解密，CodeRegistration 明文可读
- [x] **CF 字段偏移运行时提取（2026-08-19）：302515 字段偏移到手**（cf_runtime_offsets.tsv，7.2 节）
- [ ] 无痕 xarray 读取对懒加载页的实际效果
- [ ] TEE 套件（TEESimulator/tricky_store）对新版本 ACE 的适配
- [ ] /system/bin/su 的路径迁移（KernelSU manager 改 su 路径）

## 九、关联文件

- so 样本：`ai_workspace/samples/cf_re/so/`（libturinggame/libt ersafe/libtprt/libgamemaster 等）
- 字符串表：`tgpa_strings.txt` / `gm_strings.txt` / `turing_strings.txt`
- 运行时攻坚：libil2cpp.so 211MB + meta_dump_v5.fixed.bin 在 `ai_workspace/samples/cf_re/`；Il2CppDumper v6.7.46 在 `C:\Users\AprYT\Downloads\Il2CppDumper\`；header 手术脚本 `ai_workspace/scripts/cf_build_std_header.py`，运行时结构分析脚本 `cf_disasm_il2cpp_init.py`/`cf_find_fieldoffsets.py`/`cf_scan_fieldoffsets.py`
- 参考资料：cnblogs TP2 分析 / CSDN ACE 逆向对比 / CF 官方处罚规则

---

## 十、证据附录（原始数据，可独立验证）

### A. TuringFD root 检测正则（libturinggame.so @0x31bdb 原始字节）

```
00031bdb  5e 2f 73 79 73 74 65 6d 2f 78 62 69 6e 2f 6b 75   ^/system/xbin/ku
00031beb  5b 2e 5d 73 75 64 24 7c 5e 64 61 65 6d 6f 6e 73   [.]sud$|^daemons
00031bfb  75 3a 7c 5e 6b 5f 77 6f 72 6b 65 72 2f 5b 31 2d   u:|^k_worker/[1-
00031c0b  39 5d 5b 5b 3a 64 69 67 69 74 3a 5d 5d 2a 3a 5b   9][[:digit:]]*:[
00031c1b  5b 3a 64 69 67 69 74 3a 5d 5d 2b 24 7c 5e 6b 72   [:digit:]]+$|^kr
00031c2b  5f 77 6f 72 6b 65 72 2f 5b 31 2d 39 5d 5b 5b 3a   _worker/[1-9][[:
00031c3b  64 69 67 69 74 3a 5d 5d 2a 3a 5b 5b 3a 64 69 67   digit:]]*:[[:dig
00031c4b  69 74 3a 5d 5d 2b 24 7c 5e 6b 6d 5f 77 6f 72 6b   it:]]+$|^km_work
00031c5b  65 72 2f 5b 31 2d 39 5d 5b 5b 3a 64 69 67 69 74   er/[1-9][[:digit
00031c6b  3a 5d 5d 2a 3a 5b 5b 3a 64 69 67 69 74 3a 5d 5d   :]]*:[[:digit:]]
00031c7b  2b 24 7c 5e 74 77 6f 72 6b 65 72 2f 5b 31 2d 39   +$|^tworker/[1-9
00031c8b  5d 5b 5b 3a 64 69 67 69 74 3a 5d 5d 2a 3a 5b 5b   ][[:digit:]]*:[[
00031c9b  3a 64 69 67 69 74 3a 5d 5d 2b 24 7c 5e 74 75 5f   :digit:]]+$|^tu_
00031cab  77 6f 72 6b 65 72 2f 5b 31 2d 39 5d 5b 5b 3a 64   worker/[1-9][[:d
00031cbb  69 67 69 74 3a 5d 5d 2a 3a 5b 5b 3a 64 69 67 69   igit:]]*:[[:digi
00031ccb  74 3a 5d 5d 2b 24 7c 5e 74 71 5f 77 6f 72 6b 65   t:]]+$|^tq_worke
00031cdb  72 2f 5b 31 2d 39 5d 5b 5b 3a 64 69 67 69 74 3a   r/[1-9][[:digit:
00031ceb  5d 5d 2a 3a 5b 5b 3a 64 69 67 69 74 3a 5d 5d 2b   ]]*:[[:digit:]]+
00031cfb  24 7c 5e 6b 77 6f 72 6b 65 72 2f 5b 31 2d 39 5d   $|^kworker/[1-9]
00031d0b  5b 5b 3a 64 69 67 69 74 3a 5d 5d 7b 32 7d 24 7c   [[:digit:]]{2}$|
00031d1b  5e 70 65 72 6d 6d 67 72 64 24 7c 5e 33 36 30 73   ^permmgrd$|^360s
00031d2b  67 75 61 72 64 24 7c 5e 2f 64 61 74 61 2f 64 61   guard$|^/data/da
00031d3b  74 61 2f 5b 5b 3a 61 6c 6e 75 6d 3a 5d 5f 2e 2d   ta/[[:alnum:]_.-
00031d4b  5d 2b 2f 7c 5e 2f 73 79 73 74 65 6d 2f 62 69 6e   ]+/|^/system/bin
```

### B. 关键检测字符串（文件偏移 = vaddr，实测 dump）

| so | 偏移 | 字符串 | 含义 |
|---|---|---|---|
| libturinggame.so | 0x31bc3 | `/sys/fs/selinux/enforce` | SELinux 状态检测 |
| libturinggame.so | 0x31e8a | `/system/bin/debuggerd` | 反调试路径检测 |
| libtersafe.so | 0x495860 | `/data/data/%s/files/adt.s3.t` | **TP2 内核模块释放路径** |
| libtersafe.so | 0x5de750 | `/proc/%d/task/%d/status` | **全进程线程遍历（TracerPid）** |
| libtersafe.so | 0x496bbc | `/system/lib64/libc.so` | 关键库完整性 |
| libtersafe.so | 0x5dc440 | `/system/bin/linker64` | linker 完整性 |
| libtersafe.so | 0x29f0 | `tp2_sdk_ioctl` | TP2 ioctl 接口 |
| libtersafe.so | 0x2aa3 | `tss_get_report_data` | 检测数据上报 |

### C. libgamemaster.so 检测函数符号（llvm-nm -D 实测）

```
0000000000031abc T _Z14is_hook_modulePKc     is_hook_module(char*)
0000000000032d50 T _Z14is_hook_stablev       is_hook_stable()
000000000001e658 T _Z9ProtectFDi             ProtectFD(int)
00000000000403a4 T start_node_detect         节点检测
0000000000040424 T is_node_detected
```

### D. CF 进程实际加载的采集 so（/proc/PID/maps 实测）

```
libqimei.so  libtdmqimei.so  libtersafe.so  libtprt.so
libgamemaster.so  libTDataMaster.so  libpandora.so  libcgsdk.so
libMSDKPIXCore.so  libMSDKPIXBeacon.so  libMSDKPIXQQ.so  libMSDKPIXSystem.so
libMSDKPIXTDM.so  libMSDKPIXWebView.so  libMSDKPIXWechat.so  libMSDKPIXXG.so
libCrashSight.so  libMSDKUnityAdapter.so
```

### E. 侧信道采集（/proc/PID/fd 实测）

```
134 -> /dev/kgsl-3d0    GPU 状态（GPU Profiler 检测）
119 -> /dev/xr_fbt      帧缓冲
```

### F. TP2 内核模块未加载（lsmod 全量对比）

大号时期 vs 小号时期：**663 个模块集合 100% 一致，新增=0，消失=0**；dmesg init_module/finit_module 事件 = 0。

### G. 官方处罚规则（引用）

- CFM 处罚规则：https://cfm.qq.com/webplat/info/news_version3/17544/21851/21852/21854/m19873/201707/613233.shtml
- CF 端游封号规则：https://cf.qq.com/act/a20090409forceout/f_rule.htm

---

## 十一、三角洲行动手游 ACE（已拆分独立文档）

三角洲手游的 ACE 集成深度、威胁判定、坐标加密机制、无痕读取+解密方案，已拆分至独立文档 **`gg/docs/三角洲行动手游ACE分析.md`**（同源同构腾讯 ACE、无内核模块、坐标 shellcode 加密、rwproc33 无痕解密等全部内容）。本节不重复，避免与 CF 本文档混放。
