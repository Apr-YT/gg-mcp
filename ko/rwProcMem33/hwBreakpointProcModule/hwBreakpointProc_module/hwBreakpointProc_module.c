#include "hwBreakpointProc_module.h"
#include "proc_pid.h"
#include "api_proxy.h"
#include "anon_channel.h"
#include "anti_ptrace_detection.h"
#include <linux/sched/signal.h>   /* next_thread / PIDTYPE_* */


#pragma pack(push,1)
struct ioctl_request {
    char     cmd;        /* 1 字节命令 */
    uint64_t param1;     /* 参数1 */
    uint64_t param2;     /* 参数2 */
    uint64_t param3;     /* 参数3 */
    uint64_t buf_size;    /* 紧随其后的动态数据长度 */
};
#pragma pack(pop)
//////////////////////////////////////////////////////////////////

static atomic64_t g_hook_pc;


/* 句柄表改固定数组 + raw_spinlock：
 * 原实现是 cvector + struct mutex。cvector_pushback 内部 vmalloc，而命中回调
 * hwbp_handler 运行在 perf_event 溢出上下文（ARM64 调试异常，中断/抢占关闭），
 * 在那里 mutex_lock + vmalloc 会 scheduling while atomic → 整机挂死。
 * 现在：句柄表固定 16 槽（INST 写入、UNINST 用 swap-remove，全程 spinlock）；
 * 命中缓冲在 INST（进程上下文）一次性 vmalloc，回调只做 memcpy。 */
#define HWBP_MAX_HANDLES 16
#define HWBP_HIT_CAP     1024   /* 每个断点最多缓存多少条命中（= 上游 MIN_LEN） */

static DEFINE_RAW_SPINLOCK(g_hwbp_lock);
static struct HWBP_HANDLE_INFO g_hwbp_handles[HWBP_MAX_HANDLES];
static int g_hwbp_nhandle;

static void record_hit_details(struct HWBP_HANDLE_INFO *info, struct pt_regs *regs) {
    struct HWBP_HIT_ITEM hit_item = {0};
	if (!info || !regs) {
        return;
    }
	hit_item.task_id = info->task_id;
    hit_item.hit_addr = regs->pc;
	hit_item.hit_time = ktime_get_real_seconds();
    memcpy(&hit_item.regs_info.regs, regs->regs, sizeof(hit_item.regs_info.regs));
    hit_item.regs_info.sp = regs->sp;
    hit_item.regs_info.pc = regs->pc;
    hit_item.regs_info.pstate = regs->pstate;
    hit_item.regs_info.orig_x0 = regs->orig_x0;
    hit_item.regs_info.syscallno = regs->syscallno;
    /* 只做一次 memcpy —— 调用方已持 raw_spinlock，这里不能分配、不能睡眠 */
    if (info->hit_arr && info->hit_count < HWBP_HIT_CAP)
        info->hit_arr[info->hit_count++] = hit_item;
}

#ifdef CONFIG_MODIFY_HIT_NEXT_MODE
static bool arm64_move_bp_to_next_instruction(struct perf_event *bp, uint64_t next_instruction_addr, struct perf_event_attr *original_attr, struct perf_event_attr * next_instruction_attr) {
    int result;
	if (!bp || !original_attr || !next_instruction_attr || !next_instruction_addr) {
        return false;
    }
	memcpy(next_instruction_attr, original_attr, sizeof(struct perf_event_attr));
	next_instruction_attr->bp_addr = next_instruction_addr;
	next_instruction_attr->bp_len = HW_BREAKPOINT_LEN_4;
	next_instruction_attr->bp_type = HW_BREAKPOINT_X;
	next_instruction_attr->disabled = 0;
	result = x_modify_user_hw_breakpoint(bp, next_instruction_attr);
	if(result) {
		next_instruction_attr->bp_addr = 0;
		return false;
	}
	return true;
}

static bool arm64_recovery_bp_to_original(struct perf_event *bp, struct perf_event_attr *original_attr, struct perf_event_attr * next_instruction_attr) {
    int result;
	if (!bp || !original_attr || !next_instruction_attr) {
        return false;
    }
	result = x_modify_user_hw_breakpoint(bp, original_attr);
	if(result) {
		return false;
	}
	next_instruction_attr->bp_addr = 0;
	return true;
}
#endif

static void hwbp_hit_user_info_callback(struct perf_event *bp,
	struct perf_sample_data *data,
	struct pt_regs *regs, struct HWBP_HANDLE_INFO * hwbp_handle_info) {
	hwbp_handle_info->hit_total_count++;
	record_hit_details(hwbp_handle_info, regs);
}

/*
 * Handle hitting a HW-breakpoint.
 */
/* 在回调里改断点地址：只读写调试寄存器，绝不调用 perf 框架 API。
 * perf 的 modify_user_hw_breakpoint 会拿 mutex，而本函数运行在
 * ARM64 调试异常上下文（中断已关），走 perf API 必然 scheduling while atomic。
 * 返回 false 表示没找到槽位（地址已被改走）。 */
