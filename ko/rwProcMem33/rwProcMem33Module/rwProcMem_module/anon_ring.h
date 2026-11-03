#ifndef ANON_RING_H_
#define ANON_RING_H_
/* ============================================================================
 * anon_ring.h —— 匿名通道的共享内存控制环（mmap 数据面）
 *
 * ── 解决什么 ──────────────────────────────────────────────────────────────
 * 阿飞那套（anon_channel.h）已经做到「不新增可枚举对象 + fd 生命周期自管」，
 * 但每条命令仍是一次 ioctl：seccomp / ftrace / LSM 能看到一条条的 ioctl 记录。
 * 本文件把**数据面**搬到 mmap 出来的共享页：请求头、payload、结果全部只是
 * 内存读写，系统调用从数据路径上消失。
 *
 * ── 必须如实说明的三件事（别被"零 syscall"这个说法带偏）─────────────────
 * 1) 性能收益接近零。现有 ioctl 路径只 copy_from_user 33 字节头，
 *    payload 本来就直接读写用户缓冲区（read_ram_physical_addr 不落地中转）。
 *    一次物理内存读实测 69us，省下的 syscall 陷入约 1us。**这不是提速方案。**
 * 2) 换来的是一个常驻内核线程。内核没有"用户往共享页写了东西"的事件源，
 *    要免 syscall 就只能主动轮询。轮询线程本身是 /proc 里可见的对象 ——
 *    等于拿"线程可见"换"syscall 足迹"。是否划算取决于对手看什么。
 * 3) 只支持 64 位。req_seq/done_seq 依赖单条 64 位原子 ld/st（arm64 满足）。
 *
 * ── 为什么内核侧必须 use_mm ────────────────────────────────────────────────
 * 所有 handler 都是拿 `char __user *buf` 直接 copy_to_user 的。kthread 的
 * current->mm 是 NULL，直接 copy_to_user 一律失败。所以处理前 use_mm()
 * 切到 ring 所属进程的地址空间，让 buf 这个**用户地址**真正可访问 ——
 * 这样 handler 一行都不用改。ring 页本身就是该进程的一个 vma，页表项现成，
 * 不会在 kthread 里触发缺页。
 *
 * ── 安全边界（关键，不可"优化"掉）─────────────────────────────────────────
 * ★ req.buf_size 来自共享页，等价于**用户可控**。必须 clamp 到 payload 容量，
 *   否则 handler 会按这个长度在 ring 里读写到映射区之外 —— 内核越界。
 *   现有 ioctl 路径有 `size < header + buf_size` 这道外部检查兜底，
 *   ring 路径没有，只能在这里挡。
 * ★ handler 里还有几处无视 buf_size 的定长访问（init_device_info 1037B、
 *   rss 8B、cmdline 16B 等），所以 payload 容量不能小于一页 ——
 *   RING_MIN_SIZE 保证 payload >= PAGE_SIZE-33。
 *
 * ── 用户态协议（顺序不能变）───────────────────────────────────────────────
 *   1. mmap 后先校验 hdr->magic == RING_MAGIC 且 version 匹配
 *   2. ★单槽位，没有并发保护。同一个环上的请求必须由**一个线程串行**提交。
 *      要多线程并发就各 open 一个 fd、各 mmap 一个环（per-fd 隔离）
 *   3. slot 布局 = [struct anon_ioctl_request 33 字节][payload]
 *      buf_size 必须 ≤ ring_size - slot_off - 33，超了内核直接回 EINVAL
 *   4. 写完后 store_release(req_seq, n+1)，然后自旋读 done_seq
 *   5. done_seq == n+1 时：err == 0 取 result，否则按 err 处理
 *   6. ★超时/驱动线程已停时**不得复用本环**：内核可能还在处理超时的那笔
 *      请求，复用会算出同一个 seq，让下一笔命令拿到旧命令的结果（静默的
 *      错误结果，比报错危险得多）。agentd 的做法（driver.c 的 -2 分支）：
 *      munmap 拆环（vm_close → kthread_stop 会等旧请求做完）→ 回落 ioctl，
 *      直到重连。要继续用环就重新 open + mmap 拿新环
 *   7. CMD_INIT_DEVICE_INFO / CMD_ANON_GET_FD 在环上会被拒（-EPERM）：
 *      它们依赖真实进程的 current 上下文，见 rwProcMem_module.c 的说明
 *
 * ── 生存期（两条撤销路径，都必须幂等）─────────────────────────────────────
 *   fd close  → ring_owner_detach()：停线程，**不释放内存**（vma 还映射着，
 *               vfree 会让用户态访问到已归还的物理页 = 读别人内存）
 *   munmap/退出 → ring_vm_close()：停线程 → mmput → vfree → kfree
 *
 *   ★两条路径的顺序由 mm 子系统保证，不是由锁保证：vma 持有 file 引用
 *     （mmap_region 里 vma->vm_file = get_file(file)），环还映射着时 fd 的
 *     最后一个 file 引用就不会归零 → release（detach）必然晚于 vm_close。
 *     也就是说 detach 正常流程里只是兜底，跑到时 ctx->ring 已被摘空。
 *   stop_lock 的真实作用是防御纵深：万一未来有路径绕过 vma 的 file 引用
 *     直接并发碰 ctx，它保证两个 stop 串行、backref 只摘一次，不会出现
 *     vfree 时线程还在跑。别删，但别把它当成本设计的正确性依据。
 * ★ ring 的生命周期跟 fd 走：mmap 之后必须保持 fd 打开。close(fd) 后共享页
 *   内存还在、还能读写，但没有线程处理请求了（表现为永久超时）。
 * ==========================================================================*/

