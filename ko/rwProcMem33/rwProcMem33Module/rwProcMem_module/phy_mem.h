#ifndef PHY_MEM_H_
#define PHY_MEM_H_
//声明
//////////////////////////////////////////////////////////////////////////
#include <linux/fs.h>
#include <linux/pid.h>
#include <asm/page.h>
#include "phy_mem_auto_offset.h"
#include "api_proxy.h"
#include "ver_control.h"

/* ── 【AFEI 移植】fast 路径前置声明 ─────────────────────────────────────
 * 包含顺序：rwProcMem_module.h 先 include phy_mem.h，后 include phy_mem_fast.h，
 * 而 phy_mem.h 的实现里要用 fast_*_paddr，故在此前置声明，
 * 实现在 phy_mem_fast.h（已被 ver_control.h 的开关保护）。
 * 只有开关打开时才需要，关闭时不引用，避免"未声明"错误。
 * ------------------------------------------------------------------- */
#ifdef CONFIG_ENABLE_FAST_PHYMEM
static inline size_t fast_read_paddr(size_t phy_addr, void *dst, size_t size, bool to_user);
static inline size_t fast_write_paddr(size_t phy_addr, const void *src, size_t size, bool from_user);
#endif

static inline int is_pte_can_read(pte_t* pte);
static inline int is_pte_can_write(pte_t* pte);
static inline int is_pte_can_exec(pte_t* pte);
static inline int change_pte_read_status(pte_t* pte, bool can_read);
static inline int change_pte_write_status(pte_t* pte, bool can_write);
static inline int change_pte_exec_status(pte_t* pte, bool can_exec);

static inline size_t get_proc_phy_addr(struct pid* proc_pid_struct, size_t virt_addr, pte_t** out_pte);
static inline size_t read_ram_physical_addr(bool is_kernel_buf, size_t phy_addr, char* lpBuf, size_t read_size);
static inline size_t write_ram_physical_addr(size_t phy_addr, char* lpBuf, bool is_kernel_buf, size_t write_size);

//实现
//////////////////////////////////////////////////////////////////////////
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/slab.h>

#if MY_LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,83)
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#endif


#define RETURN_VALUE(size_t_ptr___out_ret, size_t___value) *size_t_ptr___out_ret=size_t___value;break;

#include <asm/pgtable.h>

