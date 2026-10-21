# HANDOVER.md — 项目交接文档（GG-MCP · 内核级 GameGuardian MCP 服务）

> ⚠️ **2026-10-15 更新 —— 下面正文是 08-16 的历史记录，其中的路径与命名已过时，勿直接照抄：**
> - 仓库路径：`D:\project\workbuddy\工具\gg` → **`D:\Kiln\gg-mcp`**（GitHub `Apr-YT/gg-mcp`）
> - PC 产物：`gg-mcp-server.exe` → **`gg-ghost-mcp.exe`**（提交 b953bc5「命名统一」）
> - 配置文件：`configs/ggmcp.json` → **`configs/gg-ghost.json`**（前者全仓已不存在）
> - adb：`D:\Kiln\ADB\adb.exe` → **`D:\Kiln\platform-tools\adb.exe`**（已入 PATH）
> - MinGW：14.2 → **16.2.0**；~~NDK / capstone 本机未安装~~ → **2026-10-21 已就位**：
>   NDK **r30**（`D:\Kiln\android-ndk-r30`）+ capstone 5.0.1（`D:\Kiln\capstone\capstone-5.0.1`），
>   agentd 可本地编译，步骤见 `docs/TOOLCHAIN.md`
> - 对外工具数 43 → **45**；wire 命令数 36 → **41**
> 现状一律以 `AGENTS.md` 为准。
>
> ⚠️ **2026-10-21 追加：GG 核心注入链路（`lib5_stub` + `libKernelGg.so`）已整条删除。**
> 本文中涉及 `lib5_stub` / `libKernelGg.so` / `gg_kernel_load_gg` / `gg_core_versions`
> 的段落均为**历史记录**，相应文件与工具已不存在。详见 `DRIVER_MATRIX.md` §4。

> 交接时间：2026-08-16
> 项目路径：`D:\project\workbuddy\工具\gg`
> 设备：Redmi myron（25102RKBEC）序列号 `134d2f8`，KernelSU root，内核 `6.12.69-android16-6`，SELinux Permissive
> 工具链：MinGW g++ 14.2（PC）、Android NDK r27d（`D:\Kiln\android-ndk-r27d-windows`）、adb（`D:\Kiln\ADB\adb.exe`）

---

## 1. 项目目标

PC 上的 AI agent 通过 **MCP（Model Context Protocol）** 直接控制安卓设备的 GameGuardian 能力：
内存搜索/修改/冻结/指针扫描等。**只通过 KO 内核驱动通信**（不用 KPM/KPatch-Next），
GG 核心采用**外挂圈子加载内核方式**（ELF 桩 + LD_PRELOAD hook syscall 270），
支持**版本族容错**（同类型如 `6.12.xx`/`101.xx`，补丁号不同均可加载）。

## 2. 架构

```
AI agent(PC) ──MCP(stdin / HTTP)──▶ gg-mcp-server (C++17, PC)
                                       │ 自定义二进制协议 (shared/wire_protocol.h)
                                       │ adb forward tcp:19100
                                       ▼
                          gg-agentd (C, 设备端 NDK arm64, root)
                              ├─ 驱动四后端: QX(隐藏进程发现+ioctl) / rwproc33(开源,硬件断点) / hook 型 KO / dev 型 KO
                              ├─ 降级链: 驱动 → process_vm_readv → /proc/pid/mem（ko_downgrade 状态机防误杀）
                              ├─ GG 核心对接: lib5_stub(ELF桩) + libKernelGg.so(hook syscall 270)
                              └─ 进程伪装: dspbridge 改名 + cmdline/环境区清空
```

- **wire 协议**：20 字节二进制头 `{magic,ver,cmd,seq,flags,payload_len}` + JSON 负载（`{"p":{}}`/`{"r":{}}`/`{"e":{code,message}}`），定义在 `shared/wire_protocol.h`（PC/设备唯一事实源）
- **MCP 工具 45 个**（2026-08-28 实测校验 schema↔dispatch 一致；交接时为 43，后新增 gg_touch/gg_swipe 触摸与 ghost 命令）：进程5/搜索7/读写8/分析7/内核3/系统3/监视反汇编5/标注5+触摸

