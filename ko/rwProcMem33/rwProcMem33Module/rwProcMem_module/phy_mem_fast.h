#ifndef PHY_MEM_FAST_H_
#define PHY_MEM_FAST_H_
/* ============================================================================
 * phy_mem_fast.h —— 物理内存直读/直写（AFEI _fast 变体移植）
 *
 * ── 来源与依据 ────────────────────────────────────────────────────────────
 * 逆向样本：阿飞2.01 / assets/same-sdk36-6.12.ko
 *   do_read_physical_memory       @ .text+0x2eec
 *   do_read_physical_memory_fast  @ .text+0x3e98
 *   do_write_physical_memory      @ .text+0x37a0
 *   do_write_physical_memory_fast @ .text+0x45d4
 *   swap_read_page_direct         @ .text+0x3494   ← 核心：vmemmap 直取 struct page
 *   swap_write_page_direct        @ .text+0x3bec
 *   lockfree_va_to_pa             @ .text+0x338c
 *   vm_rw                         @ .text+0x41b4
 *   allocate_physical_page_info   @ .text+0x403c
 *   外部符号：mem_section / vmemmap / memstart_addr / page_offset_base
 *
 * ── AFEI swap_read_page_direct 反汇编还原（0x3494，逐步验证）───────────────
 *   入参（x0..x3）= (unsigned long pfn_or_paddr?, char *dst, size_t offset, size_t len)
 *   实际语义（按寄存器用法推断）：
 *     参数1 x0 = 该 struct page 所在的 vmemmap 索引基数（用于反算 pfn）
 *     参数2 x1 = 目标缓冲
 *     参数3 x2 = 页内偏移
 *     参数4 x3 = 长度
 *
 *   前段校验：
 *     0x34b0:  add x8, x3, x2 ;  cmp x8, #0x1000 (立即数)  →  x3+x2 必须 <= 4096
 *              （即"偏移+长度不能跨页"，与 rwProcMem 的 size_inside_page 同义）
 *     0x34c0:  adrp/ldr 取一个全局指针（= vmemmap / mem_section 基址），空则失败
 *     0x34cc:  lsr x8, x0, #58        ← 取虚拟地址高 6 位！这是 **mem_section 索引**
 *              0x34d0: cmp w8, #0x1b   （27 = NR_SECTION_ROOTS-1 上限检查）
 *              0x34d8: ldr x22, [x9, x8, lsl #3]  ← mem_section[idx] → struct mem_section*
 *              ⇒ 证实：x0 是 **内核虚拟地址**，
 *                mem_section[addr >> 58] 得到所属 section
 *     0x34e0:  ldr x8, [x22, #0x220]  ← section->section_mem_map（偏移 0x220）
 *              空则失败
 *
 *   中段取页：
 *     0x34fc:  mov w0,#0xc00 (3072=GFP_KERNEL?) ; w1=0 ; w2=0 ; x3=0
 *              0x350c: bl → __alloc_pages_noprof（分配一个临时 page 做中转）
 *     0x3518:  ldr x0, [x22, #0x220]   ← 再次取 section_mem_map
 *              0x351c: adrp/add x4 ← 一个静态表
 *              mov w1,#1 ; mov w2,#0x800 (2048) ; mov w3,#0xc00
 *              0x3530: bl → 某 helper（按 section_mem_map 反查 struct page*）
 *              ⇒ 返回 x0 = 目标 pfn 对应的 struct page*
 *     0x3538:  ubfiz x8, x23, #3, #58  ← (原x0 << 3) & ((1<<58)-1)
 *              0x354c: str x8, [x0, #0x20]  ← 写回 page->... （0x20 偏移）
 *              0x3550: bl → 又一次内核调用（可能是 kmap 或 flush）
 *     0x3588:  kmap_atomic(x19)          ← ★把页映射进内核线性区
 *              0x3590: add x1, x0, x24    ← page 虚拟地址 + 页内偏移
 *              0x3594-0x359c: memcpy(x21, x1, x20)  ← 拷到用户缓冲
 *              0x35a0: __kunmap_atomic(x19)
 *
 * ── 与 rwProcMem 现有 read_ram_physical_addr 的差异（关键）────────────────
 *   现有实现（phy_mem.h:191）：
 *       bounce = kmalloc(PAGE_SIZE)
 *       ptr = xlate_dev_mem_ptr(phy_addr)     ← 依赖 ioremap/is_vmalloc 判断
 *       probe_kernel_read(bounce, ptr, sz)    ← 6.12 上已退化成 memcpy
 *       unxlate_dev_mem_ptr(...)
 *     → 每一步都有函数调用开销，且 xlate_dev_mem_ptr 对
 *       ioremap 区域特殊处理，实测在无高内存区时路径更长。
 *
 *   AFEI _fast 路径：
 *       直接由 kern 虚拟地址 → mem_section → struct page → kmap_atomic
 *     → 页内直接 memcpy。
 *     ★省掉 xlate_dev_mem_ptr / probe_kernel_read 两层间接，
 *       且 kmap_atomic 在 64 位只在需要时才真做事（HIGHMEM 通常关闭），
 *       实质等价于一次地址换算 + memcpy。
 *
 * ── 移植策略 ──────────────────────────────────────────────────────────────
 *   ★AFEI 反汇编里的 0x220 / 0xc00 / 0x800 等是**内核结构体偏移常量**，
 *     随内核版本变化（尤其 mem_section->section_mem_map 偏移）。
 *     直接照抄数字会在别的内核上错。因此本移植**不硬编码偏移**，
 *     改用内核**公开 API** 实现同样的"VA→PA→page→memcpy"最短路径：
 *
 *         virt_to_page()  / pfn_to_page()   ← 公开，跨版本稳定
 *         kmap_local_page() / kmap_atomic() ← 公开
 *         page_to_phys()                     ← 公开
 *         memcpy()                           ← 直接
 *
 *     这样既保留"少一层间接"的性能收益，
 *     又不会像硬编码偏移那样炸在别的内核上。
 *   ★对外接口与现有 read_ram_physical_addr 完全一致，可原地替换/并存。
 * ==========================================================================*/

