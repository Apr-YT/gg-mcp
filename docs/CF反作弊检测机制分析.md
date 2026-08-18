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
- **内核模块**：`adt.s3.t`（释放到 `/data/data/<pkg>/files/`，加载后自删）
  - 加载链：fcn.00168dbc（混淆控制流）→ fcn.0016a814（fopen 路径）
  - 协议：ioctl + magic（`MagicCodeWrong`）+ 签名（`TSS_SDK_SIGDATA`/`tss_recv_sec_signature`）+ `time_magic`
  - **服务端签名下发才加载**（`TssSDKOnRecvSignature`）
- **实测结论**（2026-08-19）：大号（封号）/小号/全新进程/设备重启后，**adt.s3.t 均不加载**；lsmod 663 模块集合大号 vs 小号 100% 一致。当前 CF 版本在 root/KernelSU 设备上**纯用户态检测**。

### 3.3 libgamemaster.so hook 引擎（QPP）

- `is_hook_stable`：`getpid()` → `enum_process_module()` 枚举自身进程模块 → 与基线（hook_init 时缓存 g_lm）对比**数量 + 每模块 +0x200 指针**，不一致=被 hook
- `is_hook_module(name)`：遍历已 hook 模块清单 `is_same_module` 比对
- `ProtectFD`：JNI 调 Java 层 protectFD（fd 保护）
- `start_node_detect`：VPN/代理节点检测

**局限**：只查**进程内模块与 hook**——内核驱动 + 独立进程 + 零注入 = 天然规避。

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
  tp2_sdk_ioctl @0x29f0   tss_get_report_data @0x2aa3
libgamemaster.so:
  is_hook_stable @0x32d50   is_hook_module @0x31abc
  ProtectFD @0x1e658         start_node_detect @0x403a4
libtprt.so:
  g_tprt_ori_array @0x155b  TssSDKIoctl @0x17f3c0
```

## 八、待验证项（解封后）

- [ ] TP2 内核模块在正常账号+对局中是否加载（监控脚本 `/data/local/tmp/tp2_watch3.sh` 已留）
- [ ] 无痕 xarray 读取对懒加载页的实际效果
- [ ] TEE 套件（TEESimulator/tricky_store）对新版本 ACE 的适配
- [ ] /system/bin/su 的路径迁移（KernelSU manager 改 su 路径）

## 九、关联文件

- so 样本：`ai_workspace/temp/gg_hillclimb/cf_re/so/`（libturinggame/libt ersafe/libtprt/libgamemaster 等）
- 字符串表：`tgpa_strings.txt` / `gm_strings.txt` / `turing_strings.txt`
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