## 3. ✅ 已完成并真机验证（2026-08-16）

| 项 | 结果 |
|---|---|
| 引擎全链路 itest（搜索→读→写→冻结→解冻） | **ALL PASS** |
| QX 驱动发现+对接 | ✅ `/dev/ggqx (maj 435 min 0, pid 16916)` |
| agentd 自检 | ✅ `state=kernel-driver (dev-ko ioctl)`，ioctl 0x801/0x802 读写正常 |
| gg_core_versions | ✅ 找到 `GG-qolw`，version=`101.1`，family_match=true（~600ms） |
| wtest 同连接 3 帧（kernel_status/core_versions/list_proc） | ✅ 全部响应 |
| GG 本体分析 | ✅ `宇航员修改器_太空旅客.apk` = **GameGuardian 101.1**（包名伪装 `com.tencent.gamereva`，version.gg=`16142:101.1`，运行时核心 `files/GG-qolw/lib5.so` 4.66MB AArch64） |
| 本机单测 | ✅ 33 例全过（版本族 21 + wire 11 + QX 结构布局 1） |

**设备端关键事实**：
- QX 驱动进程：`/data/kagtim`(pid 16916)、`/data/mxjvvm`(pid 21505)，均为 `(deleted)` 隐藏进程
- GG 应用在运行（com.tencent.gamereva，uid 10246）；GG 核心进程 lib5.so（pid 9561 附近）
- 设备上还有王者荣耀(com.tencent.tmgp.sgame)、CF(com.tencent.tmgp.cf) 等游戏

## 4. ⚠️ 未解决问题（下一步优先）

### 4.1 ✅ 已修复（2026-08-16 晚）：gg-mcp-server 客户端同连接第 2 次调用超时（WSAETIMEDOUT 10060）

**现象**：同一 TCP 连接上第 1 次 MCP 调用成功（如 gg_kernel_status），第 2 次调用（如 gg_core_versions）
读响应超时。实测超时 ≈300ms（不是配置的 300s），报 `err=10060 (WSAETIMEDOUT)`。
重试（新连接）同样超时。**agentd 完全正常**（wtest 原始客户端同连接 3 帧全通，agentd 进程稳定存活）。

**根因（已定位）**：`server/device/wire.cpp` 的 `tcp_connect()` 里，Windows 上
`setsockopt(SOL_SOCKET, SO_RCVTIMEO/SO_SNDTIMEO, ...)` 传的是 8 字节 `timeval{300, 0}`，
而 **Winsock 期望的是 DWORD 毫秒值**（Linux 才是 timeval）→ 只取前 4 字节 `300` = **300ms 超时**。
快调用（<300ms，如 kernel_status）碰巧通过；慢调用（如 gg_core_versions 设备端 ~600ms）必超时。
wtest 之所以全通，是因为它根本没设 SO_RCVTIMEO（纯阻塞读）。

**修复**：平台分支——Windows 传 `DWORD io_ms`，POSIX 保留 timeval（见 wire.cpp `tcp_connect`）。
启动日志现打印 `[wire] timeout cfg: io=300000ms (winsock DWORD ms)`。

**验证（真机）**：同连接连续 4 帧 kernel_status(179ms) → core_versions(726ms) → list_processes(106ms)
→ device_info(28ms) **全部成功**；`gg_core_versions` 返回 GG-qolw `101.1` `family_match=true`。
单测 29(版本族)+11(wire) 全过。

### 4.2 其他待办