#include "ver_control.h"

#ifdef CONFIG_ENABLE_FAST_PHYMEM

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/page.h>
#include <asm/memory.h>   /* virt_addr_valid()：判断线性地址背后是否有合法 struct page */

#include "api_proxy.h"
#include "phy_mem.h"

/* --------------------------------------------------------------------------
 * 【多版本兼容】page 临时映射 API
 *
 * kmap_local_page() / kunmap_local() 是 **5.11** 才引入的
 *   （commit f3ba3c710ac5 "mm/highmem: Provide kmap_local*"）。
 * 在那之前只有 kmap_atomic() / kunmap_atomic()。
 *   ★若不封装，5.10 内核上直接报 "call to undeclared function
 *     'kmap_local_page'"（本移植实测过同类报错）。
 *
 * 语义差异（对本用途无影响）：
 *   kmap_atomic 额外禁用缺页/抢占；本路径只是"映射一页 → memcpy → 解除"，
 *   不依赖这些副作用，故两者可互换。
 *   ★kunmap_atomic 的参数是 kmap 返回的地址（不是 struct page*），
 *     与 kunmap_local 一致，所以封装里统一传 kaddr。
 *
 * 统一封装成 rw_kmap_page() / rw_kunmap_page()，上游不关心版本差异。
 * ------------------------------------------------------------------------*/
#if MY_LINUX_VERSION_CODE >= KERNEL_VERSION(5,11,0)
static inline void *rw_kmap_page(struct page *page)
{
	return kmap_local_page(page);
}
static inline void rw_kunmap_page(void *kaddr)
{
	kunmap_local(kaddr);
}
#else
/* 5.11 之前：仅有 kmap_atomic 系列。
 * ★注意 kmap_atomic 在旧内核是宏，可能展开为 __kmap_atomic，
 *   两种写法都由 <linux/highmem.h> 提供，这里直接用公开名即可。 */
