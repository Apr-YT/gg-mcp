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
│  AI agent ──MCP──▶ gg-mcp-server (HTTP :8788)               │
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
| `agentd/driver.h` | +`RWPROC_CMD_GGTOUCH_TAP=15/SWIPE=16`、`rwproc_gg_touch_tap/swipe_t` 结构、`driver_touch_tap/swipe` 声明 |
| `agentd/driver.c` | +`driver_touch_tap/swipe` 实现（`rwproc_call()` 通用调用，data 传结构） |
| `agentd/main.c` | +`h_ghost_tap/h_ghost_swipe` handler（JSON→结构）、cmd_name 映射、dispatch 2 case |
| `shared/wire_protocol.h` | +`GGCMD_GHOST_TAP / GGCMD_GHOST_SWIPE` |

### 3.3 PC 端（gg-mcp-server）

| 文件 | 改动 |
|---|---|
| `server/mcp/schema.cpp` | +`gg_ghost_tap` / `gg_ghost_swipe` 工具注册（含参数 schema 与描述） |
| `server/session.cpp` | +`tool_ghost_tap/swipe` 函数、`kTools[]` 表 2 条目（`write_op=true`） |

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

**6.12 适配已知坑**：
1. `__kmalloc` 宏签名变 → 用 `kmalloc`
2. `strlcpy` 已移除（6.10+）→ 用 `strscpy`
3. `proc_maps.h` 无 6.12 实现分支 → 复用 6.6.30 分支（条件加 `>= 6.12.0`）
4. `mm->rss_stat` 类型变（6.6+ 为 `struct percpu_counter[]`）→ `linux_kernel_api.h` 已有 `>= 6.6.0` 分支自动处理

### 5.2 设备端（gg-ghost-agentd，NDK）

```bat
aarch64-linux-android28-clang -O2 -Wall -I%ROOT% -I%CAPSTONE%\include -o gg-ghost-agentd ^
  main.c driver.c process.c search.c freeze.c version.c loader.c watch.c disasm.c elfsym.c ^
  %ROOT%\third_party\cJSON.c %CAPSTONE%\libcapstone.a -pie
```

### 5.3 PC 端（gg-mcp-server，MinGW）

```bat
g++ -std=c++17 -O2 -Wall -Wextra ^
  server\main.cpp server\mcp\protocol.cpp server\mcp\schema.cpp server\session.cpp ^
  server\ida_client.cpp server\annotations.cpp server\device\wire.cpp ^
  -o gg-ghost-mcp.exe -lws2_32
```

---

## 6. 部署（设备 /data/adb/GG-Ghost/）

```
/data/adb/GG-Ghost/
├── GG-Ghost.sh            # 包装启动脚本（start/stop/status）
├── gg-ghost-agentd              # 设备端守护进程
├── libKernelGg.so.gz      # GG 核心 hook so（gzip 压缩存储）
├── lib5_stub.gz           # GG 核心 ELF 桩（gzip 压缩存储）
├── ko/
│   └── rwProcMem_module.ko  # 内核驱动（内存读写 + gg-ghost 触摸）
└── README.md              # 部署说明（简版）
```

### 启动脚本特性

- **so 防落盘**：so 以 gzip 压缩存放，`start` 时自解压到 `/data/local/tmp/gg-ghost-runtime/`
- **3 秒自动删除**：启动完成后 `sleep 3 && rm -f` 解压产物（防检测/取证，不留长期文件）
- **驱动自动加载**：检查 `/proc/modules`，未加载则 `insmod`
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

---

## 8. 反检测特性

| 特性 | 说明 |
|---|---|
| **标准输入管线** | 事件经 input core 分发，非注入 API，无 INJECT_EVENTS 痕迹 |
| **无新设备暴露**（用户态通道） | `sendevent` 直写真实触屏 `/dev/input/event6`（focaltech_ts）——复用真实设备，InputManager 零感知 |
| **虚拟设备可枚举**（内核通道） | `gg-ghost` 设备在 `dumpsys input` / `getevent` 可见（Device 10 / input9）——游戏可枚举输入设备检测（少见，注意此面） |
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

---

## 10. 工具链备忘

| 组件 | 位置 |
|---|---|
| 驱动源码 | `D:\project\workbuddy\工具\gg-rwproc33-driver\rwproc33\`（git: Apr-YT/gg-rwproc33-driver） |
| gg-mcp 源码 | `D:\project\workbuddy\工具\gg\`（git: Apr-YT/gg-mcp） |
| 设备端工具 | `D:\project\ai_workspace\scripts\gg-ghost-touch.c`（用户态 + 内核双模式） |
| 编译环境 | Linux Mint VM（king@192.168.42.128，key: `~/.ssh/vm_mint_ed25519`，内核树 /opt/gki） |
| 构建产物 | `gg\build\agentd\gg-ghost-agentd`、`gg\build\pc\gg-ghost-mcp.exe`、`gg\build\native\libKernelGg.so/lib5_stub` |
| 部署目录 | 设备 `/data/adb/GG-Ghost/`；PC 暂存 `D:\project\ai_workspace\temp\gg_hillclimb\GG-Ghost\` |

### 相关命令速查

```bash
# 用户态触摸（无内核虚拟设备暴露）
adb shell /data/local/tmp/gg-ghost-touch tap 2035 918 150
# 内核触摸（走 gg-ghost 虚拟设备）
adb shell /data/local/tmp/gg-ghost-touch -p /proc/<MEM_KEY>/<MEM_KEY> kerneltap 2035 918 150

# gg-mcp-server 启动
cd D:\project\workbuddy\工具\gg && build\pc\gg-ghost-mcp.exe --config configs\gg-ghost.json --transport http --port 8788
```

---

## 11. 后续规划

- [ ] **金币定位闭环**：触摸链路已通 → 进游戏消费/赚取金币 → `gg_search_refine` 收敛唯一地址 → `gg_set_results`/`gg_freeze` 验证
- [ ] **手势宏**：驱动侧加 swipe 多段曲线、双击、长按序列
- [ ] **GG-Ghost 伪装**：虚拟设备参数对齐真实触屏（vendor/product/name），降低设备枚举检测面
- [ ] **配置收敛**：GG-Ghost.sh 参数（PORT/KEY/KO 路径）抽到独立 config 文件
- [ ] **Magisk 自启动**：接入 service.d 自动拉起