- **QX 密钥** ✅ 已诊断（2026-08-16）：`init_key`(0x800) 为**可选**——agentd `driver.c` 与 hook 库仅在配置了密钥时才发送；
  当前设备 QX9-10 驱动无密钥时 0x801/0x802 读写完全正常（itest 全链路 ALL PASS + 自检 kernel-driver + 实测读/写/搜索）。
  `qx_key_set=false` 不是功能问题，`start_agentd.sh` 的 `QX_KEY` 保持为空即可；仅当驱动强制要求认证时才需填入。
  ⚠️ **但驱动硬件断点（0x807-0x811）无密钥时返回 ENOSPC**（疑似 init_key 锁高级功能），ptrace 断点也 ENOSPC
  （硬件槽被驱动占用）→ **CE 式"什么访问了这个地址"在本设备暂不可用**，等拿到密钥后可启用。
- **金币修改实战（登山赛车）** ✅ 2026-08-16 晚：内存狩猎（float/dword、权威 vs 副本、局内 vs 菜单）走了很多弯路后，
  **存档直改才是正解**——该游戏启动时读 `shared_prefs/hillclimbprefs.xml` 的 `coins`（int），
  强停→改 9999→重启→HOME 存档验证 **9999 生效**（游戏另有 +35 每日奖励，实测得 10034）。
  经验：改存档前先 `am force-stop`（防退出写回覆盖）；备份放 `/data/local/tmp/ggmcp/`（放 shared_prefs 目录会被游戏清掉）。
  内存路径也验证了新增的 **精确搜索 refine**（搜已知值→游戏内变化→`gg_search_refine increased` 收窄，
  175→210 从 4874 缩到 80）——这是 GG 标准流程，之前缺失现已补齐。
- **Qx 附加命令**（0x804-0x812）✅ 已接入协议定义（用户提供 `qx-对接.h`）：driver.c 已实现
  `qx_hwbp_add/get_hits/enable/disable/clear`（0x807-0x811），`gg_watch_set/poll/clear` 工具已注册。
  但真机 HWBP_ADD 返回 ENOSPC（见上：疑似 init_key 门控），功能待密钥验证。

## 5. 🔧 调试中已修复的问题（经验记录，重要）