static bool set_bp_addr_directly(const struct perf_event_attr *attr, bool is_32bit_task,
                                 uint64_t cur_addr, uint64_t new_addr) {
	int i, max_slots, val_reg, slot = -1;

	if (!attr)
		return false;
	switch (attr->bp_type) {
	case HW_BREAKPOINT_R:
	case HW_BREAKPOINT_W:
	case HW_BREAKPOINT_RW:
		val_reg = AARCH64_DBG_REG_WVR;
		max_slots = getCpuNumWrps();
		break;
	case HW_BREAKPOINT_X:
		val_reg = AARCH64_DBG_REG_BVR;
		max_slots = getCpuNumBrps();
		break;
	default:
		return false;
	}
	(void)is_32bit_task;

	for (i = 0; i < max_slots; i++) {
		if (read_wb_reg(val_reg, i) == cur_addr) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return false;

	write_wb_reg(val_reg, slot, new_addr);
	return true;
}

static void hwbp_handler(struct perf_event *bp,
	struct perf_sample_data *data,
	struct pt_regs *regs) {
	int i;
	unsigned long flags;
	uint64_t hook_pc;
	printk_debug(KERN_INFO "hw_breakpoint HIT!!!!! bp:%px, pc:%px, id:%d\n", bp, regs->pc, bp->id);

	hook_pc = atomic64_read(&g_hook_pc);
	if(hook_pc) {
		regs->pc = hook_pc;
		return;
	}

	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < g_hwbp_nhandle; i++) {
		struct HWBP_HANDLE_INFO * hwbp_handle_info = &g_hwbp_handles[i];
		if (hwbp_handle_info->sample_hbp != bp)
			continue;
		if (hwbp_handle_info->suspended)   /* 已暂停：只跳过记录 */
			break;
		if (hwbp_handle_info->original_attr.bp_type == HW_BREAKPOINT_X) {
			/* 执行断点：命中时 PC 仍停在断点指令上，原指令还没执行。
			 * perf 认为 event 仍然有效，会把断点写回硬件，于是同一条指令
			 * 被反复触发 —— 必须把断点临时挪到 pc+4，等原指令执行完、
			 * 在 pc+4 再次命中时再挪回来。全程只读写调试寄存器。 */
			if (hwbp_handle_info->next_instruction_attr.bp_addr == 0) {
				/* 首次命中：先记录，再挪到 pc+4 */
				hwbp_hit_user_info_callback(bp, data, regs, hwbp_handle_info);
				if (set_bp_addr_directly(&hwbp_handle_info->original_attr,
				                         hwbp_handle_info->is_32bit_task,
				                         regs->pc, regs->pc + 4)) {
					hwbp_handle_info->next_instruction_attr.bp_addr = regs->pc + 4;
				} else {
					/* 挪不动就禁用断点：宁可少记录，也不能把进程锁死 */
					toggle_bp_registers_directly(&hwbp_handle_info->original_attr,
					                             hwbp_handle_info->is_32bit_task, 0);
				}
			} else {
				/* 第二次命中（pc+4）：挪回原地址，这一次不记录 */
				if (set_bp_addr_directly(&hwbp_handle_info->original_attr,
				                         hwbp_handle_info->is_32bit_task,
				                         hwbp_handle_info->next_instruction_attr.bp_addr,
				                         hwbp_handle_info->original_attr.bp_addr)) {
					hwbp_handle_info->next_instruction_attr.bp_addr = 0;
				}
			}
		} else {
			/* 数据断点：访问已经发生，PC 会照常前进，只需记录 */
			hwbp_hit_user_info_callback(bp, data, regs, hwbp_handle_info);
		}
		break;
	}
	
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
}



static ssize_t OnCmdOpenProcess(struct ioctl_request *hdr, char __user* buf) {
	uint64_t pid = hdr->param1, handle = 0;
	struct pid * proc_pid_struct = NULL;
	printk_debug(KERN_INFO "CMD_OPEN_PROCESS\n");

	printk_debug(KERN_INFO "pid:%llu,size:%ld\n", pid, sizeof(pid));

	proc_pid_struct = get_proc_pid_struct(pid);
	printk_debug(KERN_INFO "proc_pid_struct *:0x%p\n", (void*)proc_pid_struct);
	if (!proc_pid_struct) {
		return -EINVAL;
	}
	handle = (uint64_t)proc_pid_struct;

	printk_debug(KERN_INFO "handle:%llu,size:%ld\n", handle, sizeof(handle));
	if (!!x_copy_to_user((void*)buf, (void*)&handle, sizeof(handle))) {
		return -EINVAL;
	}
	return 0;
}