static inline size_t get_proc_phy_addr(struct pid* proc_pid_struct, size_t virt_addr, pte_t** out_pte) {
	struct task_struct* task = pid_task(proc_pid_struct, PIDTYPE_PID);
	struct mm_struct *mm = NULL;
	//////////////////////////////////////////////////////////////////////////
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	unsigned long paddr = 0;
	unsigned long page_addr = 0;
	unsigned long page_offset = 0;
	bool is_pmd_leaf = false;

	/* 必须最先初始化：下面有多条提前 return 的路径（task/mm 为空），
	 * 调用方拿到未定义的 out_pte 后一旦使用就是野指针。 */
	*out_pte = NULL;

	if (!task) { return 0; }

	mm = get_task_mm(task);
	if (!mm) { return 0; }
	pgd = x_pgd_offset(mm, virt_addr);
	if (pgd == NULL) {
		printk_debug("pgd is null\n");
		goto out;
	}
	//printk_debug("pgd_val = 0x%lx pgd addr:0x%p\n", (unsigned long int)pgd_val(*pgd), (void*)pgd);
	//printk_debug("init_mm pgd val:0x%lx,pgd addr:0x%p\n", (unsigned long)pgd_val(*(mm->pgd)), (void*)mm->pgd);
	printk_debug("pgd_index = %zu\n", pgd_index(virt_addr));
	if (pgd_none(*pgd)) {
		printk_debug("not mapped in pgd\n");
		goto out;
	}
	printk_debug("pgd_offset ok\n");

	/*
	 * (p4ds are folded into pgds so this doesn't get actually called,
	 * but the define is needed for a generic inline function.)
	 */
	p4d = p4d_offset(pgd, virt_addr);
	//printk_debug("p4d_val = 0x%llx, p4d_index = %d\n", p4d_val(*p4d), p4d_index(virt_addr));
	printk_debug("p4d_val = 0x%llx\n", p4d_val(*p4d));
	if (p4d_none(*p4d))
	{
		printk_debug("not mapped in p4d\n");
		goto out;
	}

	pud = pud_offset(p4d, virt_addr);
	printk_debug("pud_val = 0x%llx \n", pud_val(*pud));
	if (pud_none(*pud)) {
		printk_debug("not mapped in pud\n");
		goto out;
	}
	printk_debug("pud_offset ok\n");

	pmd = pmd_offset(pud, virt_addr);
	printk_debug("pmd_val = 0x%llx\n", pmd_val(*pmd));
	//printk_debug("pmd_index = %d\n", pmd_index(virt_addr));
	if (pmd_none(*pmd)) {
		printk_debug("not mapped in pmd\n");
		goto out;
	}
	printk_debug("pmd_offset ok\n");

	/* PMD 级大页（THP / devmap）：此时 *pmd 指向**页本身**而不是页表页。
	 * pte_offset_kernel() 会把 pmd 的值当页表地址用 —— arm64 的 pmd 里带
	 * AF/SH/AttrIndx 等属性位，当指针解引用必然落到非法地址。
	 * 这是「全量扫描 anon 区域导致整机重启」的第二个根因：THP 化的堆/栈
	 * 会被扫到，而 pid 1 这类小 footprint 进程很少触发。
	 * 处理：按大页（PMD_SIZE）算物理地址；*out_pte 保持 NULL —— 大页没有
	 * PTE，调用方的 force 改权限路径会因为 !pte 而放弃，不会写错页表。 */
#if MY_LINUX_VERSION_CODE >= KERNEL_VERSION(5,4,0)
	is_pmd_leaf = pmd_leaf(*pmd);
#else
	is_pmd_leaf = pmd_trans_huge(*pmd);
#endif
	if (is_pmd_leaf) {
		page_addr = page_to_phys(pmd_page(*pmd));
		page_offset = virt_addr & (PMD_SIZE - 1);
		paddr = page_addr + page_offset;
		printk_debug("pmd leaf: page=0x%lx off=0x%lx paddr=0x%lx\n",
		             page_addr, page_offset, paddr);
		goto out;
	}

	pte = pte_offset_kernel(pmd, virt_addr);
	printk_debug("pte_val = 0x%llx\n", pte_val(*pte));
	//printk_debug("pte_index = %d\n", pte_index(virt_addr));
	if (pte_none(*pte)) {
		printk_debug("not mapped in pte\n");
		goto out;
	}
	/* pte_none 只说明「这一项没填」，不等于「这一项可用」：
	 *  - !pte_present：页已被换出/为非线性映射，pte_pfn 指向的不是 RAM；
	 *  - pte_special：PAGE_SPECIAL 映射（[vvar]、MMIO、驱动 remap 等），
	 *    背后没有合法的 struct page，pte_page() 拿到的是垃圾指针，
	 *    page_to_phys() 之后就是一次越界访问。
	 * 这两类在 sleep 这类进程的 maps 里都存在，全量扫描必然踩到。 */
	if (!pte_present(*pte)) {
		printk_debug("pte not present\n");
		goto out;
	}
#ifdef PTE_SPECIAL
	if (pte_special(*pte)) {
		printk_debug("pte is special mapping\n");
		goto out;
	}
#endif
	printk_debug("pte_offset_kernel ok\n");

	page_addr = page_to_phys(pte_page(*pte));
	page_offset = virt_addr & ~PAGE_MASK;
	paddr = page_addr | page_offset;

	printk_debug("page_addr = %lx, page_offset = %lx\n", page_addr, page_offset);
	printk_debug("vaddr = %zx, paddr = %lx\n", virt_addr, paddr);

	*out_pte = pte;
out:
	mmput(mm);
	return paddr;
}


