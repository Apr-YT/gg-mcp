#ifndef VER_CONTROL_H_
#define VER_CONTROL_H_
#include <linux/version.h>

// 独立内核模块入口模式
#define CONFIG_MODULE_GUIDE_ENTRY

// 生成proc用户层交互节点文件
#define CONFIG_USE_PROC_FILE_NODE
// 隐蔽通信密钥
#define CONFIG_PROC_NODE_AUTH_KEY "dce3771681d4c7a143d5d06b7d32548e"

// 调试打印模式
//#define CONFIG_DEBUG_PRINTK

// 动态寻址模式
#define CONFIG_KALLSYMS_LOOKUP_NAME

// 精准命中记录模式
// ★ 关闭：开启后命中回调里会 modify_user_hw_breakpoint（进 perf_event ctx->mutex），
//   而回调本身运行在调试异常上下文；W/R 监视点不需要它，X 断点才用得上。
//#define CONFIG_MODIFY_HIT_NEXT_MODE

// 反PTRACE侦测模式
// ★ 关闭：它要接管 g_hwbp_handle_info_mutex/_arr（已改为静态数组 + raw_spinlock），
//   且内部 kprobe 路径在本内核上有崩溃风险（见 RWPROC33_ADAPTER.md 10.4）。
//#define CONFIG_ANTI_PTRACE_DETECTION_MODE

/* ============================================================================
 * 【AFEI 移植】运行时 CFI 绕过开关
 *
 * 与 rwProcMem 模块同源（cfi_bypass.h / rw_symbol_lookup.h 两份拷贝）。
 * hwBreakpointProc 大量使用 perf_event 函数指针（register_user_hw_breakpoint
 * 的回调、overflow handler），在 CONFIG_CFI_CLANG=y 的内核上，一旦某处
 * 函数原型与 CFI 记录的类型不匹配，就会走 __cfi_check_fail →
 * __ubsan_handle_cfi_check_fail_abort → panic。
 *
 * ★默认开启。关闭时不影响其它功能。
 * ==========================================================================*/
#define CONFIG_ENABLE_CFI_BYPASS

/* ── 【对标 AFEI】默认隐身 ────────────────────────────────────────────────
 * 与 rwProcMem 同样的改动：AFEI 的 hide_module() 在 init_module 里默认调用，
 * 使 lsmod / /proc/modules / /sys/module 三处从一开始就看不到驱动。
 * 我方原先只在用户态主动发 CMD_HIDE_KERNEL_MODULE 时才隐藏。
 * 本开关（默认开）让 init 末尾自动执行同样的隐藏。调试期可注释掉。 */
/* ── 匿名 inode 通道：代码已移植完，但**暂不启用**（2026-10-23）────────────
 * 启用后现象：agentd 能拿到 fd（anon channel ready: fd=4），但**走匿名通道的
 * GET_NUM_BRPS(=3) 返回 -1**，而同一节点走 proc read 返回 6。
 * 已排除：getCpuNumBrps() 是纯位运算 ((read_cpuid(ID_AA64DFR0_EL1)>>12)&0xf)+1，
 *         不可能返回 -1 ⇒ 请求在 anon 链路上就没走对 target case。
 *         二分已验证：把本开关关掉重编，hwbp 立刻恢复（bps=16, threads=16）。
 * 未排除：AnonDispatchThunk 的间接调用 / cfi_bypass 的 text patch 相互作用
 *         （内存模块同一套代码是正常的，差异待查）。
 * 关掉的效果：bp 的 /proc fd 仍常驻 agentd 的 fd 表（fd 5），是当前已知暴露面。
 * 重新启用前先看 AGENTS 第 32 条。 */
#define CONFIG_USE_ANON_CHANNEL

#define CONFIG_HIDE_BY_DEFAULT

#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))
#endif
#ifndef MY_LINUX_VERSION_CODE 
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(3,10,0)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(3,10,84)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(3,18,71)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(3,18,140)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,4,21)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,4,78)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,4,153)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,4,192)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,9,112)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,9,186)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,14,83)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,14,117)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,14,141)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,19,81)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,19,113)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(5,4,61)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(5,10,43)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(5,15,41)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(6,1,75)
// 目标设备：25102RKBEC / myron / Android 16，uname -r = 6.12.23-android16-5-...
//   源码里版本分支最高阈值 6.6.30，6.12.23 走 >= 分支，与 6.6.30 代码路径相同。
#define MY_LINUX_VERSION_CODE KERNEL_VERSION(6,12,23)
#endif

#ifdef CONFIG_DEBUG_PRINTK
#define printk_debug printk
#else
static inline void printk_debug(char *fmt, ...) {}
#endif

#endif /* VER_CONTROL_H_ */