#include "ver_control.h"

#ifdef CONFIG_ANON_RING

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/atomic.h>

#define RING_MAGIC      0x31474E52u   /* "RNG1"：用户态据此确认这页已被驱动接管 */
#define RING_VERSION    1u
#define RING_SLOT_OFF   PAGE_SIZE     /* 头部独占第一页，payload 从第二页起 */
#define RING_MIN_SIZE   (PAGE_SIZE * 2u)

#define RING_ST_READY   1u
#define RING_ST_STOPPED 2u

/* 空闲退避区间（微秒）。调小 → 单命令延迟低、CPU 占用高；调大反之。
 * 200~400us 是相对一次物理内存读（69us）留出的安全余量，
 * 再小就变成纯忙等，功耗与收益不成比例。 */
#define RING_IDLE_US_MIN 200u
#define RING_IDLE_US_MAX 400u

/* 内核线程名。中性命名，不伪造 kworker —— 伪造会干扰真实运维判断，
 * 且 comm 只有 16 字节，重名线程在 ps 里反而更容易被盯上。 */
#define RING_THREAD_NAME "rwnringd"

/* 布局必须与用户态逐字节一致 */
struct ring_hdr {
	u32 magic;
	u32 version;
	u32 slot_off;      /* payload 区起始偏移 */
	u32 ring_size;     /* 整个映射大小，用户态用它算 payload 容量 */
	u64 req_seq;       /* 用户态递增（release 写） */
	u64 done_seq;      /* 内核递增（release 写） */
	s64 result;        /* handler 返回值，err == 0 时有效 */
	s32 err;           /* 失败时为正的 errno，0 表示成功 */
	u32 state;         /* RING_ST_* */
	u8  pad[16];
} __attribute__((aligned(8)));

struct ring_owner {
	struct mutex stop_lock;  /* 串行化"停线程"，见文件头生存期说明 */
	void *kva;
	unsigned long ksize;
	unsigned long user_base; /* vma->vm_start，本文件唯一依赖的用户地址 */
	struct mm_struct *mm;
	struct task_struct *thread;
	struct ring_owner **backref; /* 指回 anon_channel_ctx->ring，避免悬空指针 */
	int stopped;
};

