#ifndef ANON_CHANNEL_H_
#define ANON_CHANNEL_H_
/* ============================================================================
 * anon_channel.h —— 匿名 inode 通信通道（AFEI same/paradise 同款移植）
 *
 * ── 来源与依据 ────────────────────────────────────────────────────────────
 * 逆向样本：阿飞2.01 / assets/same-sdk36-6.12.ko
 *   same_anon_fops     @ .rodata+0xfc0（file_operations 表）
 *   same_anon_open     @ .text+0x00f8
 *   same_anon_ioctl    @ .text+0x0254   ← 真正的命令分发入口
 *   same_anon_release  @ .text+0x0490
 *   same_install_fd    @ .text+0x0038   ← 把 fd 装进目标进程
 *   same_install_fd_tw_func @ .text+0x0664（task_work 回调）
 *   same_get_file_private   @ .text+0x0004
 *   外部依赖：anon_inode_getfile（内核导出）、task_work_add（内核导出）
 *
 * ── AFEI 原始实现（反汇编还原）────────────────────────────────────────────
 *   same_anon_open(filp):
 *       ctx = kmalloc(0xCC0, GFP_KERNEL)          // 0xCC0 = 3264 字节会话上下文
 *       ctx->... = kzalloc(4, ...)                 // 首字段是个 4 字节小对象
 *       filp->private_data = ctx
 *       return 0                                    // 失败返回 -ENOMEM(-12)
 *
 *   same_anon_ioctl(filp, cmd, arg):
 *       取 filp->private_data 作为 ctx
 *       按 cmd 分发到各个 do_* 服务例程
 *       （对应符号：do_read_physical_memory / do_write_physical_memory /
 *         do_find_process / do_hide_process / do_list_processes /
 *         do_get_module_base / do_is_process_alive / do_config_gyro_hook …）
 *
 *   same_install_fd(pid):
 *       fd = find unused fd 区域（AFEI 日志 "failed to get unused fd"）
 *       file = anon_inode_getfile("same", &same_anon_fops, NULL, O_RDWR)
 *              （O_RDWR=2，即 mov w3,#2 后 movk #8,lsl16 → 0x80002）
 *       same_anon_open(file)                        // 手工初始化上下文
 *       task_work_add(target_task, tw, TWA_RESUME)   // 让目标进程自己收 fd
 *       日志 "same_install_fd: failed to create anon inode file"
 *            "same_install_fd: failed to write fd to user space, closing fd"
 *
 * ── 为什么匿名 inode 比 /proc 节点隐蔽 ─────────────────────────────────────
 *   1) 不创建任何 /proc 或 /sys 目录项 ⇒ 遍历 /proc 查不到痕迹。
 *   2) 匿名 inode 的 fd 在 /proc/<pid>/fd/N 里显示为
 *      "anon_inode:same"，看起来像 epoll/eventfd 之类常见内核对象。
 *   3) 权限完全由驱动自己控制，不经过 procfs 的 VFS 权限检查。
 *   4) 所有命令走 ioctl（不是 read），语义上是"设备控制"，
 *      与 AFEI 在 rwProcMem 里看到的 read(33+size) 完全不同形态。
 *
 * ── 移植差异 ──────────────────────────────────────────────────────────────
 *   ★本移植**不替换**你现有 /proc 通道，而是**并存**：
 *     由 ver_control.h 的 CONFIG_USE_ANON_CHANNEL 控制是否启用。
 *     两者共用同一套 DispatchCommand 语义，命令码一致 ⇒ 用户态无感。
 *   ★上下文结构体只保留本模块实际需要的字段（AFEI 的 0xCC0 里有很多
 *     gyro/ptebp 等本模块没有的东西），大小按需裁剪，不盲目照抄。
 *   ★fd 交付：AFEI 是"驱动主动往目标进程塞 fd"；
 *     本移植提供**两种**模式：
 *       (a) 驱动主动塞（同 AFEI，需 task_work_add）
 *       (b) 把自己进程 open 到的 fd 交给用户态自己 dup（更简单、更安全）
 *     默认走 (b)，因为你的调用方式本来就是"自己进程开 fd 再 read/ioctl"。
 * ==========================================================================*/

#include "ver_control.h"

#ifdef CONFIG_USE_ANON_CHANNEL

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "api_proxy.h"
#include "rw_symbol_lookup.h"

/* 与 /proc 通道共用的命令头（保持二进制兼容，用户态无需区分两条通道）*/
#pragma pack(push,1)
struct anon_ioctl_request {
	char     cmd;
	uint64_t param1;
	uint64_t param2;
	uint64_t param3;
	uint64_t buf_size;
};
#pragma pack(pop)

/* 匿名通道会话上下文（按需裁剪，不做 AFEI 的 0xCC0 全量拷贝）*/
struct anon_channel_ctx {
	u32 magic;              /* 用于私有数据校验，防止 fd 混用 */
	void *priv;             /* 对应 AFEI 的 4 字节小对象 */
};

#define ANON_CHANNEL_MAGIC 0x41464549u   /* "AFEI" */

/* --------------------------------------------------------------------------
 * open：分配会话上下文
 * 对应 AFEI same_anon_open @0xf8
 * ------------------------------------------------------------------------*/
static int anon_channel_open(struct inode *inode, struct file *filp)
{
	struct anon_channel_ctx *ctx;

	(void)inode;

	ctx = x_kmalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->magic = ANON_CHANNEL_MAGIC;
	ctx->priv  = x_kmalloc(4, GFP_KERNEL);
	if (!ctx->priv) {
		kfree(ctx);
		return -ENOMEM;      /* AFEI 此处也是返回 -12 */
	}

	filp->private_data = ctx;
	return 0;
}