static inline int is_pte_can_read(pte_t* pte) {
						if (!pte) { return 0; }
#ifdef pte_read
						if (pte_read(*pte)) { return 1; } else { return 0; }
#endif
						return 1;
					}
static inline int is_pte_can_write(pte_t* pte) {
	if (!pte) { return 0; }
	if (pte_write(*pte)) { return 1; } else { return 0; }
}
static inline int is_pte_can_exec(pte_t* pte) {
	if (!pte) { return 0; }
#ifdef pte_exec
	if (pte_exec(*pte)) { return 1; } else { return 0; }
#endif
#ifdef pte_user_exec
	if (pte_user_exec(*pte)) { return 1; } else { return 0; }
#endif
	return 0;
}
static inline int change_pte_read_status(pte_t* pte, bool can_read) {
	if (!pte) { return 0; }
	return 1;
}
static inline int change_pte_write_status(pte_t* pte, bool can_write) {
	if (!pte) { return 0; }
	if (can_write) {
		set_pte(pte, x_pte_mkwrite(*pte));
	} else {
		set_pte(pte, pte_wrprotect(*pte));
	}
	return 1;
}
static inline int change_pte_exec_status(pte_t* pte, bool can_exec) {
	if (!pte) { return 0; }
	if (can_exec) {
#ifdef pte_mknexec
		set_pte(pte, x_pte_mkwrite(*pte));
#endif
	} else {
#ifdef pte_mkexec
		set_pte(pte, x_pte_mkwrite(*pte));
#endif
	}
	return 1;
}

static inline unsigned long size_inside_page(unsigned long start,
	unsigned long size) {
	unsigned long sz;

	sz = PAGE_SIZE - (start & (PAGE_SIZE - 1));

	return min(sz, size);
}


static inline int check_phys_addr_valid_range(size_t addr, size_t count) {
	if (count == 0) {
		return 0;
	}
	if (g_phy_total_memory_size == 0) {
		init_phy_total_memory_size();
	}
	/* 整数回绕：addr + count - 1 溢出说明区间本身非法，先挡住，
	 * 否则下面任何比较都会被回绕成一个小数而误判。 */
	if (addr > (size_t)-1 - (count - 1)) {
		return 0;
	}
	/* 【2026-10-22 真机修正】**不再**用 g_phy_total_memory_size 当上界。
	 *
	 * 原先写成 `addr + count <= totalram` 是错的：totalram 是内核管理的 RAM
	 * 页总量（本机 11.6GB），而**物理地址空间远大于它** —— ARM64 的 PA_BITS=48，
	 * RAM 只是其中若干 bank。真机实测：agentd 一个普通 .data 页的 pfn 是
	 * 0x926afc（39.3GB），内核 /proc/<pid>/pagemap 也报同一个值，说明这是
	 * 合法物理地址。用 totalram 做上界会把它误拦，表现为「读写全挂但设备不崩」。
	 *
	 * 真正的合法性判据只该是「这个 pfn 背后有没有 struct page」：
	 *   - 快路径：fast_read_paddr 里的 virt_addr_valid()（内部即 pfn_valid）
	 *   - 慢路径：xlate_dev_mem_ptr() 对非 RAM 返回 NULL
	 * 这里只做回绕检查，把判定权交还给那两个精确判据。 */
	return 1;
}


