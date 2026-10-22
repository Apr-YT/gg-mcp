# GG-Ghost — 内核虚拟触摸注入套件（完整项目文档）

> 版本：v1.0.0 ｜ 日期：2026-08-18 ｜ 目标设备：REDMI myron（KernelSU），内核 6.12.69-android16-6-4k

---

## 1. 项目概述

GG-Ghost 是 **gg-mcp 生态的内核级隐形触摸注入组件**。它在 rwProcMem33 内核驱动内注册一个虚拟 input 设备（`gg-ghost`），通过驱动 proc 节点协议注入 tap / swipe 触摸事件，事件流与真实触屏完全一致：

```
input core → evdev → InputReader → InputDispatcher → 应用
```

**核心价值**：
- **隐形**：复用内核 input 子系统标准管线，无新设备暴露在系统输入之外（InputManager 眼中与真实触摸无差异）
- **无权限依赖**：绕过 `INJECT_EVENTS` 权限（`adb shell input tap` 被锁的场景全解）
- **一体通道**：与 gg-mcp 内存读写共用同一条 rwproc33 驱动通道，AI 可同时读写内存 + 注入触摸

---

## 2. 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│ PC 侧                                                       │
│  AI agent ──MCP──▶ gg-ghost-mcp (HTTP :8788)                │
│                     gg_ghost_tap / gg_ghost_swipe           │
│                          │  wire 协议 (20B 帧头 + JSON)     │
└──────────────────────────┼──────────────────────────────────┘
                           │ adb forward tcp:19100
┌──────────────────────────┼──────────────────────────────────┐
│ 设备侧                                                   ▼  │
│  gg-ghost-agentd (root, :19100) ──read(33B 头协议)──▶ rwProcMem_module.ko
│                                                   │ cmd=15 tap / 16 swipe
│                                                   ▼
│                                            gg-ghost 虚拟 input 设备
│                                            (input_mt_* + input_event)
│                                                   │
│                                                   ▼
│                                            InputReader → 应用
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 代码位置与改动清单

### 3.1 驱动层（rwproc33，6.12.69 KernelSU 适配）

| 文件 | 改动 |
|---|---|
| `rwproc33/rwProcMem33Module/rwProcMem_module/rwProcMem_module.c` | +`CMD_GGTOUCH_TAP=15/CMD_GGTOUCH_SWIPE=16` 命令处理、`gg_touch_register()` 虚拟设备注册、`gg_touch_tap_raw/swipe_raw` 注入核心、`gg_disp2raw` 坐标换算、init/exit 注册注销 |
| `.../rwProcMem_module.h` | +`gg_touch_tap{x,y,hold_ms}` / `gg_touch_swipe{x1,y1,x2,y2,ms}` 结构 |
| `.../api_proxy.h` | `__kmalloc()` → `kmalloc()`（6.12 内核宏签名变更） |
| `.../hide_procfs_dir.h` | `strlcpy()` → `strscpy()`（6.10+ 内核移除 strlcpy） |
| `.../proc_maps.h` | 6.6.30 实现分支条件扩展 `|| MY_LINUX_VERSION_CODE >= KERNEL_VERSION(6,12,0)` |
| `rwproc33/hwBreakpointProcModule/hwBreakpointProc_module/hwBreakpointProc_module.c` | +`OnCmdInstAllProcessHwbp`（CMD 13 全线程批量注册，`for_each_thread` 两阶段：RCU 内收集 task 引用、RCU 外 vmalloc/register）；`clean_hwbp` 数组改动态分配（128 槽超内核栈 2KB） |
| `.../hwBreakpointProc_module.h` | +`CMD_INST_PROCESS_HWBP_ALL=13`；`HWBP_MAX_HANDLES` 16→**128**（全线程注册）；`HWBP_HIT_CAP` 1024→512（内存平衡） |

**⚠️ 两阶段注册坑**（2026-08-18 真机重启根因）：`for_each_thread` 需 RCU 读锁，但 `vmalloc`/`register_user_hw_breakpoint` 会睡眠 → RCU 锁内调用 = `scheduling while atomic` → PANIC_ON_OOPS 整机重启。必须 **RCU 内只 `get_task_struct` 收集引用，RCU 外逐个注册，完事 `put_task_struct`**。