| # | 问题 | 根因 | 修复 |
|---|---|---|---|
| 1 | agentd "神秘死亡" | **adb push 从 Windows 重置文件权限为 644**，二进制不可执行，启动脚本却谎报成功 | `start_agentd.sh` 增加 `-x` 检查 + 启动后 pgrep 验证；部署后必须 `chmod 755` |
| 2 | agentd 被杀死 | 客户端断开后写响应触发 **SIGPIPE**，默认终止整个进程 | `signal(SIGPIPE, SIG_IGN)` + accept 循环永不退出 |
| 3 | gg_core_versions 卡死（分钟级） | loader 对**每个包**的 files/ 做 ELF 兜底扫描 + dumpsys 版本查询（数百包 × 海量文件） | 只对 `GG-` 前缀目录做完整扫描；其他目录仅快速 stat lib5.so/lib5.so.primary；版本优先读 version.gg（权威，毫秒级） |
| 4 | `family_match=0`（版本族匹配失效） | `version_parse("101.*")` 提取 `"101."`，sscanf 只解析出 1 段 → minor=0 | 重写 `version_match_family` 为 **token 化通配**（`*`=通配段），支持 `101.*`/`101.1.*`/`6.12.9` 全形态 |
| 5 | QX 驱动发现失败 | 该内核 **readlink 会把 ` (deleted)` 拼进目标路径**，`/data/kagtim (deleted)` 长度 21≠12 被过滤 | readlink 后剥离 `" (deleted)"` 后缀再匹配 |
| 6 | itest 段错误（scudo heap corruption） | `encode_value_local` 契约写 8 字节，调用方给了 `uint8_t[4]` → **栈溢出**破坏相邻 freeze 结构体 | 缓冲改 `[8]`；顺带修了 freeze 判定竞态（采样计数）与子进程自增时序（SIGUSR1 控制） |
| 7 | 同连接第 2 帧失败（早期） | gg-mcp-server 每次连接都 popen `adb forward`，**Windows 下反复执行会打断现有监听** | 改为**先直连，失败才 forward** + 重试 |
| 8 | C 注释被 `*/` 提前结束 | `/data/user/0/*/files` 里的 `*/` 序列 | 改写注释 |
| 9 | 控制台中文乱码 | Windows 控制台代码页 | `SetConsoleOutputCP(65001)` |
| 10 | 同连接第 2 次调用超时 WSAETIMEDOUT(10060) ≈300ms | Windows `setsockopt(SO_RCVTIMEO)` 期望 **DWORD 毫秒值**，代码却传 8 字节 `timeval{300,0}` → 前 4 字节 = 300ms；慢调用（gg_core_versions ~600ms）必超时 | 平台分支：Windows 传 `DWORD io_ms`，POSIX 传 timeval；真机同连接 4 帧全通 |
| 11 | 64 位进程 attach 误报 "32-bit target not supported" | `process_is_64bit` 只看 **maps 首行**地址宽度；64 位 ART 进程首行常是低地址 dalvik 匿名映射（boot.art @0x02ddc000, 8 位 hex）→ 误判 32 位（登山赛车 arm64 中招） | 改为：优先看 exe 链接（app_process64/32），否则**扫描全部 maps**，任一地址 >8 位 hex 即 64 位 |
| 12 | `gg_kernel_load_gg` 报 permission denied | loader 直接 `O_TRUNC` 覆盖 **正在被 GG 核心 exec 运行**的 lib5.so → 内核 ETXTBSY ("Text file busy") | `install_file()`：写临时文件后 **rename 覆盖**（rename 运行中可执行文件合法）；loader_restore 同样处理 |
| 13 | **rwProcMem33 hwBreakpointProc_module：`gg_watch_set` 触发内核静默挂死/看门狗重启** | **命令枚举错位**：模块头枚举从 0 开始（`CMD_OPEN_PROCESS=0`），用户态（agentd `RWBP_CMD_*`/协议）按 1 起始 → 所有命令错位 +1。`gg_watch_set` 发 cmd=1（以为 OPEN）实际触发 CLOSE → `put_pid((struct pid*)pid)` 解引用用户态整数 → 内核数据异常 → 整机挂死（设备 `panic=-1` + 看门狗重启，零内核日志） | 模块枚举显式 `=1` 起始（`CMD_OPEN_PROCESS=1...CMD_HIDE_KERNEL_MODULE=12`）。另做防御性修复：命中回调改 raw_spinlock+预分配缓冲（原 mutex/vmalloc/modify 在 ARM64 调试异常上下文必崩）、命中后清硬件槽 E 位防重入、禁用 MODIFY_HIT_NEXT_MODE/ANTI_PTRACE/hide kprobe。真机全流程验证通过。详见 `docs/engineering/RWPROC33_ADAPTER.md §10.4` |
| 14 | **`gg_search` 大范围匿名区死循环卡死（250s+ 不返回），agentd 表现"被杀/无响应"** | **rwproc 驱动对无效/只读页返回 0 字节（非负）**，`search_exact` 只处理 `got < 0`：`got==0 → rd=0 → len 不减` → 同一地址无限重读（strace 实测每 50μs 一次 `read(4,"",262177)=0`），持 `g_eng.mtx` 占满 CPU，其他连接全部阻塞，表现类似 agentd 崩溃/被杀（无 tombstone、无 crash 记录，dmesg 只见 init 的 Untracked SIGTERM/SIGKILL） | `search_exact`/`search_range`/`search_refine` 三处 `if (got < 0)` 改 `if (got <= 0)`：空读也推进块（`len-=rd; addr+=rd`），杜绝死循环。真机验证：登山赛车匿名区搜索 9999 → 250s 死循环 → **270ms 返回 count=31**；连续 3 次大搜索 250-350ms 稳定返回；itest/PC 单测全过 |
| 15 | **`gg_watch_inject`/断点命中 enrich 时 agentd 段错误崩溃（SIGSEGV, tombstone 有记录）** | **use-after-free**：`process_resolve_addr`（process.c）中 `hit_path` 指向 `maps` 数组内部，`free(maps)` 释放后仍 `snprintf(path, pathsz, "%s", hit_path)` → 堆复用后读到垃圾地址 → `__strlen_aarch64` 崩溃（tombstone: `#04 process_resolve_addr+268`）。watchtest 测试碰巧堆内容未破坏所以成功；游戏进程 maps 大、堆复用激进时必崩 | `free(maps)` 前先把 `hit_path` 拷贝到局部缓冲（`hit_path_copy[GG_MOD_NAME_MAX]`）。真机验证：`gg_watch_inject` 对游戏 libgame.so 注入 pc → 不再崩溃，正确解析 `module=libgame.so base=0x7aa1a74000 offset=0xac000`；watchtest 全流程（attach→watch_set→poll→64 命中→module/offset/stack）正常；itest 无回归 |

