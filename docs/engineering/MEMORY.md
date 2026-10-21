# AI MEMORY — 会话持久记忆（自动维护）

> 本文件由 AI agent 自动维护：每次对话/任务中获取到的**对后续工作有用的事实**（环境、凭据、
> 路径、命令、踩坑结论、当前状态、遗留问题）都追加/更新到这里，避免未来会话重新发现。
> 追加时保持结构，按小节组织；过时信息就地更新（不改历史叙事）。

## 1. 环境事实（本机 / 设备 / VM）

### 1.1 主机（Windows）
- 项目根：`D:\project\workbuddy\工具\gg`（GG-MCP：GameGuardian MCP 服务器，C++/C）
- 关键工具链：
  - MinGW g++ 14.2：`D:\Kiln\mingw64\bin`
  - Android NDK r27d：`D:\Kiln\android-ndk-r27d-windows`（clang 18.0.4）
  - adb：`D:\Kiln\ADB\adb.exe`（**不在 PATH，需手动加**：`$env:PATH = "D:\Kiln\ADB;" + $env:PATH`）
  - Capstone 5.0.1：`D:\Kiln\capstone\capstone-5.0.1`（agentd disasm 依赖，已交叉编译出 arm64 `libcapstone.a`）
  - VMware Workstation：`D:\software\VMware\vmrun.exe`
  - Python：`C:\Users\AprYT\.workbuddy\binaries\python\python.exe`（二进制脚本用它）
  - PortableGit：`C:\Users\AprYT\.workbuddy\binaries\PortableGit\versions\1.2.0`（ssh/tar/bash）
- **批处理坑**：cmd 在 GBK 代码页下会误解析 UTF-8 中文 → .bat 必须纯 ASCII（build_ndk.bat/build_pc.bat/run_tests.bat 已修）
- **PowerShell 坑**：`$args` 是自动变量不能作参数名；`Set-Content` 写 CRLF（会毁内核源码）；多文件 `adb push` 不可靠（静默损坏/丢文件）→ 逐个 push + md5 校验

### 1.2 真机（REDMI K90 Pro Max）
- 型号：25102RKBEC，代号 myron/canoe，Android 17，**KernelSU root**（/proc/modules 有 kernelsu）
- 内核：**6.12.69-android16-6**（GKI + Xiaomi abogki 改动），clang 19.0.1 构建，CONFIG_LTO_NONE
- 关键内核配置：`CFI_CLANG=y`、**`CFI_ICALL_NORMALIZE_INTEGERS=y`**、`SHADOW_CALL_STACK=y`、
  `MODVERSIONS=y`、`MODULE_SIG=y`（**无 FORCE**，未签名模块可加载）、`KALLSYMS_ALL=y`、
  `MODULE_UNLOAD=y`、PREEMPT、4K 页、`ARM64_BTI_KERNEL` 关
- 预期 vermagic：`6.12.69-android16-6-4k SMP preempt mod_unload modversions aarch64`
- 设备序列号：134d2f8；测试游戏：登山赛车 com.fingersoft.hillclimb.noncmcc（进程常变，需现查）
- **QX 驱动**（/data/kagtim、/data/mxjvvm）：随重启消失（隐藏进程自删），需用户手动重新拉起；
  当前内存通道已由 rwproc33 接管，**QX 不需要**（除非要它的硬件断点，但无密钥 ENOSPC）
- MCP 服务器：`build\pc\gg-ghost-mcp.exe --config configs\gg-ghost.json --transport http --port 8788`
  （注意：旧实例会锁 exe 无法覆盖；后台任务退出即停，重启后需 adb forward 恢复）

### 1.3 构建 VM（Linux Mint 22.3）
- IP：192.168.42.128（VMware NAT），用户 **king**，密钥 `~/.ssh/vm_linux_ed25519`（workbuddy-vm-access）
- 已升配：8 vCPU / 8GB（vmx 修改需先 stop 再改再 start，否则 VMware 关机时重写 vmx）
- 内核构建树：`/opt/gki`（GKI android16-6.12 源码，SUBLEVEL=69，LOCALVERSION="-android16-6-4k"，
  CFI_ICALL_NORMALIZE_INTEGERS=y）；构建日志 /tmp/kbuild*.log；模块源码 /opt/rwProcMem33
- 已装：make/gcc/clang-18/lld/llvm/bison/flex/libelf-dev/libdw-dev（apt）
- 免密 sudo（king）

## 2. 当前系统状态（2026-08-16 晚）