**虚拟设备注册**（`gg_touch_register()`）：
```c
gg_touch_dev = input_allocate_device();
gg_touch_dev->name = "gg-ghost";
gg_touch_dev->id.bustype = BUS_VIRTUAL;
set_bit(EV_KEY, evbit);  set_bit(BTN_TOUCH, keybit);
set_bit(EV_ABS, evbit);
input_set_abs_params(dev, ABS_MT_POSITION_X, 0, 119999, 0, 0);
input_set_abs_params(dev, ABS_MT_POSITION_Y, 0, 260799, 0, 0);
input_mt_init_slots(dev, 10, 0);   /* 与真实触屏一致 10 触点 */
input_register_device(dev);
```

**注入核心**（与真实触屏协议逐字节一致）：
```c
input_mt_slot(dev, 0);
input_mt_report_slot_state(dev, MT_TOOL_FINGER, true);
input_report_key(dev, BTN_TOUCH, 1);
input_report_abs(dev, ABS_MT_POSITION_X, x);
input_report_abs(dev, ABS_MT_POSITION_Y, y);
input_sync(dev);
/* hold_ms 延时 */
input_mt_report_slot_state(dev, MT_TOOL_FINGER, false);
input_report_key(dev, BTN_TOUCH, 0);
input_sync(dev);
```

**坐标换算**（驱动内，display → raw）：
```
raw_x = 120000 - dy * 100
raw_y = dx * 100
```
（横屏 CCW 旋转：display 2608x1200 → raw 120000x260800，即 100 倍缩放）

### 3.2 设备端（gg-ghost-agentd）

| 文件 | 改动 |
|---|---|
| `agentd/driver.h` | +`RWPROC_CMD_GGTOUCH_TAP=15/SWIPE=16`、`rwproc_gg_touch_tap/swipe_t` 结构、`driver_touch_tap/swipe` 声明；+`RWBP_CMD_INST_PROCESS_HWBP_ALL=13`、`rwproc_hwbp_start_all` 声明 |
| `agentd/driver.c` | +`driver_touch_tap/swipe` 实现（`rwproc_call()` 通用调用，data 传结构）；+`rwproc_hwbp_start_all`（一次 IOCTL 全线程注册，返回句柄数组） |
| `agentd/main.c` | +`h_ghost_tap/h_ghost_swipe` handler（JSON→结构）、cmd_name 映射、dispatch 2 case |
| `agentd/watch.c` | 路径 2 改为一次 `rwproc_hwbp_start_all`（删 list_threads 逐线程循环）；显示修复 `ntids=nb` 写回 |
| `shared/wire_protocol.h` | +`GGCMD_GHOST_TAP / GGCMD_GHOST_SWIPE` |

### 3.3 PC 端（GG-GHOST-MCP，gg-ghost-mcp.exe）

| 文件 | 改动 |
|---|---|
| `server/mcp/schema.cpp` | +`gg_ghost_tap` / `gg_ghost_swipe` 工具注册（含参数 schema 与描述）；全量 44 工具 |
| `server/session.cpp` | +`tool_ghost_tap/swipe` 函数、`kTools[]` 表 2 条目（`write_op=true`） |
| `server/main.cpp` | 命名统一 GG-GHOST-MCP（serverInfo.name=gg-ghost-mcp） |
| `configs/gg-ghost.json` | 配置（原名 ggmcp.json，`safe_mode` 字段控制只读） |

---

## 4. 协议定义

### 4.1 wire 协议（agentd ↔ server，GGCMD）

```c
GGCMD_GHOST_TAP   = 82,   /* {"p":{"x":..,"y":..,"hold_ms":..}} */
GGCMD_GHOST_SWIPE = 83,   /* {"p":{"x1":..,"y1":..,"x2":..,"y2":..,"ms":..}} */
```

### 4.2 驱动协议（agentd ↔ rwProcMem_module，33B 头）