/* --------------------------------------------------------------------------
 * release：释放会话上下文
 * 对应 AFEI same_anon_release @0x490
 * ------------------------------------------------------------------------*/
static int anon_channel_release(struct inode *inode, struct file *filp)
{
	struct anon_channel_ctx *ctx = filp->private_data;

	(void)inode;

	if (ctx) {
		if (ctx->priv)
			kfree(ctx->priv);
		kfree(ctx);
		filp->private_data = NULL;
	}
	return 0;
}

/* --------------------------------------------------------------------------
 * ioctl：命令分发
 * 对应 AFEI same_anon_ioctl @0x254
 *
 * ★这里刻意**不**实现具体业务，只做分发转接 ——
 *   实际业务由 rwProcMem_module.c 的 DispatchCommand 处理，
 *   保证 /proc 通道与匿名通道走同一套命令实现，不重复代码。
 *   由 anon_channel_set_dispatcher() 在 init 时注入。
 * ------------------------------------------------------------------------*/
typedef long (*anon_dispatch_fn)(struct anon_ioctl_request *req, char __user *buf);

static anon_dispatch_fn g_anon_dispatcher;

static void anon_channel_set_dispatcher(anon_dispatch_fn fn)
{
	g_anon_dispatcher = fn;
}

static long anon_channel_ioctl(struct file *filp, unsigned int cmd,
                               unsigned long arg)
{
	struct anon_channel_ctx *ctx = filp->private_data;
	struct anon_ioctl_request req;
	char __user *ubuf;

	/* 私有数据校验：防止别的匿名 inode fd 误进这里 */
	if (!ctx || ctx->magic != ANON_CHANNEL_MAGIC)
		return -EINVAL;

	/*
	 * 约定：cmd 传入 sizeof(req)，arg 是用户态缓冲区基址。
	 * 用户态布局 = [struct anon_ioctl_request][payload...]
	 * （与 /proc 通道的 read() 写法完全一致，便于复用）
	 */
	if (cmd < sizeof(req))
		return -EINVAL;

	ubuf = (char __user *)arg;
	if (!ubuf)
		return -EINVAL;

	if (x_copy_from_user(&req, ubuf, sizeof(req)))
		return -EFAULT;

	if (!g_anon_dispatcher)
		return -ENOSYS;

	return g_anon_dispatcher(&req, ubuf + sizeof(req));
}

/* 对应 AFEI same_anon_fops @.rodata+0xfc0 */
static const struct file_operations anon_channel_fops = {
	.owner          = THIS_MODULE,
	.open           = anon_channel_open,
	.release        = anon_channel_release,
	.unlocked_ioctl = anon_channel_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = anon_channel_ioctl,
#endif
};

/* --------------------------------------------------------------------------
 * anon_channel_install_fd —— 在【当前进程】上下文里装一个匿名通道 fd
 *
 * ★★ 2026-10-10 真机实测纠正（重要设计缺陷修复）：
 *   原实现在驱动 init() 里调本函数，fd 会装进 **insmod 进程**的 fd 表。
 *   insmod 执行完 init_module 立刻退出 ⇒ 该 fd 随进程回收，
 *   通道当场消失。真机验证：全系统扫 `/proc/<pid>/fd` 找不到任何
 *   `anon_inode:rwproc`。
 *
 *   正解：`fd_install()` 装的是**发起该系统调用的进程**的 fd 表。
 *   因此本函数只能在"用户态主动触发的路径"里调用 ——
 *   即由用户态 ioctl 进来，这样 fd 落在用户态自己的进程里。
 *   见 rwProcMem_module.c 里 CMD_ANON_GET_FD 的处理。
 *
 *   本函数只管"造 file + 装 fd"，不作业务判断，供上层复用。
 * ------------------------------------------------------------------------*/
static int anon_channel_install_fd(void)
{
	struct file *file;
	int fd;

	fd = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printk_debug(KERN_INFO "[rwanon] failed to get unused fd\n");
		return fd;
	}

	/*
	 * anon_inode_getfile(name, fops, priv, flags)
	 * AFEI 用的名字是 "same"，这里用 "rwproc" —— 会显示在
	 * /proc/<pid>/fd/N -> anon_inode:rwproc，看起来像内核对象。
	 */
	/* 名字会显示成 /proc/<pid>/fd/N -> anon_inode:<name>。
	 * 用 "dmabuf" 而不是 "rwproc"：后者等于在 fd 表里直接写着"内存读写通道"。
	 * dmabuf 是 Android 上极常见的内核对象，混在 graphics 相关 fd 里不显眼。 */
	file = anon_inode_getfile("dmabuf", &anon_channel_fops, NULL, O_RDWR);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		printk_debug(KERN_INFO "[rwanon] failed to create anon inode file\n");
		return PTR_ERR(file);
	}

	/* 手动走一遍 open 初始化上下文（同 AFEI 手工调 same_anon_open）*/
	{
		int r = anon_channel_open(file_inode(file), file);
		if (r) {
			fput(file);
			put_unused_fd(fd);
			return r;
		}
	}

	fd_install(fd, file);
	printk_debug(KERN_INFO "[rwanon] fd installed: %d\n", fd);
	return fd;
}

static void anon_channel_destroy(void)
{
	/* 匿名 fd 归属用户进程，随其 close/退出自动 release，这里无需清理。
	 * 若曾在 init 里误装过 fd，那属于 bug，已在上面说明并移除。 */
	printk_debug(KERN_INFO "[rwanon] anon channel destroyed\n");
}

#endif /* CONFIG_USE_ANON_CHANNEL */
#endif /* ANON_CHANNEL_H_ */