/* 由 anon_channel_set_dispatcher() 一并注入，处理请求时转调统一分发 */
static anon_dispatch_fn g_ring_dispatcher;

static void anon_ring_set_dispatcher(anon_dispatch_fn fn)
{
	g_ring_dispatcher = fn;
}

static int ring_thread_fn(void *arg)
{
	struct ring_owner *o = arg;
	struct ring_hdr *h = o->kva;
	char *slot = (char *)o->kva + RING_SLOT_OFF;
	size_t cap = o->ksize - RING_SLOT_OFF - sizeof(struct anon_ioctl_request);
	u64 done = smp_load_acquire(&h->done_seq);

	while (!kthread_should_stop()) {
		u64 req = smp_load_acquire(&h->req_seq);
		struct anon_ioctl_request rq;
		long ret;
		char __user *ubuf;

		if (req == done) {
			usleep_range(RING_IDLE_US_MIN, RING_IDLE_US_MAX);
			continue;
		}

		/* 请求头从内核虚拟地址读，不经过 copy_from_user：
		 * 此时还没 use_mm，用户地址不可用。头 33 字节，拷贝开销可忽略。 */
		memcpy(&rq, slot, sizeof(rq));

		if (rq.buf_size > cap) {
			/* 见文件头"安全边界"：这里放过去就是内核越界读写 */
			h->result = 0;
			h->err = EINVAL;
		} else {
			/* 6.4 起 use_mm/unuse_mm 改名为 kthread_use_mm/kthread_unuse_mm。
			 * 目标是 6.12，直接走新名；若要兼顾 6.3 及更早，这里需要版本分支。 */
			kthread_use_mm(o->mm);
			ubuf = (char __user *)(o->user_base + RING_SLOT_OFF +
			                       sizeof(struct anon_ioctl_request));
			ret = g_ring_dispatcher ? g_ring_dispatcher(&rq, ubuf) : -ENOSYS;
			kthread_unuse_mm(o->mm);

			if (ret < 0) {
				h->result = 0;
				h->err = (s32)-ret;
			} else {
				h->result = (s64)ret;
				h->err = 0;
			}
		}

		done = req;
		smp_store_release(&h->done_seq, done);
	}
	return 0;
}

/* 返回时保证：① 线程已退出；② backref 已摘除且 ctx 槽位已清空。
 *
 * ★ 正常流程下 vm_close 与 release 不会并发（vma 持 file 引用，见文件头
 *   生存期说明），这里的锁是防御纵深：backref 的读写全部收在锁内，
 *   若未来出现绕过 vma 引用的并发路径，也不会出现对已 kfree 的 ctx 写入。
 * ★ kthread_stop 必须在锁内，且不能在锁外"看到 thread==NULL 就直接返回"：
 *   那会让 vfree 跑在 kthread_stop 前面，线程还在访问已释放的 ring 页。 */
static void ring_owner_stop(struct ring_owner *o)
{
	mutex_lock(&o->stop_lock);

	if (o->backref) {
		if (*o->backref == o)
			*o->backref = NULL;
		o->backref = NULL;
	}

	if (!o->stopped) {
		struct task_struct *t = o->thread;

		o->thread = NULL;
		if (t) {
			((struct ring_hdr *)o->kva)->state = RING_ST_STOPPED;
			kthread_stop(t);
		}
		o->stopped = 1;
	}

	mutex_unlock(&o->stop_lock);
}

static void ring_vm_close(struct vm_area_struct *vma)
{
	struct ring_owner *o = vma->vm_private_data;

	if (!o)
		return;

	vma->vm_private_data = NULL;
	ring_owner_stop(o);   /* 内含 backref 摘除（经 stop_lock 与 detach 串行） */

	mmput(o->mm);
	vfree(o->kva);
	kfree(o);
}