- **agentd 后端 = rwproc33**（`scripts/start_agentd.sh`：KO_TYPE=rwproc + 两个 auth key）
- 已加载模块：`rwProcMem_module.ko`（Live）；**hwBreakpointProc_module.ko 未加载**（INST 崩溃）
- rwproc 通道全链路验证通过：kernel_status=kernel-driver、attach、read（含文件页）、
  view_memory、resolve_symbol、pointer_chain、disassemble
- 工具总数：43 个 MCP 工具；wire 命令 36 条（GGCMD_VIEW_MEMORY=34 / RESOLVE_SYMBOL=35 / POINTER_CHAIN=36）
- 设备侧部署目录：/data/local/tmp/ggmcp/（agentd + ko/ + start_agentd.sh）

## 3. rwproc33 适配关键结论（最值钱的踩坑）

### 3.1 模块构建（VM 上）
1. **KCFI 必须匹配设备**：gki_defconfig 默认 `CFI_ICALL_NORMALIZE_INTEGERS` 是关的，
   设备内核是开的 → 哈希不匹配 → insmod 后 5-7 分钟随机内核崩溃。
   修复：`scripts/config --file out/.config --enable CFI_ICALL_NORMALIZE_INTEGERS` + **`make syncconfig`**
   （仅 olddefconfig 不会同步进 include/config/auto.conf！）→ 编译行出现
   `-fsanitize-cfi-icall-experimental-normalize-integers`
2. **vermagic 精确匹配**：SUBLEVEL=69 + CONFIG_LOCALVERSION="-android16-6-4k" + 关 LOCALVERSION_AUTO
   → 零补丁；权威校验值从设备已加载厂商模块的 modinfo 提取
3. **MODVERSIONS CRC**：Xiaomi 改结构体 → 符号 CRC 与 vanilla GKI 大面积不同 →
   从设备全部厂商模块（`/vendor/lib/modules` + `/vendor_dlkm/lib/modules`，401 个）的 `__versions`
   建映射批量修补（`scripts/patch_ko_versions.py`）；无结构体依赖的符号（copy_from_kernel_nofault）天然匹配
4. **Windows checkout 内核源码两大坑**：CRLF 行尾（`sed -i 's/\r$//'` 修）；大小写撞名文件
   （xt_DSCP.c 等 13 对，Windows 丢一半 → `git archive HEAD <paths>` 补回）
5. **模块版本门控**：`MY_LINUX_VERSION_CODE=6.6.30`（作者最新分支，proc_maps.h 有 6.6.30 专属实现）
6. 6.x API：`__kmalloc`→`kmalloc`、`strlcpy`→`strscpy`
7. **hide_procfs_dir 必须禁用**（kprobe hook 崩溃风险）：置空 hide_procfs_dir.h + 注释调用
8. **agentd 侧 rwproc 读写用 is_force=0**（不翻转 PTE，避免与目标进程活跃页竞争崩溃）

### 3.2 协议要点（与上游源码核对过）
- 33 字节 packed 头 `{cmd,p1,p2,p3,buf_size}`；数据区在 buf+33；read() 返回值=结果长度
- OPEN_PROCESS / INST_HWBP 的句柄写回 **buf+33**，返回值 0
- INST_HWBP param3 = `len | (type<<8)`；UNINST/COUNT/DETAIL 的 param1 = 断点 ID（perf_event 指针）
- HWBP_HIT_ITEM packed 312 字节（my_user_pt_regs 288）；hit_addr 实现里 = 命中指令 pc
- GET_HWBP_HIT_COUNT 返回 {hit_total_count, hit_item_arr_count} 16 字节
- 命中列表**无清除命令**、累计至 1024 → watch_poll 按已消费数去重（agentd 已实现）

## 4. 遗留问题（按优先级）