static inline void *rw_kmap_page(struct page *page)
{
	return kmap_atomic(page);
}
static inline void rw_kunmap_page(void *kaddr)
{
	kunmap_atomic(kaddr);
}
#endif

/* --------------------------------------------------------------------------
 * fast_read_va —— 直读：内核虚拟地址（物理页线性映射区）→ 用户/内核缓冲
 *
 * 参数：
 *   kvaddr    内核虚拟地址（已经由 get_proc_phy_addr 换算得到物理地址后，
 *             这里再经 phys_to_virt 拿到线性映射地址；或直接传线性地址）
 *   dst       目标缓冲
 *   size      长度
 *   to_user   目标是否为用户地址（true 走 copy_to_user）
 *
 * 返回实际拷贝字节数。
 *
 * ★与 AFEI do_read_physical_memory_fast 等价的最短路径：
 *   线性映射区地址 → virt_to_page → kmap_local_page → memcpy
 *   对 lowmem（ARM64 常态）而言 kmap_local_page 是恒等变换，
 *   编译器/内核内联后只剩一次减法 + memcpy。
 * ------------------------------------------------------------------------*/
static inline size_t fast_read_linear(void *kvaddr, void *dst, size_t size, bool to_user)
{
	size_t done = 0;

	while (done < size) {
		unsigned long cur = (unsigned long)kvaddr + done;
		struct page *page;
		size_t page_off;
		size_t chunk;
		void *kaddr;

		/* 【根因修复 2026-10-22】__va() 只做一次线性换算，**不校验**结果背后
		 * 有没有 struct page。phy_addr 跨页前进时只要有一页不是 RAM
		 * （MMIO 空洞 / reserved / pfn > max_pfn），后续 kmap 就会碰到
		 * vmemmap 之外的结构体 → synchronous external abort → 整机重启
		 * （无 tombstone、pstore 空）。这是「全量搜索导致设备重启」的触发点。
		 *
		 * ⚠️ 判据必须用 `virt_to_pfn()`，**不能**写 `PHYS_PFN(cur - PAGE_OFFSET)`
		 * （2026-10-23 真机实测纠正，代价是查了两轮）：
		 *   arm64 线性映射是  __va(p) = p - memstart_addr + PAGE_OFFSET，
		 *   所以  cur - PAGE_OFFSET = p - memstart_addr，**不是 p**。
		 *   本机 memstart_addr ≈ 2GB（0x80000 页），实测同一地址：
		 *       PHYS_PFN(cur - PAGE_OFFSET) = 0xa6238e   ← 少了 2GB
		 *       virt_to_pfn(cur)            = 0xae238e   ← 正确
		 *   后果：paddr 合法但算出的 pfn 落进 MMIO 空洞时被误拦 → 读返回 0，
		 *   表现为「driver self-test 读自身时好时坏」「部分页静默读不到」。
		 *   virt_to_pfn 内部走 __virt_to_phys，已扣除 memstart_addr。 */
		if (!pfn_to_online_page(virt_to_pfn((void *)cur)))
			break;

		page = virt_to_page((void *)cur);
		page_off = offset_in_page(cur);
		chunk = min(size - done, PAGE_SIZE - page_off);

		kaddr = rw_kmap_page(page);
		if (!kaddr)
			break;


		if (to_user) {
			if (x_copy_to_user((char *)dst + done,
			                   (char *)kaddr + page_off, chunk)) {
				rw_kunmap_page(kaddr);
				break;
			}
		} else {
			memcpy((char *)dst + done, (char *)kaddr + page_off, chunk);
		}

		rw_kunmap_page(kaddr);
		done += chunk;
	}
	return done;
}

