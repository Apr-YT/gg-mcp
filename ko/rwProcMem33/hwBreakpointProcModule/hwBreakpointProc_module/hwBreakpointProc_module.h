#ifndef _HWBP_PROC_H_
#define _HWBP_PROC_H_
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>   /* for_each_thread */
#include <linux/vmalloc.h>
#include <linux/pid.h>
#include <linux/init.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/compat.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/kallsyms.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/ksm.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/pid.h>
#include <linux/slab.h> //kmalloc与kfree
#include "ver_control.h"
#include "arm64_register_helper.h"
#include "cvector.h"
/* ── 【AFEI 移植】运行时 CFI 绕过 ──────────────────────────────────────────
 * 顺序有依赖：rw_symbol_lookup 被 cfi_bypass 依赖。
 * ────────────────────────────────────────────────────────────────────────*/
#include "rw_symbol_lookup.h"
#include "cfi_bypass.h"
#ifdef CONFIG_USE_PROC_FILE_NODE
#include <linux/proc_fs.h>
#include "hide_procfs_dir.h"
#endif
//////////////////////////////////////////////////////////////////

/* 命令码必须与 agentd/driver.h 的 RWBP_CMD_* 一一对应。
 *
 * ★ 上游这里首项没有赋值（从 0 起），而 rwProcMem 模块首项是
 *   `CMD_INIT_DEVICE_INFO = 1` —— 两个模块基准不同。afei 的 prebuilt 按 0 起编译，
 *   agentd 按 1 起始发送，于是 cmd=1（以为 OPEN_PROCESS）实际执行
 *   CMD_CLOSE_PROCESS → put_pid((struct pid*)1) 解引用野指针 → 内核崩。
 *   这里显式从 1 起始，消除该偏移。
 *
 * ★ UNHIDE 从 12 挪到 14，把 13 留给 INST_PROCESS_HWBP_ALL —— 否则它会与
 *   agentd 的批量注册命令撞号。
 */
enum {
	CMD_OPEN_PROCESS = 1, 			// 打开进程
	CMD_CLOSE_PROCESS, 				// 关闭进程
	CMD_GET_NUM_BRPS, 				// 获取CPU硬件执行断点支持数量
	CMD_GET_NUM_WRPS, 				// 获取CPU硬件访问断点支持数量
	CMD_INST_PROCESS_HWBP,			// 安装进程硬件断点
	CMD_UNINST_PROCESS_HWBP,		// 卸载进程硬件断点
	CMD_SUSPEND_PROCESS_HWBP,		// 暂停进程硬件断点
	CMD_RESUME_PROCESS_HWBP,		// 恢复进程硬件断点
	CMD_GET_HWBP_HIT_COUNT,			// 获取硬件断点命中地址数量
	CMD_GET_HWBP_HIT_DETAIL,		// 获取硬件断点命中详细信息
	CMD_SET_HOOK_PC,				// 设置无条件Hook跳转
	CMD_HIDE_KERNEL_MODULE,			// 隐藏驱动
	CMD_INST_PROCESS_HWBP_ALL,		// 13: 进程内全部线程批量注册（agentd 用）
	CMD_UNHIDE_KERNEL_MODULE,		// 14: 【AFEI·逃生舱】恢复模块可见性
	CMD_ANON_GET_FD,				// 15: 在当前进程装一个匿名通道 fd（返回 fd 号）
};

struct hwBreakpointProcDev {
#ifdef CONFIG_USE_PROC_FILE_NODE
	struct proc_dir_entry *proc_parent;
	struct proc_dir_entry *proc_entry;
#endif
	bool is_hidden_module; //是否已经隐藏过驱动列表了
};
static struct hwBreakpointProcDev *g_hwBreakpointProc_devp;

static ssize_t hwBreakpointProc_read(struct file* filp, char __user* buf, size_t size, loff_t* ppos);
static int hwBreakpointProc_release(struct inode *inode, struct file *filp);
static const struct proc_ops hwBreakpointProc_proc_ops = {
    .proc_read    = hwBreakpointProc_read,
	.proc_release = hwBreakpointProc_release,
};

#pragma pack(1)
struct my_user_pt_regs {
	uint64_t regs[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t pstate;
	uint64_t orig_x0;
	uint64_t syscallno;
};
struct HWBP_HIT_ITEM {
	uint64_t task_id;
	uint64_t hit_addr;
	uint64_t hit_time;
	struct my_user_pt_regs regs_info;
};
#pragma pack()

struct HWBP_HANDLE_INFO {
	uint64_t task_id;
	struct perf_event * sample_hbp;
	struct perf_event_attr original_attr;
	bool is_32bit_task;
	/* 断点挪位状态：bp_addr 为 0 表示断点就在原地址；非 0 表示当前被
	 * 临时挪到该地址（执行断点要跨过自身那条指令）。挪位改为直接写
	 * BVR，不再依赖 CONFIG_MODIFY_HIT_NEXT_MODE（那条路走 perf API，
	 * 在调试异常上下文里会拿 mutex 睡眠）。*/
	struct perf_event_attr next_instruction_attr;
	size_t hit_total_count;
	/* 命中缓冲：INST 时一次性 vmalloc MIN_LEN 条（进程上下文，可睡眠）；
	 * 命中回调只往 hit_arr[hit_count++] 里 memcpy，不做任何分配。
	 * 原来用 cvector，pushback 内部 vmalloc —— 在 perf_event 溢出回调
	 * （中断上下文）里会 scheduling while atomic。 */
	struct HWBP_HIT_ITEM *hit_arr;
	u32 hit_count;
	/* 暂停标志：SUSPEND 置 1 后回调只跳过记录，不碰硬件属性
	 * （原来靠 modify_user_hw_breakpoint，要进 perf_event 的 mutex）。 */
	int suspended;
};

#endif /* _HWBP_PROC_H_ */