static inline size_t read_ram_physical_addr(bool is_kernel_buf, size_t phy_addr, char* lpBuf, size_t read_size) {
	/* ── 【AFEI 移植】fast 路径开关 ──────────────────────────────────────
	 * 若开启 CONFIG_ENABLE_FAST_PHYMEM，直接走 __va + kmap_local_page
	 * 的最短路径（phy_mem_fast.h），省掉 xlate_dev_mem_ptr + bounce 中转。
	 * 本函数签名/返回值与慢路径完全一致，调用点无需改动。
	 * ──────────────────────────────────────────────────────────────── */
#ifdef CONFIG_ENABLE_FAST_PHYMEM
	return fast_read_paddr(phy_addr, lpBuf, read_size, !is_kernel_buf);
#else
	void *bounce;
	size_t realRead = 0;
	if (!check_phys_addr_valid_range(phy_addr, read_size)) {
		printk_debug(KERN_INFO "Error in check_phys_addr_valid_range:%zu,size:%zu\n", phy_addr, read_size);
		return 0;
	}
	bounce = x_kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!bounce) {
		return 0;
	}

	while (read_size > 0) {
		size_t sz = size_inside_page(phy_addr, read_size);

		/*
		 * On ia64 if a page has been mapped somewhere as uncached, then
		 * it must also be accessed uncached by the kernel or data
		 * corruption may occur.
		 */

		char *ptr = xlate_dev_mem_ptr(phy_addr);
		int probe;

		if (!ptr) {
			printk_debug(KERN_INFO "Error in x_xlate_dev_mem_ptr:0x%zx\n", phy_addr);
			break;
		}
		probe = x_probe_kernel_read(bounce, ptr, sz);
		unxlate_dev_mem_ptr(phy_addr, ptr);
		if (probe) {
			break;
		}
		if (is_kernel_buf) {
			memcpy(lpBuf, bounce, sz);
		} else {
			unsigned long remaining = x_copy_to_user(lpBuf, bounce, sz);
			if (remaining) {
				printk_debug(KERN_INFO "Error in x_copy_to_user(\n");
				break;
			}
		}
		lpBuf += sz;
		phy_addr += sz;
		read_size -= sz;
		realRead += sz;
	}
	kfree(bounce);
	return realRead;
#endif /* !CONFIG_ENABLE_FAST_PHYMEM */
}

static inline size_t write_ram_physical_addr(size_t phy_addr, char* lpBuf, bool is_kernel_buf, size_t write_size) {
	/* 与读路径同样的开关：fast 走 __va + kmap_local_page 直写。 */
#ifdef CONFIG_ENABLE_FAST_PHYMEM
	return fast_write_paddr(phy_addr, lpBuf, write_size, !is_kernel_buf);
#else
	size_t realWrite = 0;
	if (!check_phys_addr_valid_range(phy_addr, write_size)) {
		printk_debug(KERN_INFO "Error in check_phys_addr_valid_range:0x%zx,size:%zu\n", phy_addr, write_size);
		return 0;
	}


	while (write_size > 0) {
		size_t sz = size_inside_page(phy_addr, write_size);

		/*
			* On ia64 if a page has been mapped somewhere as uncached, then
			* it must also be accessed uncached by the kernel or data
			* corruption may occur.
			*/

		char *ptr = xlate_dev_mem_ptr(phy_addr);
		if (!ptr) {
			printk_debug(KERN_INFO "Error in xlate_dev_mem_ptr:0x%zx\n", phy_addr);
			break;
		}
		if (is_kernel_buf) {
			memcpy(ptr, lpBuf, sz);
		} else {
			unsigned long copied = x_copy_from_user(ptr, lpBuf, sz);
			if (copied) {
				unxlate_dev_mem_ptr(phy_addr, ptr);
				realWrite += sz - copied;
				printk_debug(KERN_INFO "Error in x_copy_from_user(\n");
				break;
			}
		}
		unxlate_dev_mem_ptr(phy_addr, ptr);

		lpBuf += sz;
		phy_addr += sz;
		write_size -= sz;
		realWrite += sz;
	}
	return realWrite;
#endif /* !CONFIG_ENABLE_FAST_PHYMEM */
}
#endif /* PHY_MEM_H_ */