## 6. 当前设备状态（交接时）

```
agentd      : 运行中 (--driver qx, kernel-driver)   ← 含 process_is_64bit/refine/watch/conn日志
QX 驱动     : /data/kagtim (pid 16916, deleted) + /data/mxjvvm (pid 21505)
adb forward : tcp:19100 tcp:19100 ✅
GG 101.1    : 已装 com.tencent.gamereva，已还原原版，核心进程正常运行
登山赛车    : com.fingersoft.hillclimb.noncmcc 已装并运行（演示目标，arm64）；金币已改 10034
PC 服务器   : build\pc\gg-mcp-server.exe --transport http --port 8788
APK         : apk\build\ggmcp.apk（已装 com.ggmcp.app，GG 风格 UI：状态/进程列表/搜索/结果）
```

## 6.5 APK（2026-08-16 晚新增）

- 源码 `apk/`（Java，无第三方依赖，org.json 用系统内置）；构建 `scripts/build_apk.ps1`（aapt2/d8/apksigner 纯命令行，
  ASCII 暂存目录 `D:\Kiln\apk-stage` 规避 aapt2 中文路径问题；keystore 持久化在 `apk\ggmcp.keystore`）
- 架构：APK 内嵌 gg-agentd（assets）+ su 启动（EngineManager），UI 走 wire 协议连 127.0.0.1:19100（WireClient 每次调用新连接）
- **UI 为 GG 修改器风格**（深色主题 + 底部标签 搜索/内存/进程/设置 + 类型页签 BYTE..DOUBLE + 新搜索/继续/模糊/区间 +
  refine 四键 增加/减少/未变/变化 + 结果行 地址|值|十六进制 + 冻结勾选 + 内存 hex 编辑器）
- 真机已验证：状态页（kernel-driver/QX）、进程列表+过滤+附加、精确搜索（dword 10034 → 13 命中）+ GG 格式结果列表、内存 hex 读取
- **踩坑记录**：① aapt2 不认中文路径（"工具\gg"）→ ASCII 暂存；② javac 中文注释需 `-encoding UTF-8`；③ **agentd 响应是裸对象**
  （无 `{"r":}` 包装）→ WireClient 需兼容（PC wire.cpp 早有同样处理）；④ **MIUI 会冻结 app 导致 socket 挂起** →
  `dumpsys deviceidle whitelist +com.ggmcp.app` + `am set-standby-bucket ... active`；⑤ 状态文本勿含原始 JSON（双引号破坏 uiautomator dump）；
  ⑥ **javac `-bootclasspath` 下 lambda 报"找不到符号"** → 改 `-classpath android.jar`；⑦ ScrollView 在 weight 布局里塌缩为 0 高 → 直接 LinearLayout
- 下一步可选：悬浮窗（GG 的悬浮球）、冻结勾选联动列表刷新、AI 自动分析（参考 B 站「2026年游戏逆向天花板：AI内核全自动分析」）