```c
struct rwproc_request { char cmd; u64 param1; u64 param2; u64 param3; u64 buf_size; };  /* 33B packed */

RWPROC_CMD_GGTOUCH_TAP   = 15,   /* buf = rwproc_gg_touch_tap {u32 x, y, hold_ms}        (12B) */
RWPROC_CMD_GGTOUCH_SWIPE = 16,   /* buf = rwproc_gg_touch_swipe {u32 x1,y1,x2,y2,ms}     (20B) */
```

坐标均为 **display 逻辑坐标**（横屏 2608x1200），驱动内换算 raw。

---

## 5. 构建

### 5.1 驱动（KO，需 Linux 环境）

```bash
# Linux Mint VM: king@192.168.42.128, 内核树 /opt/gki (android16-6.12, out/ 已构建)
cd /opt/rwProcMem33/rwProcMem33Module/rwProcMem_module
make -C /opt/gki/out M=$PWD ARCH=arm64 LLVM=1 CC=clang KBUILD_MODPOST_WARN=1 modules
# 产物: rwProcMem_module.ko (~315KB)
```

**CRC 修补**（CONFIG_MODVERSIONS=y，必须）：
```bash
python patch_ko_versions.py <设备厂商ko目录> rwProcMem_module.ko
# 设备厂商 ko: adb pull /vendor/lib/modules/
# 实测: patched=23 same=24 missing=1 (copy_from_kernel_nofault, 内核导出符号, 不影响加载)
```

**硬件断点模块**（hwBreakpointProc_module.ko，全线程断点）：
```bash
cd /opt/rwProcMem33/hwBreakpointProcModule/hwBreakpointProc_module
make -C /opt/gki/out M=$PWD ARCH=arm64 LLVM=1 CC=clang KBUILD_MODPOST_WARN=1 modules
# 产物: hwBreakpointProc_module.ko (~323KB) → 同样跑 patch_ko_versions.py (patched=11 same=21 missing=0)
```

**6.12 适配已知坑**：
1. `__kmalloc` 宏签名变 → 用 `kmalloc`
2. `strlcpy` 已移除（6.10+）→ 用 `strscpy`
3. `proc_maps.h` 无 6.12 实现分支 → 复用 6.6.30 分支（条件加 `>= 6.12.0`）
4. `mm->rss_stat` 类型变（6.6+ 为 `struct percpu_counter[]`）→ `linux_kernel_api.h` 已有 `>= 6.6.0` 分支自动处理
5. `HWBP_MAX_HANDLES` 扩 128 后 `clean_hwbp` 静态数组超内核栈 2KB（-Wframe-larger-than）→ 改 vmalloc 动态分配
6. 断点模块 `for_each_thread` 内严禁 vmalloc/register（RCU 锁内睡眠 → 整机重启）→ 两阶段注册

### 5.2 设备端（gg-ghost-agentd，NDK）

```bat
aarch64-linux-android28-clang -O2 -Wall -I%ROOT% -I%CAPSTONE%\include -o gg-ghost-agentd ^
  main.c driver.c process.c search.c freeze.c version.c loader.c watch.c disasm.c elfsym.c ^
  %ROOT%\third_party\cJSON.c %CAPSTONE%\libcapstone.a -pie
```

### 5.3 PC 端（GG-GHOST-MCP，MinGW）

```bat
g++ -std=c++17 -O2 -Wall -Wextra ^
  server\main.cpp server\mcp\protocol.cpp server\mcp\schema.cpp server\session.cpp ^
  server\annotations.cpp server\device\wire.cpp ^
  -o gg-ghost-mcp.exe -lws2_32
```

---

## 6. 部署（设备 /data/adb/GG-Ghost/）

```
/data/adb/GG-Ghost/
├── GG-Ghost.sh            # 包装启动脚本（start/stop/status）
├── gg-ghost-agentd        # 设备端守护进程
├── ko/
│   ├── rwProcMem_module.ko      # 内存读写 + gg-ghost 触摸驱动
│   └── hwBreakpointProc_module.ko  # 硬件断点驱动（全线程 INST_ALL）
└── README.md              # 部署说明（简版）
```