/* --------------------------------------------------------------------------
 * fast_write_linear —— 直写（同上，反向）
 * 对应 AFEI swap_write_page_direct @0x3bec
 * ------------------------------------------------------------------------*/
static inline size_t fast_write_linear(void *kvaddr, const void *src,
                                       size_t size, bool from_user)
{
	size_t done = 0;

	while (done < size) {
		unsigned long cur = (unsigned long)kvaddr + done;
		struct page *page;
		size_t page_off;
		size_t chunk;
		void *kaddr;

		/* 与读路径同一条守卫（细节见读路径那段 memstart_addr 说明）。 */
		if (!pfn_to_online_page(virt_to_pfn((void *)cur)))
			break;

		page = virt_to_page((void *)cur);
		page_off = offset_in_page(cur);
		chunk = min(size - done, PAGE_SIZE - page_off);

		/*
		 * ★写路径必须先确认页可写。
		 * AFEI 用 lockfree_va_to_pa 自行处理；本移植走内核公开方式，
		 * 由调用方（get_proc_phy_addr + change_pte_write_status）
		 * 保证页表可写，这里不再重复检查。
		 */
		kaddr = rw_kmap_page(page);
		if (!kaddr)
			break;

		if (from_user) {
			if (x_copy_from_user((char *)kaddr + page_off,
			                     (const char *)src + done, chunk)) {
				rw_kunmap_page(kaddr);
				break;
			}
		} else {
			memcpy((char *)kaddr + page_off, (const char *)src + done, chunk);
		}

		rw_kunmap_page(kaddr);
		done += chunk;
	}
	return done;
}

/* --------------------------------------------------------------------------
 * fast_read_paddr —— AFEI do_read_physical_memory_fast 对外的等价接口
 *
 * 语义：已拿到物理地址 phy_addr，但**不经过** xlate_dev_mem_ptr，
 *       直接用 __va() 拿到线性映射地址再 fast_read_linear。
 *
 * ★注意 __va() 只在 lowmem 线性区有效。若 phy_addr 超出线性映射范围
 *   （HIGHMEM/高于 memstart 的部分），退回慢路径更安全 ——
 *   这正是 AFEI 里那个 "check_phys_addr_valid_range" 检查的意义。
 * ------------------------------------------------------------------------*/
static inline size_t fast_read_paddr(size_t phy_addr, void *dst, size_t size, bool to_user)
{
	size_t realRead = 0;

	/* 保留 AFEI 的物理地址合法性检查 */
	if (!check_phys_addr_valid_range(phy_addr, size)) {
		printk_debug(KERN_INFO "[rwfast] phys range invalid: %zu\n", phy_addr);
		return 0;
	}

	/* 分页循环：与 AFEI swap_*_page_direct 的 "偏移+长度不跨页" 校验对应 */
	while (realRead < size) {
		size_t sz = size_inside_page(phy_addr, size - realRead);
		void *kv = (void *)__va(phy_addr);

		{
			size_t n = fast_read_linear(kv, (char *)dst + realRead, sz, to_user);
			realRead += n;
			if (n < sz)
				break;
		}
		phy_addr += sz;
	}
	return realRead;
}

static inline size_t fast_write_paddr(size_t phy_addr, const void *src,
                                      size_t size, bool from_user)
{
	size_t realWrite = 0;

	if (!check_phys_addr_valid_range(phy_addr, size)) {
		printk_debug(KERN_INFO "[rwfast] phys range invalid: %zu\n", phy_addr);
		return 0;
	}

	while (realWrite < size) {
		size_t sz = size_inside_page(phy_addr, size - realWrite);
		void *kv = (void *)__va(phy_addr);

		{
			size_t n = fast_write_linear(kv, (const char *)src + realWrite, sz, from_user);
			realWrite += n;
			if (n < sz)
				break;
		}
		phy_addr += sz;
	}
	return realWrite;
}

#endif /* CONFIG_ENABLE_FAST_PHYMEM */
#endif /* PHY_MEM_FAST_H_ */