static ssize_t OnCmdCloseProcess(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	printk_debug(KERN_INFO "CMD_CLOSE_PROCESS\n");
	printk_debug(KERN_INFO "proc_pid_struct*:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	release_proc_pid_struct(proc_pid_struct);
	return 0;
}

static ssize_t OnCmdGetCpuNumBrps(struct ioctl_request *hdr, char __user* buf) {
	printk_debug(KERN_INFO "CMD_GET_NUM_BRPS\n");
	return getCpuNumBrps();
}

static ssize_t OnCmdGetCpuNumWrps(struct ioctl_request *hdr, char __user* buf) {
	printk_debug(KERN_INFO "CMD_GET_NUM_WRPS\n");
	return getCpuNumWrps();
}

static ssize_t OnCmdInstProcessHwbp(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	uint64_t proc_virt_addr = hdr->param2;
	char hwbp_len  =  hdr->param3 & 0xFF;
	char hwbp_type = (hdr->param3 >> 8) & 0xFF;

	pid_t pid_val;
	struct task_struct *task;
	struct HWBP_HANDLE_INFO hwbp_handle_info = { 0 };
	unsigned long flags;
	printk_debug(KERN_INFO "CMD_INST_PROCESS_HWBP\n");
	printk_debug(KERN_INFO "proc_pid_struct *:%px\n", proc_pid_struct);
	printk_debug(KERN_INFO "proc_virt_addr :%px\n", proc_virt_addr);
	printk_debug(KERN_INFO "hwbp_len:%zu\n", hwbp_len);
	printk_debug(KERN_INFO "hwbp_type:%d\n", hwbp_type);

	pid_val = pid_nr(proc_pid_struct);
	printk_debug(KERN_INFO "pid_val:%d\n", pid_val);

	if (!pid_val) {
		printk_debug(KERN_INFO "pid_nr failed.\n");
		return -EINVAL;
	}

	task = pid_task(proc_pid_struct, PIDTYPE_PID);
	if (!task) {
		printk_debug(KERN_INFO "get_pid_task failed.\n");
		return -EINVAL;
	}
	
	hwbp_handle_info.task_id = pid_val;
	hwbp_handle_info.is_32bit_task = is_compat_thread(task_thread_info(task));
	ptrace_breakpoint_init(&hwbp_handle_info.original_attr);
	hwbp_handle_info.original_attr.bp_addr = proc_virt_addr;
	hwbp_handle_info.original_attr.bp_len = hwbp_len;
	hwbp_handle_info.original_attr.bp_type = hwbp_type;
	hwbp_handle_info.original_attr.disabled = 0;

	hwbp_handle_info.sample_hbp = x_register_user_hw_breakpoint(&hwbp_handle_info.original_attr, hwbp_handler, NULL, task);
	printk_debug(KERN_INFO "register_user_hw_breakpoint return: %px\n", hwbp_handle_info.sample_hbp);
	if (IS_ERR((void __force *)hwbp_handle_info.sample_hbp)) {
		int ret = PTR_ERR((void __force *)hwbp_handle_info.sample_hbp);
		printk_debug(KERN_INFO "register_user_hw_breakpoint failed: %d\n", ret);
		return ret;
	}
	/* 命中缓冲在进程上下文预分配（可睡眠），回调里只用不申请 */
	hwbp_handle_info.hit_arr = vmalloc(HWBP_HIT_CAP * sizeof(struct HWBP_HIT_ITEM));
	if (!hwbp_handle_info.hit_arr) {
		x_unregister_hw_breakpoint(hwbp_handle_info.sample_hbp);
		return -ENOMEM;
	}
	hwbp_handle_info.hit_count = 0;

	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	if (g_hwbp_nhandle >= HWBP_MAX_HANDLES) {
		raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
		vfree(hwbp_handle_info.hit_arr);
		x_unregister_hw_breakpoint(hwbp_handle_info.sample_hbp);
		return -ENOSPC;
	}
	g_hwbp_handles[g_hwbp_nhandle++] = hwbp_handle_info;
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);

	if (x_copy_to_user((void*)buf, &hwbp_handle_info.sample_hbp, sizeof(uint64_t))) {
		return -EINVAL;
	}
	return 0;
}

/* 对进程全部线程批量安装硬件断点（CMD_INST_PROCESS_HWBP_ALL = 13）。
 *
 * ★ 两阶段注册：RCU 读锁内只 get_task_struct 收集引用，注册放到 RCU 外。
 *   register_user_hw_breakpoint / vmalloc 都会睡眠，放进 RCU 临界区必然
 *   scheduling while atomic → PANIC_ON_OOPS，整机重启。
 *
 * 入参：param1 = 内核 pid 句柄、param2 = addr、param3 = (len | type << 8)
 * 出参：buf[0..4) = 注册数 N，buf[4..4+8N) = perf_event* 数组。成功返回 0。
 *
 * 槽位管理沿用本文件的 g_hwbp_nhandle 模型（与单线程 INST 同一契约），
 * 单个线程失败只跳过、不影响其余线程。 */