### 启动脚本特性

- **so 防落盘**：so 以 gzip 压缩存放，`start` 时自解压到 `/data/local/tmp/gg-ghost-runtime/`
- **3 秒自动删除**：启动完成后 `sleep 3 && rm -f` 解压产物（防检测/取证，不留长期文件）
- **双驱动自动加载**：`load_ko` 加载 rwProcMem_module，`load_ko_bp` 加载断点模块（存在则加载，失败不影响内存/触摸）
- **管理命令**：`start` / `stop` / `status`

### 使用

```sh
su -c 'sh /data/adb/GG-Ghost/GG-Ghost.sh start'     # 启动
su -c 'sh /data/adb/GG-Ghost/GG-Ghost.sh status'    # 状态
su -c 'sh /data/adb/GG-Ghost/GG-Ghost.sh stop'      # 停止
```

启动后需 `adb forward tcp:19100 tcp:19100` 打通 PC ↔ 设备。

---

## 7. MCP 工具用法（AI 消费）

### gg_ghost_tap

```json
{"name": "gg_ghost_tap", "arguments": {"x": 2035, "y": 918, "hold_ms": 150}}
```

| 参数 | 类型 | 说明 |
|---|---|---|
| x | int | display X（横屏 0-2607） |
| y | int | display Y（横屏 0-1199） |
| hold_ms | int | 按下保持毫秒（默认 120） |

### gg_ghost_swipe

```json
{"name": "gg_ghost_swipe", "arguments": {"x1": 100, "y1": 600, "x2": 2400, "y2": 600, "ms": 400}}
```

| 参数 | 类型 | 说明 |
|---|---|---|
| x1,y1 | int | 起点 display 坐标 |
| x2,y2 | int | 终点 display 坐标 |
| ms | int | 滑动时长毫秒（默认 300，驱动内 20 步线性插值） |

**坐标获取**：`adb exec-out screencap -p` 截图 + 像素分析（PIL 扫目标按钮颜色簇质心）。

### 硬件断点（gg_watch_set / poll / clear，全线程）

```json
{"name": "gg_watch_set", "arguments": {"addr": "0x707c510ed4", "len": 4, "type": 2}}
{"name": "gg_watch_poll", "arguments": {}}
{"name": "gg_watch_clear", "arguments": {}}
```

| 参数 | 说明 |
|---|---|
| addr | 监视地址（支持 `libgame+0x702ED4` 偏移语法，脚本端解析） |
| type | 1=read / 2=write / 3=读写（默认 2） |
| len | 监视长度字节（默认 4） |

**全线程特性**：驱动 `CMD_INST_PROCESS_HWBP_ALL` 一次注册进程**全部线程**的 per-task 断点（任务切换自动换装 debug 寄存器，不受每 CPU 4 槽限制）。`watch_poll` 返回命中 PC + 反汇编上下文，可 `gg_disassemble` 反查写入代码。

**用法流程**：`watch_set` → 用户操作触发写入 → `watch_poll` 拿命中 PC → `gg_disassemble` 看写入逻辑。

---

## 8. 反检测特性

| 特性 | 说明 |
|---|---|
| **标准输入管线** | 事件经 input core 分发，非注入 API，无 INJECT_EVENTS 痕迹 |
| **无新设备暴露**（用户态通道） | `sendevent` 直写真实触屏 `/dev/input/event6`（focaltech_ts）——复用真实设备，InputManager 零感知 |
| **虚拟设备伪装**（内核通道） | 虚拟设备伪装成真实触屏 `focaltech_ts`（同 name/bus/vendor/product，`dumpsys input` 显示两个 focaltech_ts，枚举不可区分） |
| **触摸行为随机化** | tap 坐标 ±3px + hold ±10ms、swipe 滑点 ±2px 抖动（get_random_u32）——无机械规律，难行为检测 |
| **so 防落盘** | gzip 存储 + 运行时自解压 + 3 秒自动删除 |
| **驱动隐藏** | rwProcMem33 支持 `CMD_HIDE_KERNEL_MODULE`（隐藏 /proc/modules 条目与 kobject） |