1. **hwBreakpointProc_module（2026-08-17 进展）**：
   - ✅ **崩溃已修复**：根因 = 命中回调在 ARM64 调试异常上下文（中断/抢占关闭）里调用
     `modify_user_hw_breakpoint()` / `toggle_bp_registers_directly()` 与 kretprobe 回调里的
     `mutex_lock`/`copy_from_user` → "scheduling while atomic" BUG → 设备
     `CONFIG_PANIC_ON_OOPS=y` → 重启。**两个危险模式都已禁用**（ver_control.h）：
     `CONFIG_MODIFY_HIT_NEXT_MODE` + `CONFIG_ANTI_PTRACE_DETECTION_MODE`；
     handler 已重写为 raw_spinlock + 预分配环形缓冲 + 命中后清硬件槽 E 位（防无限重触发）
     （注：ver_control.h/handler 由并发 agent 改写，00:34 时间戳）
   - ✅ INST/POLL/UNINST 协议全过
   - ✅ **真实命中已捕获（2026-08-17 01:38）**：27 次硬件断点命中，带完整寄存器
     （x0-x30/sp/pc）+ pc 模块归属 + 32 条预反汇编！写者为注入游戏的 "watchtest" 测试模块
     （tid 16545, pc=0x5ff2f6e97c）
   - ✅ **线程绑定问题已解决**：模块 CMD_INST 只绑 pid 的 leader 线程（pid_task(PIDTYPE_PID)），
     游戏写入来自工作线程 → 单点注册命中率为 0。agentd watch.c 已改为**多线程注册**
     （枚举 /proc/pid/task，主线程 + 前 3 个线程各注册一个断点，最多 GG_RWPROC_MAX_BPS=4 个
     WRP 槽；watch.h rwproc_bp_ids[]/hit_consumed[] 数组化；poll 逐 bp 聚合 + 去重）。
     实测 `bps=4/80`（游戏 80 线程覆盖 4 个）
   - ⚠️ 设备上并发 agent 的调试产物：hwbp_dbg/fix/prod/b.ko + 曾运行 strace 自动重启循环
     （已停）；另有注入游戏的 watchtest 测试模块（验证用）
   - 遗留小项：多线程注册目前取"前 3 个线程"（readdir 顺序），未按 CPU 活跃度排序；
     命中指令的"被访问地址"模块未给出（hit_addr 实现里=pc），需从寄存器推断
2. **agentd 死循环 bug（已修复 2026-08-17）**：rwproc 读对无物理映射的页返回 **0 字节**
   （force=0 模式），搜索/指针扫描循环 `if (got < 0)` 未处理 0 → `rd=0` → 死循环占死互斥锁
   （strace 里 `read(4,"",262177)=0` 无限重复）→ 全部调用挂起。
   已修：search.c 所有扫描循环 `got < 0` → `got <= 0`（pointer_scan line 476 是最后一个，
   精确/区间/模糊循环此前已被并发 agent 修好）。
   教训：**驱动读返回 0（非负）也是异常路径，扫描循环必须推进**。
3. **GG 核心对接**（gg_kernel_load_gg）：libKernelGg.so 只认 dev/qx 协议，rwproc 模式走
   process_vm_readv 降级；若依赖 GG 本体需改 native/hook/hook.c 支持 rwproc 通道
4. **搜索性能**：rwproc 逐页物理读，system_server 全匿名区扫描 >5min；游戏足迹小可用
   （实测游戏 268MB 匿名区模糊快照 init 秒级完成，refine(changed) 秒级）
5. 服务器/设备重启后：需 `adb forward tcp:19100 tcp:19100` + `chmod 755 gg-agentd`（push 重置权限）
   + 重 insmod 模块 + 启动 agentd

## 5. 常用命令速查

```powershell
# SSH VM
ssh -i "$env:USERPROFILE\.ssh\vm_linux_ed25519" -o StrictHostKeyChecking=no king@192.168.42.128 <cmd>
# VM 内核模块构建
cd /opt/gki && make ARCH=arm64 CC=clang LLVM=1 O=out M=/opt/rwProcMem33/... modules -j8
# 设备 insmod（ko 目录 root 属主，push 需先到 ggmcp/ 再 su cp）
adb shell "su -c 'cp -f /data/local/tmp/ggmcp/X.ko /data/local/tmp/ggmcp/ko/ && insmod /data/local/tmp/ggmcp/ko/X.ko'"
# MCP 调用（HTTP）
# 见 McpCall 函数模式：POST /mcp 的 tools/call
# 修补 CRC
python scripts\patch_ko_versions.py build\devmod\all build\ko\X.ko
```

## 6. 会话历史速记（2026-08-16）
> 注（2026-10-15）：以下为当时的事实记录，保持原样。其中 **IDA 联动已于 2026-10-15 整体移除**
> （`server/ida_client.*`、`tests/unit/test_ida_client.cpp` 已删除；`gg_resolve_symbol` 现为纯本地静态解析，
> 响应不再含 `ida` 字段）。旧实现可从 git 历史恢复。
- 本会话交付：gg_view_memory / gg_resolve_symbol(+IDA 预留) / gg_pointer_chain / rwproc33 后端全链路
- 关键文件：agentd/{driver.c,main.c,watch.c,elfsym.c}, server/{ida_client.*,session.cpp}, docs/engineering/RWPROC33_ADAPTER.md
- 新工具脚本：scripts/{patch_ko_versions.py, extract_kernel.py, patch_module_layout_crc.py}
- 设备当前（22:5x）：rwProcMem Live、agentd pid 19195 运行中、MCP 服务器后台任务运行中