static ssize_t OnCmdInstAllProcessHwbp(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	uint64_t proc_virt_addr = hdr->param2;
	char hwbp_len  = hdr->param3 & 0xFF;
	char hwbp_type = (hdr->param3 >> 8) & 0xFF;
	pid_t pid_val;
	struct task_struct *leader, *task;
	struct task_struct **tasks = NULL;
	uint64_t *bp_ids = NULL;
	unsigned long flags;
	int n = 0, ntasks = 0, i;

	if (!proc_pid_struct)
		return -EINVAL;
	pid_val = pid_nr(proc_pid_struct);
	if (!pid_val)
		return -EINVAL;
	/* OPEN_PROCESS 的 pid 是 find_get_pid() 按 PIDTYPE_PID 关联的，
	 * 用 PIDTYPE_TGID 在这条链上查不到；先取 task 再走 group_leader。*/
	task = pid_task(proc_pid_struct, PIDTYPE_PID);
	if (!task) {
		printk_debug(KERN_INFO "CMD_INST_ALL pid %d task not found\n", pid_val);
		return -EINVAL;
	}
	leader = task->group_leader;
	if (!leader) {
		printk_debug(KERN_INFO "CMD_INST_ALL pid %d leader not found\n", pid_val);
		return -EINVAL;
	}

	tasks = (struct task_struct **)vmalloc(sizeof(*tasks) * HWBP_MAX_HANDLES);
	if (!tasks)
		return -ENOMEM;
	bp_ids = (uint64_t *)vmalloc(sizeof(uint64_t) * HWBP_MAX_HANDLES);
	if (!bp_ids) {
		vfree(tasks);
		return -ENOMEM;
	}

	/* 阶段一：RCU 内只收集引用，不睡眠。
	 *
	 * ⚠ 这里不能用 for_each_thread：它展开成 `for (t = p; t != NULL; t = next_thread(t))`，
	 *   而 next_thread 取的是 thread_group.next —— thread_group 是循环链表，
	 *   绕一圈回到 leader，永远不会变成 NULL。只靠数量上限兜底的后果是
	 *   同一个 task 被重复收集、重复注册断点，真机会直接 panic。
	 *   改用 do-while，显式判断「绕回 leader」即结束。*/
	rcu_read_lock();
	task = leader;
	do {
		if (ntasks >= HWBP_MAX_HANDLES)
			break;
		get_task_struct(task);
		tasks[ntasks++] = task;
		task = next_thread(task);
	} while (task != leader);
	rcu_read_unlock();

	printk_debug(KERN_INFO "CMD_INST_ALL pid %d: collected %d threads\n",
		pid_val, ntasks);

	/* 阶段二：RCU 外逐个注册（此处允许睡眠） */
	for (i = 0; i < ntasks; i++) {
		struct HWBP_HANDLE_INFO info = { 0 };
		struct perf_event *bp;

		info.task_id = task_pid_nr(tasks[i]);
		info.is_32bit_task = is_compat_thread(task_thread_info(tasks[i]));
		ptrace_breakpoint_init(&info.original_attr);
		info.original_attr.bp_addr = proc_virt_addr;
		info.original_attr.bp_len = hwbp_len;
		info.original_attr.bp_type = hwbp_type;
		info.original_attr.disabled = 0;

		bp = x_register_user_hw_breakpoint(&info.original_attr, hwbp_handler, NULL, tasks[i]);
		if (IS_ERR((void __force *)bp)) {
			printk_debug(KERN_INFO "CMD_INST_ALL tid %d register failed: %d\n",
				task_pid_nr(tasks[i]), (int)PTR_ERR((void __force *)bp));
			continue;	/* 单线程失败不影响其余 */
		}
		info.sample_hbp = bp;

		/* 命中缓冲在进程上下文预分配，回调里只 memcpy */
		info.hit_arr = vmalloc(HWBP_HIT_CAP * sizeof(struct HWBP_HIT_ITEM));
		if (!info.hit_arr) {
			x_unregister_hw_breakpoint(bp);
			continue;
		}
		info.hit_count = 0;

		raw_spin_lock_irqsave(&g_hwbp_lock, flags);
		if (g_hwbp_nhandle >= HWBP_MAX_HANDLES) {
			raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
			vfree(info.hit_arr);
			x_unregister_hw_breakpoint(bp);
			continue;
		}
		g_hwbp_handles[g_hwbp_nhandle++] = info;
		raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);

		bp_ids[n++] = (uint64_t)bp;
	}

	for (i = 0; i < ntasks; i++)
		put_task_struct(tasks[i]);
	vfree(tasks);

	if (x_copy_to_user((void*)buf, &n, sizeof(n))) {
		vfree(bp_ids);
		return -EINVAL;
	}
	if (n > 0 && x_copy_to_user((void*)(buf + sizeof(int)), bp_ids,
	                            n * sizeof(uint64_t))) {
		vfree(bp_ids);
		return -EINVAL;
	}
	vfree(bp_ids);

	printk_debug(KERN_INFO "CMD_INST_ALL pid %d addr %px len %d type %d -> %d/%d threads\n",
		pid_val, (void*)proc_virt_addr, hwbp_len, hwbp_type, n, ntasks);
	return 0;
}