**检测面提示**（防御方可能手段）：
- `dumpsys input` / `getevent -pl` 枚举设备 → 可见 `gg-ghost`
- 触摸事件时间戳/坐标模式分析（机器人点击 pattern）→ 可做行为检测
- 若需完全隐形：优先用户态通道（sendevent 写 event6），或后续让 gg-ghost 伪装成已知设备参数

---

## 9. 测试验证记录（2026-08-18）

| 环节 | 结果 |
|---|---|
| 驱动编译（VM） | ✅ rwProcMem_module.ko 315400B |
| CRC 修补 | ✅ patched=23 same=24 missing=1 |
| insmod 加载 | ✅ 无报错，`/proc/modules` Live |
| 虚拟设备注册 | ✅ dmesg `gg-ghost: virtual input device registered`；Device 10 / input9 |
| 设备能力 | ✅ BTN_TOUCH + ABS_MT_SLOT(0-9) + POSITION_X/Y + TRACKING_ID |
| 内核注入（gg-ghost-touch kerneltap） | ✅ rc=0，游戏弹窗响应（悬架升级） |
| 用户态注入（sendevent/gg-ghost-touch tap） | ✅ UI 切换（主菜单→选车） |
| GG-Ghost.sh 启动 | ✅ agentd RUNNING / ko LOADED / 3s 自动清理 so |
| MCP 端到端 | ✅ `gg_ghost_tap(2035,918,150)` → `ok:1` → 游戏 UI 响应 |

### 9.1 全线程硬件断点端到端（2026-08-18 20:05）

| 环节 | 结果 |
|---|---|
| 全线程注册 | ✅ `gg_watch_set` → bps=65~69（游戏全线程，旧版仅 4-16） |
| 断点命中 | ✅ 用户买越野摩托（-75000）→ `watch_poll` hits=1 |
| 命中 PC | `0x707e5ca4ec` = `str w9, [x8]`（购买扣款写回） |
| 反汇编 | `ldr w9,[x8] / sub w9,w9,w0 / str w9,[x8]` = 读-减-写回（x8=金币地址, w0=花费） |
| 闭环验证 | 金币 1000059 → 925059（-75000 ✓ 与购买吻合） |
| 设备稳定 | ✅ 无死机（两阶段注册修复后） |

**对比旧版**：16 槽/4 线程注册 = 0 命中（抓不到工作线程写入）；128 槽/全线程 INST_ALL = 1 命中（抓到扣款代码）。

### 9.2 金币定位与静态偏移（2026-08-18 20:10）

- **逻辑金币静态偏移 = `libgame.so + 0x702ED4`**（.bss 全局变量）
- 三次跨 ASLR 验证（进程重启后基址变化，偏移恒命中）：`模块基址 + 0x702ED4 = 金币值`
- 镜像副本在 heap（无静态偏移）；UI 显示独立缓存（事件刷新）
- 存档对象结构：金币附近 = 车辆条目数组（每 0x28 一条 {ID,flag,指针,112}）+ 状态标志 + 保存时间戳 + 槽名 "slot_0" + 魔数 0x12345678

---

## 10. 工具链备忘

| 组件 | 位置 |
|---|---|
| 驱动源码 | `D:\project\workbuddy\工具\gg-rwproc33-driver\rwproc33\`（git: Apr-YT/gg-rwproc33-driver） |
| gg-mcp 源码 | `D:\project\workbuddy\工具\gg\`（git: Apr-YT/gg-mcp） |
| 设备端工具 | `D:\project\ai_workspace\scripts\gg-ghost-touch.c`（用户态 + 内核双模式） |
| 编译环境 | Linux Mint VM（king@192.168.42.128，key: `~/.ssh/vm_mint_ed25519`，内核树 /opt/gki） |
| 构建产物 | `gg-mcp\build\agentd\gg-agentd`、`gg-mcp\build\pc\gg-ghost-mcp.exe`、`gg-mcp\build\ko\*.ko` |
| 部署目录 | 设备 `/data/adb/GG-Ghost/`；**成品包 `D:\Finished product\GG-Ghost\`** |
| Python 脚本 | `gg\tools\`（ggghost.py / hillclimb_coin.py / ptrscan.py） |

### 相关命令速查

```bash
# 用户态触摸（无内核虚拟设备暴露）
adb shell /data/local/tmp/gg-ghost-touch tap 2035 918 150
# 内核触摸（走 gg-ghost 虚拟设备）
adb shell /data/local/tmp/gg-ghost-touch -p /proc/<MEM_KEY>/<MEM_KEY> kerneltap 2035 918 150

