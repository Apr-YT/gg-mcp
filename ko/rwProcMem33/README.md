# 驱动1名称：Linux ARM64内核硬件进程内存读写驱动39
本驱动支持所有能解锁BL的手机，无论小米、黑鲨、红魔、ROG、一加、三星、摩托罗拉等等，并且不需要手机厂商开放内核源码。只需要手动修改五六处地方，就可以跑在任意机型的内核上！具体修改过程不再本文章的论述中。本文章仅提供驱动源代码。

### 本驱动接口列表：
1.  驱动_打开进程: OpenProcess
2.  驱动_读取进程内存: ReadProcessMemory
3.  驱动_写入进程内存: WriteProcessMemory
4.  驱动_关闭进程: CloseHandle
5.  驱动_获取进程内存块列表: VirtualQueryExFull（可选：显示全部内存、仅显示物理内存）
6.  驱动_获取进程PID列表: GetPidList
7.  驱动_提升进程权限到Root: SetProcessRoot
8.  驱动_获取进程占用物理内存大小: GetProcessPhyMemSize
9.  驱动_获取进程命令行: GetProcessCmdline
10.  驱动_隐藏驱动: HideKernelModule

# 驱动2名称: Linux ARM64内核硬件断点进程调试驱动3
### 本驱动接口列表：
1.  驱动_打开进程: OpenProcess
2.  驱动_关闭进程: CloseHandle
3.  驱动_获取CPU支持硬件执行断点的数量: GetNumBRPS
4.  驱动_获取CPU支持硬件访问断点的数量: GetNumWRPS
5.  驱动_设置进程硬件断点: AddProcessHwBp
6.  驱动_删除进程硬件断点: DelProcessHwBp
7.  驱动_暂停硬件断点: SuspendProcessHwBp
8.  驱动_恢复硬件断点: ResumeProcessHwBp
9.  驱动_读取硬件断点命中信息: ReadHwBpInfo
10. 驱动_设置无条件Hook跳转: SetHookPC

## 目录说明：
> **rwProcMem33Module**：进程内存读写驱动
>>* **rwProcMem_module**：（*内核层*）驱动源码
>>* **testKo**：（*应用层*）调用驱动demo
>>* **testTarget**：（*应用层*）读取第三方进程demo
>>* **testMemSearch**：（*应用层*）搜索第三方进程内存demo
>>* **testDumpMem**：（*应用层*）保存第三方进程内存demo
>>* **testCEServer**：（*应用层*）CheatEngine远程服务器，可配合CheatEngine7.1远程连接使用

> **hwBreakpointProcModule**：硬件断点进程调试驱动
>>* **hwBreakpointProc_module**：（*内核层*）驱动源码
>>* **testHwBp**：（*应用层*）调用驱动demo
>>* **testHwBpTarget**：（*应用层*）硬件断点第三方进程demo
>>* **testHwBpClient**：（*应用层*）硬件断点工具之远程客户端
>>* **testHwBpServer**：（*应用层*）硬件断点工具之远程服务端

## 更新日志：
2025-7（读写驱动）：
  * **1.去除无用接口**
  * **2.修复驱动进程列表获取缺陷**
  * **3.修复驱动maps列表获取的一些bug**
  * **4.修复android15兼容性问题**
  * **5.新增一种驱动隐蔽通信手段**
  
2025-5（读写驱动）：
  * **1.支持Linux6.6**

2024-10（硬件断点驱动）：
  * **1.支持Linux5.10、6.1**
  * **2.修复硬件断点会卡死的bug**
  * **3.修复搜索Kit的一些bug**

---

## 来源与合并说明

本仓库是 GG-MCP 项目使用的内核驱动源码基线，由三处来源合并而成，**以本仓库为准**：

| 来源 | 内容 | 在本仓库的位置 |
|---|---|---|
| [abcz316/rwProcMem33](https://github.com/abcz316/rwProcMem33) | 上游原始驱动（第三方开源，遵循上游 LICENSE / 声明） | `rwProcMem33Module/`、`hwBreakpointProcModule/` 的基底 |
| AFEI port（原仓库 `rwProcMem33-afei`） | CFI bypass、proc 节点残留修复、KO 瘦身、`docs-afei/` 分析笔记 | 已在基线中，见 `AFEI_CHANGES.md` |
| `gg-rwproc33-driver`（GitHub，私有备份仓） | MODVERSIONS CRC 流水线、6.12.69 权威 CRC 表、真机适配记录 | `tools/`、`assets/` |

### 为什么不直接合并两者的源码

两边源码各有取舍，**本仓库的版本更完整**，不要用另一份覆盖：

| | 本仓库 | `gg-rwproc33-driver` |
|---|---|---|
| 内存模块 | 6452 行，15 条命令，含 `anon_channel.h` / `cfi_bypass.h` / `phy_mem_fast.h` / `rw_symbol_lookup.h` | 5206 行，缺上述四个头 |
| hwbp 命令枚举 | 1 起始（已修） | 1 起始（已修） |
| hwbp 命中回调 | `raw_spinlock` + 预分配缓冲 | 仍见 mutex / cvector 痕迹 |
| **执行断点命中后的单步挪位** | **已实现**（直接写 BVR 的两段式：pc → pc+4 → 原地址） | **没有** —— 会 PC 原地反复触发、进程卡死 R 状态 |

最后一条是真机实测出来的：没有单步挪位时，执行断点命中后 perf 会把断点写回硬件，
同一条指令被无限重复触发。**不要把 hwbp 源码换成另一份**，否则这个修复会丢。

### 资产与工具

- `assets/kcrctab_6.12.69.json` —— 设备 6.12.69 的**权威符号 CRC 表**（9884 个导出符号）。
  取自真机内核 `__kcrctab`，而不是自编内核推算的值。
- `assets/device_config.txt` —— 设备真值 `.config`（配置内核树用）。
- `tools/` —— MODVERSIONS CRC 流水线（纯标准库，离线可用）：
  - `extract_kernel_crc.py`：从内核 Image 定位 `__ksymtab` 投票反解 `__kcrctab`，导出 CRC 全表
  - `patch_ko_crc.py`：用 CRC 表**双写** `.ko` 的 `__versions` + `__version_ext_crcs`
  - `crosscheck_ko_crc.py`：批量交叉校验（判据：冲突数 = 0）

### 两条必须记住的 MODVERSIONS 事实

1. `CONFIG_EXTENDED_MODVERSIONS=y` 时内核**只读 `__version_ext_crcs`**，
   `__versions` 仅为兼容保留 —— **只改 `__versions` 是静默无效的，必须双写**。
2. 模块带 `__versions` 时走 `same_magic(has_crcs=true)`：
   **跳过首个空格前的内核版本，只比对空格后的标志串**。
   所以 vermagic 写成 `6.12.69-android16-6-4k` 就能通过，即使设备 `uname -r`
   带 git hash 与 ABO 编号。

### 未合并的部分（有意为之）

`gg-rwproc33-driver` 的 `kernel-assets/` 共 129MB，其中 77MB 内核镜像、
23MB kallsyms、21MB 设备 .ko **没有复制进来** —— 这些都能从设备重新导出，
不值得占仓库体积。需要时按 `tools/README.md` 的流程重新提取。
