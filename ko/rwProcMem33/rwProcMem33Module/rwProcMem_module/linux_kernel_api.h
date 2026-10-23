#ifndef LINUX_KERNEL_API_H_
#define LINUX_KERNEL_API_H_
#include "ver_control.h"

#include <linux/module.h>
#include <asm/memory.h>

/* ============================================================================
 *【本地新增·6.12 适配】x_probe_kernel_read 改为直接 memcpy
 *
 * 背景（实测排查结论）：
 *   原实现 #if >= KERNEL_VERSION(5,8,0) 分支调用 copy_from_kernel_nofault，
 *   但该符号在 6.12 上无法给外部模块链接：
 *     - 源码 mm/maccess.c:50 确有 EXPORT_SYMBOL_GPL(copy_from_kernel_nofault)
 *     - mm/maccess.o 确有 __export_symbol_copy_from_kernel_nofault
 *     - mm/maccess.symtypes 确有该符号原型
 *     - 但 Module.symvers / vmlinux.symvers 里都没有它
 *     - 设备侧 375 个厂商 ko 的 __versions 里同样查不到它
 *   即：它不在本机内核的可用导出符号集内，任何版本配置都编不出来。
 *   （2026-08-18 日志记的 missing=1 copy_from_kernel_nofault 就是同一个问题，
 *     当年也没解决，只是标了"预期"。）
 *
 * 为什么可以直接 memcpy：
 *   唯一调用点是 phy_mem.h:219 的 read_ram_physical_addr()，那里拿到的是
 *   xlate_dev_mem_ptr() 返回的**内核虚拟地址**（映射好的物理页），
 *   而不是用户地址。在内核上下文里对内核虚拟地址做 memcpy 是允许的——
 *   原代码用 copy_from_kernel_nofault 只是为了"万一会fault"更保险。
 *   驱动已经用 x_kmalloc(GFP_KERNEL) 分配了合法内核缓冲区，
 *   目标地址是已映射的内核页，不存在用户指针那种校验需求。
 *
 * 注意保留了原 probe_kernel_read(<5.8) 分支不动，避免影响旧内核路径。
 */
#if MY_LINUX_VERSION_CODE < KERNEL_VERSION(5,8,0)

long probe_kernel_read(void* dst, const void* src, size_t size);

static long x_probe_kernel_read(void* bounce, const char* ptr, size_t sz) {
    return probe_kernel_read(bounce, ptr, sz);
}

#endif

#if MY_LINUX_VERSION_CODE >= KERNEL_VERSION(5,8,0)

/* ★若开启 CONFIG_ENABLE_FAST_PHYMEM，慢路径 read_ram_physical_addr 不再
 *   调用本函数（改走 __va + kmap_local_page），会触发 -Wunused-function。
 *   它属于"按开关条件使用"的工具函数，故标 __maybe_unused 而非删除，
 *   以保留慢路径可用性。 */
static __maybe_unused long x_probe_kernel_read(void* bounce, const char* ptr, size_t sz) {
    memcpy(bounce, ptr, sz);
    return 0;
}

#endif


#if MY_LINUX_VERSION_CODE < KERNEL_VERSION(6,6,0)
static inline pte_t x_pte_mkwrite(pte_t pte) {
    return pte_mkwrite(pte);
}
#else
static inline pte_t x_pte_mkwrite(pte_t pte) {
    struct vm_area_struct vma = {.vm_flags = VM_READ};
    return pte_mkwrite(pte, &vma);
}
#endif

#if MY_LINUX_VERSION_CODE < KERNEL_VERSION(6,6,0)
static size_t x_read_mm_struct_rss(struct mm_struct * mm, ssize_t offset) {
        struct mm_rss_stat *rss_stat = (struct mm_rss_stat *)((size_t)&mm->rss_stat + offset);
        size_t total_rss;
		ssize_t val1, val2, val3;
		val1 = atomic_long_read(&rss_stat->count[MM_FILEPAGES]);
		val2 = atomic_long_read(&rss_stat->count[MM_ANONPAGES]);
#ifdef MM_SHMEMPAGES
		val3 = atomic_long_read(&rss_stat->count[MM_SHMEMPAGES]);
#else
		val3 = 0;
#endif
		if (val1 < 0) { val1 = 0; }
		if (val2 < 0) { val2 = 0; }
		if (val3 < 0) { val3 = 0; }
		total_rss = val1 + val2 + val3;
        return total_rss;
}
#else
static size_t x_read_mm_struct_rss(struct mm_struct * mm, ssize_t offset) {
        struct percpu_counter *rss_stat = (struct percpu_counter *)((size_t)&mm->rss_stat + offset);
        size_t total_rss;
		ssize_t val1, val2, val3;
		val1 = percpu_counter_read(&rss_stat[MM_FILEPAGES]);
        val2 = percpu_counter_read(&rss_stat[MM_ANONPAGES]);

        #ifdef MM_SHMEMPAGES
        val3 = percpu_counter_read(&rss_stat[MM_SHMEMPAGES]);
        #else
        val3 = 0;
        #endif
		if (val1 < 0) { val1 = 0; }
		if (val2 < 0) { val2 = 0; }
		if (val3 < 0) { val3 = 0; }
		total_rss = val1 + val2 + val3;
        return total_rss;
}
#endif

#endif /* LINUX_KERNEL_API_H_ */