# gg-ghost-mcp 启动
cd D:\Kiln\gg-mcp && build\pc\gg-ghost-mcp.exe --config configs\gg-ghost.json --transport http --port 8788

# Python 脚本（GG Lua 等价物）
python gg\tools\ggghost.py read libgame+0x702ED4
python gg\tools\hillclimb_coin.py freeze 999999
python gg\tools\ptrscan.py libgame+0x702ED4
```

---

## 12. Python 脚本客户端（GG 修改器 Lua 等价物）

`gg/tools/` 下 3 个脚本，封装 MCP 调用 + 静态偏移解析 + 状态保存，**AI 定位数据首选用脚本而非裸 curl**。

| 文件 | 用途 |
|---|---|
| `ggghost.py` | 通用 MCP 客户端（44 工具封装，`libgame+0x` 偏移语法，CLI 模式） |
| `hillclimb_coin.py` | 登山赛车金币脚本：`check` / `freeze` / `unfreeze` / `locate`（读值→搜→存 state）/ `locate2`（交叉→写测→冻结测） |
| `ptrscan.py` | 多级指针扫描（`gg_set_results`→`gg_pointer_scan`→`gg_pointer_chain`→迭代到模块基址） |

**用法**：
```bash
python tools/ggghost.py search <v> [type] [region]
python tools/ggghost.py freeze libgame+0x702ED4 999999
python tools/ggghost.py watch libgame+0x702ED4 2 4 && python tools/ggghost.py poll
python tools/hillclimb_coin.py locate   # 第一步搜当前值存 state
python tools/hillclimb_coin.py locate2 <新值>  # 第二步交叉收敛
python tools/ptrscan.py <目标地址> [层数]  # 扫指针链
```

**适用判断**：全局变量（如金币 .bss）→ 静态偏移直接定位，**无指针链**（代码 adrp+add 相对寻址）；堆上动态对象（玩家/NPC/背包）→ 用 `ptrscan.py` 追指针链到基址。

---

## 13. 成品交付（D:\Finished product\GG-Ghost\）

```
D:\Finished product\GG-Ghost\
├── GG-GHOST-MCP\            # PC 端（gg-ghost-mcp.exe + config\gg-ghost.json）
├── GG-Ghost-Device\         # 设备端部署包（GG-Ghost.sh + agentd + so.gz + ko×2 + README）
├── docs\GG-GHOST.md         # 本文档
└── tools\                   # Python 脚本
```

**成品取用即部署**：设备端 push `/data/adb/GG-Ghost/` → `GG-Ghost.sh start`；PC 端 `gg-ghost-mcp.exe --config config\gg-ghost.json --port 8788`。

---

## 14. 后续规划

- [x] **金币定位闭环**：✅ 2026-08-18 完成（逻辑金币 `libgame+0x702ED4`，冻结=无限购买）
- [x] **全线程硬件断点**：✅ 2026-08-18 完成（INST_ALL 69 线程注册，命中扣款代码）
- [ ] **手势宏**：驱动侧加 swipe 多段曲线、双击、长按序列
- [ ] **GG-Ghost 伪装**：虚拟设备参数对齐真实触屏（vendor/product/name），降低设备枚举检测面
- [ ] **配置收敛**：GG-Ghost.sh 参数（PORT/KEY/KO 路径）抽到独立 config 文件
- [ ] **Magisk 自启动**：接入 service.d 自动拉起
- [ ] **钻石定位**：同法（钻石变化→交叉→偏移）

---

## 15. 反检测伪装措施清单（对抗 ACE，已部署 2026-08-19 设备验证）

> **命名说明**：本文档 §3/§6 描述的是**源码原名**（`rwProcMem_module.ko` / `gg-ghost-agentd` / `gg-*`）；本节描述的是**设备侧已部署的伪装变体名**（`apr_dlkm` / `dspbridge` / `aq_*`），二者为同一套件在不同命名阶段的形态。本套件在腾讯 ACE（CF / 三角洲同源）环境存活所实施的全部伪装。**剩余暴露面见 15.3，治本需 susfs / 刷内核。**

### 15.1 内核驱动伪装（apr_dlkm，rwproc33 协议）
- **模块名伪装**：`rwProcMem_module` → **`apr_dlkm`**（高通音频 DSP 风格名），ACE 扫 `/proc/modules` 只见"厂商音频固件"。
- **大小伪装**：`.bss` 加 256KB padding（`g_module_size_pad[262144]`）→ lsmod Size 32768→**294912（288KB）**，落入真实厂商驱动大小区间，规避 `module_size_in_range` 过小判定；`.bss` 不占 `.ko` 文件空间、不影响功能。
- **内核符号去特征**：`gg_*` → `aq_*`（含 printk `"gg-ghost"`→`"aq-dsp"`）；目录/文件 `rwProcMem*` → `adsp_hal_dlkm/aq_*`；Makefile `-ffile-prefix-map` 去编译路径（`__FILE__` 泄露）。验证：`/proc/kallsyms` 的 `rwProcMem/gg_` = 0。
- **节点改名**：`/proc` 节点由 `ver_control.h` 硬编码 `CONFIG_PROC_NODE_AUTH_KEY`（32 位 hex 特征）→ 改名 **`aqmem`**（厂商风格，fd 显示 `/proc/aqmem/aqmem`）。
- **固件化加载**：驱动存 **`apr_dlkm.fw`**（`insmod` 不看扩展名，实测成功）→ 设备上任何时刻无 `.ko` 文件，ACE 扫目录只见"厂商固件"。
- **无痕读取**：手写 xarray 遍历 + `page_address` 直读 shmem page cache——**零缺页（不 fault-in）/零接触/零新增导出符号**；读路径 PTE 直读优先 → 页缓存回退 → 判空洞。

### 15.2 agentd 伪装（dspbridge）
- 二进制改名 **`dspbridge`**（避开系统已有 `qseecomd`），部署 `/data/adb/vendor_security/`（vsec.sh 启停）。
- **cmdline 隐藏**：`main` 解析参数后调用 `hide_process` 清空 `argv[1..]` + `environ`（保留 `argv[0]`=程序名），`/proc/PID/cmdline` 仅程序名、comm 与二进制名一致，消除 `--driver rwproc/--rwproc-key aqmem/--rwproc-bp-key ...` 泄露（`libckguard` `/proc/%u/cmdline` 检测入口）。
- **二进制 strings 清零**：`/data/adb/GG-Ghost` 路径、`gg-touch`、`GG_KO_*` 环境变量名等特征批量替换，新二进制 strings 归零。
- **tmp 不留文件**：push 中转后立即删除；ko 直接 `insmod /data/adb/vendor_security/apr_dlkm.fw`，无自解压/无 tmp 写。

### 15.3 KernelSU 痕迹处理与剩余暴露面
- **已修**：`ksud` → **`adspd`** 改名（su 功能不受影响）；重启清 dmesg `last unloaded: rwProcMem_module`；删 `/data/adb/GG-Ghost` 旧目录 + `/data/local/tmp` 残留（`gg-agentd`/`tp2_watch*`/`sox` 等 ACE 重点目录）。
- **剩余暴露面（治本需 susfs / 刷内核，当前未做）**：`u:r:ksu:s0` 域（KernelSU su 不支持 `--context`）、`/system/bin/su`、`/data/adb/ksu/`、`kernelsu` 内核模块名、`/system/bin/su` 路径检测可能命中 TuringFD。
- **封号归因提示**：历史 7 天封号主因更可能是 **TEE 伪造套件被识破**（TEESimulator/tricky_store，腾讯对 TEE/KeyMint 零容忍）+ KernelSU root 痕迹，工具 fault-in/伪装贡献次要。

### 15.4 铁律
- **CF / 腾讯系游戏绝不 `gg_attach`**（注入 = ACE 秒封）；只读驱动直读才安全。
- 进程/设备重启后 ASLR 变化 → 读内存前必须实时查 `/proc/PID/maps`（旧基址读新进程误判驱动坏，实际驱动正常）。

---

## 16. 三角洲行动手游：坐标无痕读取 + Unicorn 解密（设计 2026-08-19）

> 本套件在三角洲（`com.tencent.tmgp.dfm`，1.201.x）的**数据获取方案**。游戏分析面（ACE 集成深度/坐标加密机制）见独立文档 `三角洲行动手游ACE分析.md`（属游戏 ACE 分析，不在此文档重复）。

### 16.1 难度增量（相比 CF）

CF 关键数据是**明文全局变量**（libgame.so + 偏移，读完即用）。三角洲的**玩家坐标/物资被 ACE shellcode 实时加密**——用驱动直读拿到的是密文，须先解密。这是三角洲比 CF 多出来的一截难度。

### 16.2 ACE 加密机制（逆向结论）

- **Hook**：`libUE4.so` 的 `USceneComponent::SetRelativeLocation` → `BR X16` 跳进**运行时 mmap 注入的 940KB RWX 匿名区 shellcode**（43 个 OLLVM CFF 函数，不在 so 文件）。
- **数据布局**（相对 component / actor）：
  - `+0x168` 明文 `FEncVector`（X/Y/Z + 标志，解密读取点）
  - `+0x210` 加密坐标（ACE 写入）
  - `+0x174` `FEncHandler`（加密标志）
  - `AActor +0x180` = `RootComponent`
- **解密**：Unicorn 模拟 `shellcode+0x9E000`（栈 0x290），`X0`=component、`X1`=加密区、`LR`=RET_STUB；另含 `+0x50000` 查找、`+0x510A0` MurmurHash3。
- `files/ace_shell_di.dat`（22B）仅 shellcode 版本/配置标记，不含密钥。

### 16.3 我们的无痕方案（本套件杀手锏）

公开 `dfm_android_unicorn` 用 `process_vm_readv` + `ptrace`（用户态，TPRT 监控面）。本套件用内核驱动绕开：

| 步骤 | 公开方案（会被检） | 本套件（apr_dlkm 无痕） |
|---|---|---|
| 抓 RWX shellcode / 读内存 | `process_vm_readv` | rwproc33 零缺页页缓存直读，不改页状态 |
| 取 `TPIDR_EL0`（解密必需） | `ptrace` 挂 GameThread | 内核直读 `task_struct->tpidr_el0`，不 ptrace |
| 读 maps 定位 RWX 区 | `/proc/PID/maps` | 内核读目标 `mm` 结构 |
| 解密 | PC Unicorn 模拟 `shellcode+0x9E000` | **同**（与读取解耦） |

### 16.4 执行流程（待用户实机）

1. **用户启动三角洲进对局**（坐标有值、shellcode 才注入）。
2. gg 无痕驱动抓：`libUE4` maps → RWX 区 940KB shellcode → 加密坐标(`+0x210`) → GameThread `TPIDR_EL0`。
3. PC 端 Unicorn 模拟 `shellcode+0x9E000` 解密，从 `+0x168` 读明文。
4. 版本偏移 37114 vs README 37110 可能微调 → 解构后若读歪，需重定位偏移（RWX 区 base 由 maps 动态取，shellcode 内偏移需实测校准）。

### 16.5 风险

- 解密依赖逆向偏移，**游戏大版本更新即失效**，需随版本重定位（与 CF 静态偏移同理，但三角洲多了 shellcode 一层）。
- Unicorn 模拟需精确还原寄存器/内存环境，否则解密歪；先用真实进程内存 dump 校准再批量跑。