static ssize_t OnCmdUninstProcessHwbp(struct ioctl_request *hdr, char __user* buf) {
	struct perf_event * sample_hbp = (struct perf_event *)hdr->param1;
	int i;
	unsigned long flags;
	bool found = false;
	struct HWBP_HIT_ITEM *freed_arr = NULL;
	printk_debug(KERN_INFO "CMD_UNINST_PROCESS_HWBP\n");
	printk_debug(KERN_INFO "sample_hbp *:%px\n", sample_hbp);
	if(!sample_hbp) {
		return -EFAULT;
	}

	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < g_hwbp_nhandle; i++) {
		struct HWBP_HANDLE_INFO * hwbp_handle_info = &g_hwbp_handles[i];
		if (hwbp_handle_info->sample_hbp != sample_hbp)
			continue;
		freed_arr = hwbp_handle_info->hit_arr;              /* vfree 放到锁外 */
		g_hwbp_handles[i] = g_hwbp_handles[--g_hwbp_nhandle]; /* swap-remove */
		found = true;
		break;
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
	if (freed_arr)
		vfree(freed_arr);            /* 可能睡眠，必须在锁外 */
	if(found) {
		x_unregister_hw_breakpoint(sample_hbp);
	}
	
	return 0;
}

static ssize_t OnCmdSuspendProcessHwbp(struct ioctl_request *hdr, char __user* buf) {
	struct perf_event * sample_hbp = (struct perf_event *)hdr->param1;
	bool found = false;
	int i;
	unsigned long flags;
	printk_debug(KERN_INFO "CMD_SUSPEND_PROCESS_HWBP\n");
	if (!sample_hbp)
		return -EFAULT;

	/* 只改标志位：回调看到 suspended 就跳过记录。不去动 perf_event 属性 ——
	 * 那要进 perf_event 的 mutex，而本模块的锁已经是 raw_spinlock。 */
	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < g_hwbp_nhandle; i++) {
		if (g_hwbp_handles[i].sample_hbp != sample_hbp)
			continue;
		g_hwbp_handles[i].suspended = 1;
		found = true;
		break;
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);

	return found ? 0 : -EFAULT;
}
static ssize_t OnCmdResumeProcessHwbp(struct ioctl_request *hdr, char __user* buf) {
	struct perf_event * sample_hbp = (struct perf_event *)hdr->param1;
	bool found = false;
	int i;
	unsigned long flags;
	printk_debug(KERN_INFO "CMD_RESUME_PROCESS_HWBP\n");
	if (!sample_hbp)
		return -EFAULT;

	/* 只改标志位：回调看到 suspended 就跳过记录。不去动 perf_event 属性 ——
	 * 那要进 perf_event 的 mutex，而本模块的锁已经是 raw_spinlock。 */
	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < g_hwbp_nhandle; i++) {
		if (g_hwbp_handles[i].sample_hbp != sample_hbp)
			continue;
		g_hwbp_handles[i].suspended = 0;
		found = true;
		break;
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);

	return found ? 0 : -EFAULT;
}
static ssize_t OnCmdGetHwbpHitCount(struct ioctl_request *hdr, char __user* buf) {
	#pragma pack(1)
	struct buf_layout {
		uint64_t hit_total_count;
		uint64_t hit_item_arr_count;
	};
	#pragma pack()
	struct perf_event *sample_hbp = (struct perf_event *)hdr->param1;
	struct buf_layout user_data = {0};
	int i;
	unsigned long flags;
	printk_debug(KERN_INFO "CMD_GET_HWBP_HIT_COUNT\n");
	printk_debug(KERN_INFO "sample_hbp *:%px\n", sample_hbp);

	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < g_hwbp_nhandle; i++) {
		struct HWBP_HANDLE_INFO * hwbp_handle_info = &g_hwbp_handles[i];
		if (hwbp_handle_info->sample_hbp == sample_hbp && hwbp_handle_info->hit_arr) {
			user_data.hit_total_count = hwbp_handle_info->hit_total_count;
			user_data.hit_item_arr_count = hwbp_handle_info->hit_count;
			break;
		}
	}

	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
	
	printk_debug(KERN_INFO "user_data.hit_total_count:%zu\n", user_data.hit_total_count);
	if (x_copy_to_user((void*)buf, &user_data, sizeof(user_data))) {
		return -EINVAL;
	}
	return 0;
}

static ssize_t OnCmdGetHwbpHitDetail(struct ioctl_request *hdr, char __user* buf) {
	struct perf_event *sample_hbp = (struct perf_event *)hdr->param1;
	size_t size = hdr->buf_size;
	struct HWBP_HIT_ITEM *tmp;
	size_t taken = 0;
	unsigned long flags;
	int i;

	if (size < sizeof(struct HWBP_HIT_ITEM))
		return -EINVAL;

	/* 锁外分配临时缓冲：copy_to_user 可能缺页睡眠，raw_spinlock 内不能做；
	 * 锁内只把命中条目 memcpy 出来，锁外再交给用户态。 */
	tmp = vmalloc(size);
	if (!tmp)
		return -ENOMEM;

	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < g_hwbp_nhandle; i++) {
		struct HWBP_HANDLE_INFO * hwbp_handle_info = &g_hwbp_handles[i];
		if (hwbp_handle_info->sample_hbp != sample_hbp || !hwbp_handle_info->hit_arr)
			continue;
		taken = hwbp_handle_info->hit_count;
		if (taken > size / sizeof(struct HWBP_HIT_ITEM))
			taken = size / sizeof(struct HWBP_HIT_ITEM);
		memcpy(tmp, hwbp_handle_info->hit_arr, taken * sizeof(struct HWBP_HIT_ITEM));
		break;
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);

	if (taken &&
	    x_copy_to_user((void*)buf, tmp, taken * sizeof(struct HWBP_HIT_ITEM))) {
		vfree(tmp);
		return -EINVAL;
	}
	vfree(tmp);
	/* 返回**字节数**（read() 惯例）。原来返回条目数，而 agentd 用
	 * rsize / sizeof(item) 折算条目数 → 条数 < 312 时恒为 0，永远取不到命中。 */
	return (ssize_t)(taken * sizeof(struct HWBP_HIT_ITEM));
}

