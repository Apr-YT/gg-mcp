#ifndef VERSION_CONTROL_H_
#define VERSION_CONTROL_H_

// 独立内核模块入口模式
#define CONFIG_MODULE_GUIDE_ENTRY

// 生成proc用户层交互节点文件
#define CONFIG_USE_PROC_FILE_NODE
// 隐蔽通信密钥
#define CONFIG_PROC_NODE_AUTH_KEY "e84523d7b60d5d341a7c4d1861773ecd"

/* ============================================================================
 * 【AFEI 移植开关】以下三项来自逆向 阿飞2.01（same/paradise 驱动），
 * 逐条已用反汇编验证（见 cfi_bypass.h / anon_channel.h / phy_mem_fast.h 头部注释）。
 * 全部默认按"最小侵入"原则配置：原 /proc 通道行为完全不变。
 * ==========================================================================*/

/* ① 运行时 CFI 绕过（AFEI cfi_bypass @0x156c / disable_kprobe_blacklist @0x0da0）
 *    把 __cfi_check_fail 等失败处理函数 patch 成 return 1，
 *    消除 CONFIG_CFI_CLANG 内核上的 CFI panic 死机点。
 *    ★默认开启：这是本移植最有价值的一项，且不改变任何对外接口。 */
#define CONFIG_ENABLE_CFI_BYPASS

/* ── 【对标 AFEI】默认隐身 ────────────────────────────────────────────────
 * AFEI 的 hide_module() 是在 init_module 里**默认就调用**的：list_del_init
 * 摘掉链表 + kobject_del 删 sysfs，于是 lsmod / /proc/modules / /sys/module
 * 三处从一开始就看不到驱动。
 *
 * 我方原先只在用户态主动发 CMD_HIDE_KERNEL_MODULE 时才隐藏 —— "有武器但收
 * 在鞘里"，默认站在明处。本开关（默认开）让 init 末尾自动执行同样的隐藏。
 *
 * 注意：隐藏后 rmmod 依然可用（模块引用计数与链表分离），但 lsmod 不可见；
 *       调试期可注释掉本行恢复可见。 */
#define CONFIG_HIDE_BY_DEFAULT

/* ② 匿名 inode 通信通道（AFEI same_anon_ioctl / same_install_fd）
 *    不创建 /proc 节点，改用 anon_inode 的 ioctl 通道，更难被发现。
 *    ★默认**关闭**（下面这行保持注释状态就是关）。
 *    开启方式二选一：
 *      (a) 编译时传 -DCONFIG_USE_ANON_CHANNEL=1（推荐，不动源码）
 *      (b) 把下面这行的注释放开
 *    ★注意：`#ifdef` 只判"有没有定义"，不判值。
 *      所以 -D...=1 展开出的 "1" 与源码里的"空定义"都能让 #ifdef 成立。 */
/* 2026-10-23 启用：agentd 在拿到匿名 fd 后，所有命令改走 ioctl(fd, 33, buf)，
 * proc 通道只用于首次引导（fd_install 必须在用户态触发的路径里完成）。
 * 节点仍保留 —— agentd 崩溃重启时还要靠它重新引导，见 AGENTS 第 31 条。 */
#define CONFIG_USE_ANON_CHANNEL

/* ③ 物理内存直读直写快路径（AFEI swap_read/write_page_direct / *_fast）
 *    用 virt_to_page + kmap_local_page 替代 xlate_dev_mem_ptr + bounce 两层间接。
 *    ★已实测（2026-10-10，真机 REDMI K90 Pro Max / 6.12.23）：
 *        慢路径 ~690 MB/s（单次 90us） → 快路径 ~900 MB/s（单次 69us），提速 ~24%
 *      功能四判据（ELF魔数/代码段全等/跨进程64KB全等/写往返AB魔数）全绿。
 *    ⇒ 因收益明确，**默认开启**。若某内核上 __va 线性映射不适用
 *      （HIGHMEM 等），把下面这行注释掉即可回到慢路径，接口完全一致。 */
#define CONFIG_ENABLE_FAST_PHYMEM

/* 打印内核调试信息 */
//#define CONFIG_DEBUG_PRINTK


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
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(6,6,30)
// 目标设备：25102RKBEC / myron / Android 16
//   uname -r = 6.12.23-android16-5-g16e473de48a3-abogki462654244-4k
// 注意：源码里所有版本分支的最高阈值就是 6.6.30，6.12.23 全部走 >= 分支，
//   与 6.6.30 生成的代码路径完全相同；改此处只为语义正确，不影响编译产物语义。
#define MY_LINUX_VERSION_CODE KERNEL_VERSION(6,12,23)
#endif

#ifdef CONFIG_DEBUG_PRINTK
#define printk_debug printk
#else
static inline void printk_debug(char *fmt, ...) {}
#endif

#endif /* VERSION_CONTROL_H_ */