static const struct vm_operations_struct ring_vm_ops = {
	.close = ring_vm_close,
};

/* vma 已完整映射后才算接管；任何一步失败都自己清理并返回错误，
 * 不把半成品留给 vm_close（那时 vma 还没指向 owner）。 */
static int ring_owner_attach(struct ring_owner **slot, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	struct ring_owner *o;
	struct ring_hdr *h;
	int ret;

	/* size 上限 U32_MAX：ring_size 在头部是 u32，超了会被静默截断，
	 * 用户态按截断值算出的 payload 容量就会大于真实映射区。 */
	if (vma->vm_pgoff != 0 || size < RING_MIN_SIZE ||
	    size > (unsigned long)U32_MAX || (size & ~PAGE_MASK))
		return -EINVAL;

	o = kzalloc(sizeof(*o), GFP_KERNEL);
	if (!o)
		return -ENOMEM;
	mutex_init(&o->stop_lock);

	o->kva = vmalloc_user(size);
	if (!o->kva) {
		kfree(o);
		return -ENOMEM;
	}
	o->ksize = size;

	h = o->kva;
	h->magic = RING_MAGIC;
	h->version = RING_VERSION;
	h->slot_off = RING_SLOT_OFF;
	h->ring_size = (u32)size;
	h->state = RING_ST_READY;

	o->mm = get_task_mm(current);
	if (!o->mm) {
		vfree(o->kva);
		kfree(o);
		return -EFAULT;
	}
	o->user_base = vma->vm_start;

	ret = remap_vmalloc_range(vma, o->kva, 0);
	if (ret) {
		mmput(o->mm);
		vfree(o->kva);
		kfree(o);
		return ret;
	}

	/* VM_DONTCOPY：fork 出来的子进程若共享这块页，它的请求会由只用
	 *   父进程 mm 的 kthread 处理，用户地址对不上 → 结果写到错误的内存。
	 * VM_DONTDUMP：通信页不该被 core dump 带出去。
	 * VM_DONTEXPAND：vmalloc 区不支持 expand，mremap 要拒绝。 */
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	vma->vm_ops = &ring_vm_ops;
	vma->vm_private_data = o;
	o->backref = slot;
	*slot = o;

	/* 放到最后：失败时 vma 已指向 owner，内核会自动 munmap →
	 * vm_close 兜底释放，这里不能重复清理。但要先把 ctx 的指针和
	 * backref 摘掉，否则 mmap 返回错误后 ctx->ring 仍非 NULL，
	 * 紧接着重试 mmap 会误报 -EBUSY。 */
	o->thread = kthread_run(ring_thread_fn, o, RING_THREAD_NAME);
	if (IS_ERR(o->thread)) {
		ret = PTR_ERR(o->thread);
		o->thread = NULL;
		o->stopped = 1;
		o->backref = NULL;
		*slot = NULL;
		return ret;
	}
	return 0;
}

/* fd close 路径：只停线程，内存交给 vm_close。 */
static void ring_owner_detach(struct ring_owner **slot)
{
	struct ring_owner *o = *slot;

	if (!o)
		return;

	/* stop 内部经 stop_lock 摘 backref 并把 *slot 清空；
	 * 返回后 ctx 可以被安全释放，vm_close 不会再来碰它。 */
	ring_owner_stop(o);
	*slot = NULL;
}

#else  /* !CONFIG_ANON_RING */

struct ring_owner;

static inline void anon_ring_set_dispatcher(void *fn) { (void)fn; }
static inline int ring_owner_attach(struct ring_owner **slot,
                                    struct vm_area_struct *vma)
{
	(void)slot;
	(void)vma;
	return -EOPNOTSUPP;
}
static inline void ring_owner_detach(struct ring_owner **slot) { (void)slot; }

#endif /* CONFIG_ANON_RING */
#endif /* ANON_RING_H_ */