static ssize_t OnCmdSetHookPc(struct ioctl_request *hdr, char __user* buf) {
	uint64_t pc = hdr->param1;
	printk_debug(KERN_INFO "CMD_SET_HOOK_PC\n");
	printk_debug(KERN_INFO "pc:%px\n", pc);
	atomic64_set(&g_hook_pc, pc);
	return 0;
}

static ssize_t OnCmdHideKernelModule(struct ioctl_request *hdr, char __user* buf) {
	printk_debug(KERN_INFO "CMD_HIDE_KERNEL_MODULE\n");
	if (g_hwBreakpointProc_devp->is_hidden_module == false) {
		g_hwBreakpointProc_devp->is_hidden_module = true; 
		list_del_init(&__this_module.list);
		kobject_del(&THIS_MODULE->mkobj.kobj);
	}
	return 0;
}

/* ── 【对标 AFEI】init 阶段默认隐身（与 rwProcMem 同款改动）──────────────── */
#ifdef CONFIG_HIDE_BY_DEFAULT
static void hide_module_by_default(void) {
	if (g_hwBreakpointProc_devp->is_hidden_module == false) {
		g_hwBreakpointProc_devp->is_hidden_module = true;
		list_del_init(&__this_module.list);
		kobject_del(&THIS_MODULE->mkobj.kobj);
		printk_debug(KERN_INFO "[rwNova] module hidden by default (init)\n");
	}
}
#endif

/* ── 【新增·逃生舱】恢复驱动可见性（与 rwProcMem 同款修复）────────────────────
 * 背景：hide 用 list_del_init(&__this_module.list) 把模块从内核 module 链表
 * 摘掉，导致 rmmod <name> 永远失败、module_exit 永不执行，进而 /proc 节点
 * 永久残留 —— 这正是 "already registered" WARN 的根因。
 * 本函数做逆向操作：重新挂回模块链表 + sysfs，让 rmmod 重新可用。
 * `modules` 链表头未导出，用 rw_sym_lookup 从 kallsyms 取；
 * kobject_add 已 EXPORT_SYMBOL，直接调用。
 * ────────────────────────────────────────────────────────────────────────────*/
static int unhide_module(void) {
	unsigned long modules_addr;
	struct list_head *modules_list;
	int ret;

	if (g_hwBreakpointProc_devp->is_hidden_module == false)
		return 0;   /* 本来就没藏 */

	ret = kobject_add(&THIS_MODULE->mkobj.kobj,
	                  THIS_MODULE->mkobj.kobj.parent,
	                  "%s", THIS_MODULE->name);
	if (ret)
		printk_debug(KERN_INFO "[rwNova] unhide: kobject_add failed %d\n", ret);

	modules_addr = rw_sym_lookup("modules");
	if (modules_addr) {
		modules_list = (struct list_head *)modules_addr;
		list_add(&__this_module.list, modules_list);
		printk_debug(KERN_INFO "[rwNova] module unhidden (list restored)\n");
	} else {
		printk_debug(KERN_INFO "[rwNova] unhide: 'modules' symbol not found\n");
	}

	g_hwBreakpointProc_devp->is_hidden_module = false;
	return 0;
}

static ssize_t OnCmdUnhideKernelModule(struct ioctl_request *hdr, char __user* buf) {
	(void)hdr;
	(void)buf;
	return (ssize_t)unhide_module();
}

/* 前置声明：匿名通道桥接要调它，本体在文件更下方 */
static inline ssize_t DispatchCommand(struct ioctl_request *hdr, char __user* buf);

#ifdef CONFIG_USE_ANON_CHANNEL
/* 在当前进程上下文里装一个匿名通道 fd（与 rwProcMem_module.c 的同名逻辑一致）。
 * 必须在用户态主动调用的路径里执行 —— fd_install() 装的是发起本次系统调用的
 * 进程的 fd 表，放 init 里只会装进 insmod 进程。 */
static ssize_t OnCmdAnonGetFd(struct ioctl_request *hdr, char __user* buf) {
	int fd;
	int32_t out;

	(void)hdr;
	fd = anon_channel_install_fd();
	if (fd < 0)
		return fd;

	out = (int32_t)fd;
	if (x_copy_to_user((void *)buf, &out, sizeof(out)))
		return -EFAULT;
	return fd;
}