## 6.6 反汇编（Capstone 集成, 2026-08-16 晚）

- agentd 静态链接 **Capstone 5.0.1**（仅 AArch64，libcapstone.a 4.6MB 交叉编译自 NDK r27d，
  源码 `D:\Kiln\capstone\capstone-5.0.1`，构建 `D:\Kiln\capstone\build-arm64`；cmake 在 `D:\Kiln\cmake`）
  ⚠️ CMake 选项名是 **`CAPSTONE_ARM64_SUPPORT`**（不是 AARCH64），且 `CAPSTONE_ARCHITECTURE_DEFAULT=OFF` 时才生效
- 新命令 **GGCMD_DISASSEMBLE(33)** + MCP 工具 **`gg_disassemble {addr, length}`**（默认 64B，上限 4KB）：
  返回 `{arch,start,bytes_read,count,insns:[{addr,bytes,mnemonic,op_str,comment}]}`
  - 注释启发式：call/jump（解析目标地址）/return/load/store/addr/syscall（Capstone groups + 助记符）
  - agentd 文件：`agentd/disasm.{c,h}`；编译需 `-I<capstone>/include` + `libcapstone.a`
- **断点命中 → AI 自动反汇编 ±32 条**（调用模式）：
  ```
  gg_watch_set {addr}            # 设写监视点
  gg_watch_poll {}               # 命中 → hits[].pc 为写者指令地址
  gg_disassemble {addr: pc-128, length: 256}   # ±32 条指令（ARM64 定长 4B）
  # AI 分析反汇编结果 → 定位权威变量/逻辑
  ```
- **命中日志增强（缺口 3, 2026-08-16）**：`gg_watch_poll` 对每条命中自动附带：
  - **模块归属**：`module`（basename）/`module_path`/`module_base`/`module_offset`（pc−base，process_resolve_addr）
  - **调用栈回溯**：`stack`（FP 链, 最多 5 层, stack_pc[0]=命中 pc；链结束帧返回地址计入）
  - **预反汇编**：`disasm`（pc±16 条 = 32 条, 复用 disasm_a64）
  - 寄存器扩展：`fp/sp/lr`（x29/sp/x30）
  - **`gg_watch_inject {pc,fp,sp,lr}`** 调试工具：断点不可用环境手动构造命中走完整 enrich 管道
  - 真机验证（注入 + 手工 FP 链）：libc.so+0x70000 / 栈 3 层 / 32 条指令 / `bti c`
  - ⚠️ 回溯前提：目标代码编译时保留 FP（x29）；-fomit-frame-pointer 的代码只能回溯 1 层
  - ⚠️ 构造测试链须用已验证可写堆地址：QX 驱动对**部分无效/只读页"假装写成功"**（写返回 bytes 但读回 0）

## 6.7 AI 标注库（跨会话记忆, 2026-08-16 晚）

- **用途**：AI 逆向发现字段后保存标注（JSON 持久化），后续会话用 `gg_annotate_context` 注入上下文，形成跨会话"记忆"
- **实现**：PC 端 `server/annotations.{h,cpp}`（AnnotationStore，mutex + 写盘 `annotations.json`，路径可配 `annotations_file`）；
  纯本地工具，不经过 agentd
- **工具**（5 个）：
  - `gg_annotate_add {tag, description, address, module, module_offset, type, size, frozen, confidence, notes}`
    ——按 tag 去重合并（保留 created，刷新 updated）
  - `gg_annotate_list {tag?}` / `gg_annotate_get {tag}` / `gg_annotate_remove {tag}`
  - `gg_annotate_context {tag?}` ——生成纯文本上下文（AI 会话开始/分析前调用注入）
- **推荐字段**：module+module_offset（进程重启地址会变，模块偏移可重定位）；address 仅作参考
- 真机验证：add/list/get/context/更新合并/磁盘持久化全部通过（工作目录 `annotations.json`，已存 coins/fuel 示例）
- 真机验证：attach agentd 自身 → `gg_disassemble libc+0x70000` → **64 条真实指令**（memcpy 风格函数：
  bti/ld1/cmeq/shrn + 分支目标解析 + load/store/return 注释）
