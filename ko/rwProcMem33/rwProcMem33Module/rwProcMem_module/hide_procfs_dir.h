#ifndef _HIDE_PROCFS_DIR_H_
#define _HIDE_PROCFS_DIR_H_

#include "ver_control.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/string.h>

static char g_hide_dir_name[256] = {0};

static filldir_t old_filldir;

#if MY_LINUX_VERSION_CODE < KERNEL_VERSION(6,1,0)
static int my_filldir(struct dir_context *buf,
                      const char *name,
                      int namelen,
                      loff_t offset,
                      u64 ino,
                      unsigned int d_type)
{
    if (namelen == strlen(g_hide_dir_name) &&
        !strncmp(name, g_hide_dir_name, namelen))
    {
        return 0;
    }
    return old_filldir(buf, name, namelen, offset, ino, d_type);
}
#else
static bool my_filldir(struct dir_context *ctx,
                       const char *name,
                       int namelen,
                       loff_t offset,
                       u64 ino,
                       unsigned int d_type)
{
    if (namelen == strlen(g_hide_dir_name) &&
        !strncmp(name, g_hide_dir_name, namelen))
    {
        return true;
    }
    return old_filldir(ctx, name, namelen, offset, ino, d_type);
}
#endif

static int handler_pre(struct kprobe *kp, struct pt_regs *regs)
{
    struct dir_context *ctx = (struct dir_context *)regs->regs[1];
    old_filldir = ctx->actor;
    ctx->actor = my_filldir;
    return 0;
}

static struct kprobe kp_hide_procfs_dir = {
    .symbol_name = "proc_root_readdir",
    .pre_handler = handler_pre,
};

static bool start_hide_procfs_dir(const char* hide_dir_name)
{
	//这里原理上可以换成SKRoot的汇编写法。避免kprobe。
    int ret;
    /*【本地新增·6.12 适配】strlcpy -> strscpy
     * 依据 2026-08-18 日志（L350）：6.10+ 内核移除了 strlcpy。
     * strscpy 是内核自带的安全拷贝（include/linux/string.h），
     * 会强制检查并保证 NUL 结尾，语义与 strlcpy 一致。
     *
     * ⚠ strscpy 是**函数**，签名 ssize_t strscpy(char *, const char *, size_t)，
     *   三个参数缺一不可。这里原先写着「内核 strscpy 宏用 sizeof(dst)」并只传两参 ——
     *   那个说法是错的，6.12 上照样编译失败（too few arguments）。
     *   hwBreakpointProc_module 的同一份文件一直是三参，两边不一致正好说明这点。
     */
    strscpy(g_hide_dir_name, hide_dir_name, sizeof(g_hide_dir_name));
    ret = register_kprobe(&kp_hide_procfs_dir);
    if (ret) {
        printk_debug("[hide_procfs_dir] register_kprobe failed: %d\n", ret);
        return false;
    }
    printk_debug("[hide_procfs_dir] kprobe installed, hiding \"%s\"\n", g_hide_dir_name);
    return true;
}

static void stop_hide_procfs_dir(void)
{
    unregister_kprobe(&kp_hide_procfs_dir);
    printk_debug("[hide_procfs_dir] kprobe removed\n");
}

#endif  // _HIDE_PROCFS_DIR_H_