/* 匿名通道 -> 统一分发。布局与 /proc 通道的 read() 完全一致。 */
static long AnonDispatchThunk(struct anon_ioctl_request *req, char __user *buf) {
	BUILD_BUG_ON(sizeof(struct anon_ioctl_request) != sizeof(struct ioctl_request));
	return (long)DispatchCommand((struct ioctl_request *)req, buf);
}
#endif

static inline ssize_t DispatchCommand(struct ioctl_request *hdr, char __user* buf) {
	switch (hdr->cmd) {
	case CMD_OPEN_PROCESS:
		return OnCmdOpenProcess(hdr, buf);
	case CMD_CLOSE_PROCESS:
		return OnCmdCloseProcess(hdr, buf);
	case CMD_GET_NUM_BRPS:
		return OnCmdGetCpuNumBrps(hdr, buf);
	case CMD_GET_NUM_WRPS:
		return OnCmdGetCpuNumWrps(hdr, buf);
	case CMD_INST_PROCESS_HWBP:
		return OnCmdInstProcessHwbp(hdr, buf);
	case CMD_INST_PROCESS_HWBP_ALL:
		return OnCmdInstAllProcessHwbp(hdr, buf);
	case CMD_UNINST_PROCESS_HWBP:
		return OnCmdUninstProcessHwbp(hdr, buf);
	case CMD_SUSPEND_PROCESS_HWBP:
		return OnCmdSuspendProcessHwbp(hdr, buf);
	case CMD_RESUME_PROCESS_HWBP:
		return OnCmdResumeProcessHwbp(hdr, buf);
	case CMD_GET_HWBP_HIT_COUNT:
		return OnCmdGetHwbpHitCount(hdr, buf);
	case CMD_GET_HWBP_HIT_DETAIL:
		return OnCmdGetHwbpHitDetail(hdr, buf);
	case CMD_SET_HOOK_PC:
		return OnCmdSetHookPc(hdr, buf);
	case CMD_HIDE_KERNEL_MODULE:
		return OnCmdHideKernelModule(hdr, buf);
	case CMD_UNHIDE_KERNEL_MODULE:
		return OnCmdUnhideKernelModule(hdr, buf);
#ifdef CONFIG_USE_ANON_CHANNEL
	case CMD_ANON_GET_FD:
		return OnCmdAnonGetFd(hdr, buf);
#endif
	default:
		return -EINVAL;
	}
	return -EINVAL;
}

static ssize_t hwBreakpointProc_read(struct file* filp,
                              char __user* buf,
                              size_t size,
                              loff_t* ppos) {
    struct ioctl_request hdr = {0};
    size_t header_size = sizeof(hdr);

    if (size < header_size) {
        return -EINVAL;
    }

    if (x_copy_from_user(&hdr, buf, header_size)) {
        return -EFAULT;
    }

    if (size < header_size + hdr.buf_size) {
        return -EINVAL;
    }

    return DispatchCommand(&hdr, buf + header_size);
}

static void clean_hwbp(void) {
	struct perf_event *bps[HWBP_MAX_HANDLES];
	struct HWBP_HIT_ITEM *arrs[HWBP_MAX_HANDLES];
	int n = 0, i;
	unsigned long flags;

	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < g_hwbp_nhandle; i++) {
		bps[n] = g_hwbp_handles[i].sample_hbp;
		arrs[n] = g_hwbp_handles[i].hit_arr;
		n++;
	}
	g_hwbp_nhandle = 0;
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);

	/* unregister / vfree 都可能睡眠，一律在锁外 */
	for (i = 0; i < n; i++) {
		if (bps[i])
			x_unregister_hw_breakpoint(bps[i]);
		if (arrs[i])
			vfree(arrs[i]);
	}
}
static int hwBreakpointProc_release(struct inode *inode, struct file *filp) {
	/* 句柄表是静态数组，clean_hwbp 已经把 g_hwbp_nhandle 归零，无需重建 */
	clean_hwbp();
	return 0;
}