- ⚠️ 已知怪癖：登山赛车某次重启后（pid 28401）libgame.so 文件映射经 QX 驱动**读全零**（libc/堆正常），
  疑似驱动对个别进程文件页的读取问题；换进程/换地址正常，反汇编工具本身无碍

## 6.8 AI 结果实时同步（2026-08-17 新增）

- **用途**：AI（PC 端）搜索/refine/冻结时，手机悬浮窗搜索页**实时同步显示**同一批结果（地址+当前值+冻结勾选+状态栏计数），与 GG 修改器体验一致；标注同步（§6.7 中继）外补上"数据同步"一环
- **实现**：agentd 新增 `results_update` 事件广播（`agentd/main.c event_results_update`）：
  - 触发点：`gg_search`/`gg_search_range`/`gg_search_refine`/`gg_clear_results`/`gg_set_results`/`gg_freeze`/`gg_unfreeze`
  - 载荷：`{count, frozen_count, results:[{index, addr, value, value_hex, frozen}]}`，前 100 条且**冻结项优先**（前 50 条带当前值，避免事件过大）
  - app 端 `OverlayUi.handleResultsUpdate`：刷新 `resultAdapter`（含冻结 HashSet）+ infoText + 状态栏 + 日志页一行"AI 搜索结果同步"
- 真机验证：AI 搜 7777 → app 显示"命中 21 个地址" + 结果行 `0x239e768`；AI 冻结 → app 勾选框自动打上 + "命中 21 个地址 · 冻结 1"
- ⚠️ 事件载荷上限：结果 >100 条时 app 只显示冻结项+前 100 条（GG 也是分页）；模糊搜索物化后同样生效

## 7. 使用流程（修通 4.1 后）

```bat
rem ① 构建
scripts\build_pc.bat && scripts\build_ndk.bat

rem ② 部署（QX 模式已默认；KO 文件放 build\ko\）
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -GgApk "C:\Users\AprYT\Downloads\宇航员修改器.zip"

rem ③ 启动 PC MCP 服务器
build\pc\gg-mcp-server.exe --config configs\ggmcp.json --transport http --port 8788

rem ④ 验证
powershell -ExecutionPolicy Bypass -File scripts\verify.ps1
```

AI 客户端配置见 `scripts/mcp_config.example.json`（stdio 与 http 两种）。

## 8. 代码索引

| 路径 | 说明 |
|---|---|
| `server/` | PC 端 C++ MCP 服务器（`main.cpp` 入口、`mcp/protocol.cpp` MCP 协议、`mcp/schema.cpp` 30 工具、`session.cpp` 工具实现、`device/wire.cpp` wire 客户端【⚠️ 4.1 问题所在】） |
| `agentd/` | 设备端 C 守护（`driver.c` KO/QX 驱动层、`search.c` 搜索引擎、`freeze.c` 冻结、`loader.c` GG 核心对接、`version.c` 版本族匹配） |
| `native/` | GG 核心对接（`hook/hook.c` syscall 270 拦截、`stub/lib5_stub.c` ELF 桩） |
| `shared/wire_protocol.h` | PC↔设备协议唯一事实源 |
| `scripts/` | 构建/部署/启动/KO 安装/verify |
| `tests/` | 单测（PC）+ 设备测试（itest 引擎全链路 / ltest loader / qtest QX 发现 / wtest wire 客户端） |
| `docs/engineering/` | ARCHITECTURE / PROTOCOL（含 QX 协议） / VERSION_MATCHING / ACCEPTANCE |

## 9. 合规提醒

仅供技术研究与学习。勿用于联网游戏作弊（可能违反服务条款并被封号）或非法用途；
`configs/ggmcp.json` 的 `safe_mode: true` 可启用只读会话。