static int hwBreakpointProc_dev_init(void) {
	/* ── 【AFEI 移植 · 步骤0】运行时 CFI 绕过（必须最先）────────────────────
	 * 本模块大量使用 perf_event 回调等函数指针，CFI 检查失败会 panic。
	 * 必须在任何可能触发间接调用的操作之前完成 patch。
	 * 顺序：先清 kprobe 黑名单（这样后面 rw_sym_lookup 才能探到敏感符号），
	 *       再 cfi_bypass（其内部 rw_sym_lookup 依赖 kprobe 路径）。
	 * ────────────────────────────────────────────────────────────────────*/
#ifdef CONFIG_ENABLE_CFI_BYPASS
	disable_kprobe_blacklist();
	cfi_bypass();
#endif

#ifdef CONFIG_KALLSYMS_LOOKUP_NAME
	if(!init_kallsyms_lookup()) {
		printk_debug(KERN_EMERG "init_kallsyms_lookup failed\n");
		return -EBADF;
	}
#endif
	/* 句柄表是静态数组（g_hwbp_handles / g_hwbp_nhandle），锁由
	 * DEFINE_RAW_SPINLOCK 静态初始化，这里只需清零计数。 */
	g_hwbp_nhandle = 0;

	/* 反 ptrace 侦测依赖已不存在的 mutex + cvector，且其 kprobe 路径本身有
	 * 崩溃风险；ver_control.h 里已关闭 CONFIG_ANTI_PTRACE_DETECTION_MODE。 */

	g_hwBreakpointProc_devp = x_kmalloc(sizeof(struct hwBreakpointProcDev), GFP_KERNEL);
	memset(g_hwBreakpointProc_devp, 0, sizeof(struct hwBreakpointProcDev));

#ifdef CONFIG_USE_PROC_FILE_NODE
	/* ── 【根因修复·幂等注册】先清残留，再注册 ────────────────────────────
	 * 与 rwProcMem_module.c 同款修复。病灶：带 CONFIG_HIDE_BY_DEFAULT 的模块
	 * 加载后会被 list_del_init 摘出模块链表，令 rmmod 永久失败、module_exit
	 * 永不执行 ⇒ /proc/<KEY> 残留 ⇒ 下次 proc_mkdir 撞名 WARN 且注册失败。
	 * 修复：注册前无条件 remove_proc_subtree(KEY, NULL) 摘掉任何同名残留。
	 * ────────────────────────────────────────────────────────────────────*/
	remove_proc_subtree(CONFIG_PROC_NODE_AUTH_KEY, NULL);

	g_hwBreakpointProc_devp->proc_parent = proc_mkdir(CONFIG_PROC_NODE_AUTH_KEY, NULL);
	if (g_hwBreakpointProc_devp->proc_parent) {
		g_hwBreakpointProc_devp->proc_entry = proc_create(CONFIG_PROC_NODE_AUTH_KEY, S_IRUSR | S_IWUSR, g_hwBreakpointProc_devp->proc_parent, &hwBreakpointProc_proc_ops);
	} else {
		printk_debug(KERN_INFO "[rwNova] proc_mkdir failed, fallback to /proc root\n");
		g_hwBreakpointProc_devp->proc_entry = proc_create(CONFIG_PROC_NODE_AUTH_KEY, S_IRUSR | S_IWUSR, NULL, &hwBreakpointProc_proc_ops);
	}
	if (g_hwBreakpointProc_devp->proc_entry) {
		start_hide_procfs_dir(CONFIG_PROC_NODE_AUTH_KEY);
	} else {
		printk_debug(KERN_INFO "[rwNova] proc_create failed\n");
	}
#endif

#ifdef DEBUG_PRINTK
	printk_debug(KERN_EMERG "Hello, %s debug\n", CONFIG_PROC_NODE_AUTH_KEY);
	//test1();
	//test2();
	//test3();
	//test4();
#else
	printk_debug(KERN_EMERG "Hello\n");
#endif

	/* ── 【对标 AFEI】收尾：默认隐身 ──────────────────────────────────── */
#ifdef CONFIG_USE_ANON_CHANNEL
	/* 只注入分发函数，不创建 fd —— fd 必须由用户态触发（见 OnCmdAnonGetFd） */
	anon_channel_set_dispatcher(AnonDispatchThunk);
#endif

#ifdef CONFIG_HIDE_BY_DEFAULT
	hide_module_by_default();
#endif
	return 0;
}

static void hwBreakpointProc_dev_exit(void) {
	
#ifdef CONFIG_ANTI_PTRACE_DETECTION_MODE
	stop_anti_ptrace_detection();
#endif

	clean_hwbp();
	
#ifdef CONFIG_USE_PROC_FILE_NODE
	if(g_hwBreakpointProc_devp->proc_entry) {
		proc_remove(g_hwBreakpointProc_devp->proc_entry);
		g_hwBreakpointProc_devp->proc_entry = NULL;
	}
	
	if(g_hwBreakpointProc_devp->proc_parent) {
		proc_remove(g_hwBreakpointProc_devp->proc_parent);
		g_hwBreakpointProc_devp->proc_parent = NULL;
	}

	/* 【根因修复·对称清理】兜底按名字摘掉整棵子树，避免节点泄漏 */
	remove_proc_subtree(CONFIG_PROC_NODE_AUTH_KEY, NULL);

	stop_hide_procfs_dir();
#endif
	kfree(g_hwBreakpointProc_devp);
	printk_debug(KERN_EMERG "Goodbye\n");
}

int __init init_module(void) {
    return hwBreakpointProc_dev_init();
}

void __exit cleanup_module(void) {
    hwBreakpointProc_dev_exit();
}

#ifndef CONFIG_MODULE_GUIDE_ENTRY
//Hook:__cfi_check_fn
unsigned char* __check_(unsigned char* result, void *ptr, void *diag)
{
	printk_debug(KERN_EMERG "my__cfi_check_fn!!!\n");
	return result;
}

//Hook:__cfi_check_fail
unsigned char * __check_fail_(unsigned char *result)
{
	printk_debug(KERN_EMERG "my__cfi_check_fail!!!\n");
	return result;
}
#endif

unsigned long __stack_chk_guard;

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux");
MODULE_DESCRIPTION("Linux default module");

