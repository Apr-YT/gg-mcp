#ifndef CFI_BYPASS_H_
#define CFI_BYPASS_H_
/* ============================================================================
 * cfi_bypass.h —— 运行时内核 CFI 检查函数绕过（AFEI same/paradise 同款手法移植）
 *
 * ── 来源与依据 ────────────────────────────────────────────────────────────
 * 逆向样本：阿飞2.01 / assets/same-sdk36-6.12.ko
 *   函数 cfi_bypass @ .text+0x156c（0x156c..0x1c24，约 1.7KB）
 *   配套 disable_kprobe_blacklist @ .text+0x0da0
 *   写代码：hook_write_range  @ .text+0x800c
 *           check_target_can_hijack @ .text+0x8098
 *   解析符号：init_arch      @ .text+0x7f9c
 *   日志串 "[same] cfi_bypass patched: %d"
 *   调用点：init_module（.init.text）里，紧跟 init_arch 之后、supercalls 之前
 *
 * ── AFEI 原始实现（反汇编还原，已逐条重定位验证）────────────────────────────
 *
 *   [1] init_arch() —— 解析两个**未导出**符号并存入 .bss 指针
 *       aarch64_insn_write_ptr = kallsyms_lookup_name_ex("aarch64_insn_write")
 *       flush_icache_range_ptr = kallsyms_lookup_name_ex("caches_clean_inval_pou")
 *       if (!flush_icache_range_ptr)
 *           flush_icache_range_ptr = kallsyms_lookup_name_ex("__flush_icache_range")
 *       return (aarch64_insn_write_ptr == 0);   // 非0 = 失败
 *
 *       ★★ 2026-10-10 真机实测纠正（重定位表为准，非字符串列表）：
 *         三次 kallsyms_lookup_name_ex 调用的实参依次是
 *           .rodata+0x5d7 = "aarch64_insn_write"
 *           .rodata+0x735 = "caches_clean_inval_pou"   ← 主选
 *           .rodata+0xb7d = "__flush_icache_range"     ← 备选
 *         ※ 之前从 .rodata 字符串列表推断"解析 flush_icache_range"是**错的**：
 *           那个名字来自变量名 flush_icache_range_ptr，不是被解析的符号。
 *         ※ 为什么是 caches_clean_inval_pou：
 *           arm64 的 flush_icache_range() 是 static inline，内联后转调的
 *           底层实体就是 caches_clean_inval_pou()（POU=Point of Unification，
 *           把 D-cache 与 I-cache 同步到统一点）。二者签名完全相同
 *           (void (unsigned long start, unsigned long end))，语义等价。
 *         ※ 真机验证（2026-10-10）：
 *             真机 kallsyms 里 **没有** flush_icache_range / __flush_icache_range
 *             （inline 函数没有独立符号），但 **有** caches_clean_inval_pou。
 *             实测：只解析 flush_icache_range 时 arch syms not ready → 跳过。
 *       ★证据：.rodata 中确有 aarch64_insn_write / caches_clean_inval_pou /
 *         __flush_icache_range 三个字符串；两个 _ptr 变量位于 .bss。
 *       ★5.10 版本 init_arch @0x9614 改用 register_kprobe("kallsyms_lookup_name")
 *         先拿到 kallsyms 函数本身，再查上述符号（因 5.7+ 不再导出该函数）。
 *
 *   [2] hook_write_range(addr, src_words, len_bytes) —— 真正写 text
 *       循环: 逐 4 字节 (*aarch64_insn_write_ptr)(addr + i, src[i]);   // blr x9
 *       收尾: (*flush_icache_range_ptr)(addr, addr + len);            // blr x8
 *       ★关键：**不用 set_memory_rw / set_memory_ro**！
 *         aarch64_insn_write() 内部用 patch_map()（fixmap）自行把 text 页
 *         临时映射为可写，并持 patch_lock 自旋锁保证原子性。
 *         见 arch/arm64/kernel/patching.c: __aarch64_insn_write()。
 *
 *   [3] cfi_bypass() 的循环体：
 *       1) 取目标函数地址 p（先经 kallsyms_lookup_name_ex 解析；已有缓存则用缓存）
 *       2) 检查 *(u32*)p 是否已等于补丁值 → 是则跳过（幂等，"already patched"）
 *       3) 否则把 8 字节指令序列经 hook_write_range 写入 p：
 *              MOVZ X0, #1   = 0xd2800020   (小端 20 00 80 d2)
 *              RET           = 0xd65f03c0   (小端 c0 03 5f d6)
 *          即把函数体改成 "return 1"，使 CFI 检查永远"通过"
 *       4) 每次写完计数 +1，最终 printk "[same] cfi_bypass patched: %d"
 *
 *   注意这 8 字节是**两条独立指令**，不是一条 64 位指令——
 *   我最初按单条解码时把 0xd2800020 误读成 "MOVZ X0,#0"，
 *   实际 imm16=(w>>5)&0xffff=1 ⇒ **return 1**（"通过"语义）。
 *   这个 1 很关键：CFI 检查函数返回 1/非负表示"校验通过"。
 *
 *   目标函数清单（从 .rodata 字符串 + 符号引用还原，7 条全部命中）：
 *     __cfi_check_fail / __cfi_slowpath / _cfi_slowpath / __cfi_slowpath_diag
 *     / __ubsan_handle_cfi_check_fail / __ubsan_handle_cfi_check_fail_abort
 *     / report_cfi_failure
 *   另有独立函数 disable_kprobe_blacklist()：
 *     取 kprobe_blacklist（__start_kprobe_blacklist .. __end_kprobe_blacklist），
 *     把每条 entry 的 name 清空，日志 "Disabled %d kprobe blacklist entries"
 *
 * ── 为什么需要它（你的 6.12 设备同理）──────────────────────────────────────
 *   设备开 CONFIG_CFI_CLANG=y 后，内核所有间接调用点会插入 __cfi_check()，
 *   调用到不匹配的函数原型就触发 __cfi_check_fail →
 *   __ubsan_handle_cfi_check_fail_abort → 直接 panic/死机。
 *   本模块大量使用函数指针（proc_ops、回调）以及（若启用）inline hook，
 *   一旦某处签名不匹配就会被 CFI 拦下。
 *   把上述"失败处理函数"改成 return 1，等于让 CFI 检查**永不上报**，
 *   从而消除死机点。这与 rwProcMem_module.c 里那段被注释掉的
 *   __check_/_check_fail_（编译期 hook 思路）相比，是**运行期 patch**，
 *   不需要编译期符号，对所有内核版本统一生效。
 *
 * ── ★★ 真机可行性（本轮重大修正，务必先读）────────────────────────────────
 *   最初版本用 set_memory_rw() / set_memory_ro() 解 text 只读，
 *   用 flush_icache_range() 同步 icache —— **这条路真机必失败**：
 *
 *   权威证据 A（设备真实导出符号表 device_symvers.txt，4409 条）：
 *       set_memory_rw        → 不存在
 *       set_memory_ro        → 不存在
 *       caches_clean_inval_pou（flush_icache_range 的底层）→ 不存在
 *   权威证据 B（AFEI 六个版本 ko 的 llvm-nm -u 依赖）：
 *       set_memory_rw/ro、flush_icache_range 六版本**一个都没用**。
 *   旁证：2015-03 起 arm64 官方就 unexport 了 set_memory_ro/rw，
 *         理由明确写"不允许模块改内核代码"。
 *
 *   ⇒ 正解即 [1][2]：用 aarch64_insn_write()（fixmap 自解只读）+ 
 *     caches_clean_inval_pou（主选，= arm64 flush_icache_range 的内联实体）
 *     / __flush_icache_range（备选 1）/ flush_icache_range（备选 2），
 *     三者均运行期 kallsyms 解析（未导出符号）。
 *     本文件已按此重写。
 *
 * ── ★★★ 2026-10-10 真机实测结论（第一手，优先级最高）────────────────────
 *   设备：25102RKBEC / myron / Android16 / 6.12.23-android16-5-...-4k
 *   环境：KernelSU + SELinux Enforcing + CONFIG_EXTENDED_MODVERSIONS=y
 *         + CONFIG_CFI_CLANG=y + CONFIG_STRICT_MODULE_RWX=y
 *         + CONFIG_MODULE_SIG_FORCE **未开**（⇒ 无需签名即可 insmod）
 *
 *   ① 模块已成功 insmod（CRC 补丁后 RC=0），/proc/<KEY>/ 节点建立成功。
 *   ② CFI 补丁第一版**被跳过**，日志：
 *        [rwcfi] arch syms not ready: write=ffffffda7cd98c14 flush=0000...
 *      ⇒ aarch64_insn_write 解析成功（地址合法），但 flush 为 0。
 *   ③ 根因：真机 kallsyms 里**没有** flush_icache_range / __flush_icache_range
 *      （它们是 static inline，不产生独立符号）。
 *      真机**有**的等价符号是 caches_clean_inval_pou。
 *   ④ 已改：解析顺序改为 caches_clean_inval_pou → __flush_icache_range
 *      → flush_icache_range（三级兜底）。
 * ==========================================================================*/

#include "ver_control.h"

#ifdef CONFIG_ENABLE_CFI_BYPASS

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/mm.h>

/* ★不再需要 <linux/set_memory.h> / <asm/cacheflush.h>：
 *   - 只读解除由 aarch64_insn_write() 内部处理（fixmap）
 *   - icache 同步走运行期解析的函数指针
 * 保留 <asm/io.h> 仅为 memcpy 等常规声明。 */
#include <asm/io.h>

/* ★rw_sym_lookup() 的来源。必须在本文件使用前引入。 */
#include "rw_symbol_lookup.h"

/* --------------------------------------------------------------------------
 * 待 patch 的 CFI 失败处理函数名清单
 * 顺序无所谓；解析不到的直接跳过。
 * ------------------------------------------------------------------------*/
static const char *const g_cfi_target_names[] = {
	"__cfi_check_fail",
	"__cfi_slowpath",
	"_cfi_slowpath",              /* 无前缀别名（部分内核实为此名） */
	"__cfi_slowpath_diag",
	"__ubsan_handle_cfi_check_fail",
	"__ubsan_handle_cfi_check_fail_abort",
	"report_cfi_failure",
	NULL
};

/* AFEI 写入的那 8 字节：MOVZ X0,#1 ; RET */
#define CFI_STUB_MOV_X0_1   0xd2800020u
#define CFI_STUB_RET        0xd65f03c0u

/* --------------------------------------------------------------------------
 * [1] 运行期解析"写 text"所需的两个未导出符号（对应 AFEI init_arch @0x7f9c）
 *
 *   aarch64_insn_write     : int (void *addr, u32 insn)
 *   caches_clean_inval_pou : void (unsigned long start, unsigned long end)
 *     —— arm64 的 flush_icache_range() 是 static inline，其内联体最终转调
 *        本函数。真机 kallsyms 里**只有它**，没有 flush_icache_range。
 *     备选 __flush_icache_range（旧内核实名，见 LKML patch v2/v4/v5）
 *
 *   ★都是**未导出符号**，编译期拿不到，只能 rw_sym_lookup()。
 *   ★解析失败不致命：cfi_bypass() 会整体跳过并返回 -1。
 *
 *   ★★ 2026-10-10 真机实测（务必读）：
 *      在本机 6.12.23-android16 上，device_symvers / kallsyms 里
 *        caches_clean_inval_pou  → **存在**
 *        flush_icache_range      → **不存在**（inline，无独立符号）
 *        __flush_icache_range    → **不存在**
 *      所以解析顺序必须是 caches_clean_inval_pou 优先。
 *      实测日志（错版）："[rwcfi] arch syms not ready: write=ffff... flush=0"
 *      —— write 解析成功、flush 为 0，导致整个 cfi_bypass 被跳过。
 * ------------------------------------------------------------------------*/
typedef int  (*rw_insn_write_fn)(void *addr, u32 insn);
typedef void (*rw_flush_icache_fn)(unsigned long start, unsigned long end);

static rw_insn_write_fn    g_insn_write;
static rw_flush_icache_fn  g_flush_icache;

/* 解析一次，结果缓存（幂等，可重复调用）*/
static int cfi_resolve_arch_syms(void)
{
	if (g_insn_write && g_flush_icache)
		return 0;   /* 已解析 */

	if (!g_insn_write) {
		g_insn_write = (rw_insn_write_fn)
			rw_sym_lookup("aarch64_insn_write");
		printk_debug(KERN_INFO "[rwcfi] aarch64_insn_write = %px\n",
		             (void *)g_insn_write);
	}

	if (!g_flush_icache) {
		/* ★主选：arm64 flush_icache_range() 的内联实体名 */
		g_flush_icache = (rw_flush_icache_fn)
			rw_sym_lookup("caches_clean_inval_pou");
		/* 备选 1：旧内核里带下划线前缀的实体名 */
		if (!g_flush_icache)
			g_flush_icache = (rw_flush_icache_fn)
				rw_sym_lookup("__flush_icache_range");
		/* 备选 2：某些内核把它导出/留名的情形 */
		if (!g_flush_icache)
			g_flush_icache = (rw_flush_icache_fn)
				rw_sym_lookup("flush_icache_range");
		printk_debug(KERN_INFO "[rwcfi] flush_icache = %px\n",
		             (void *)g_flush_icache);
	}

	/* 两个都要有；缺一不可（缺写=改不了，缺 flush=可能取到旧指令）*/
	if (!g_insn_write || !g_flush_icache) {
		printk(KERN_INFO "[rwcfi] arch syms not ready: write=%px flush=%px\n",
		       (void *)g_insn_write, (void *)g_flush_icache);
		return -1;
	}
	return 0;
}

/* --------------------------------------------------------------------------
 * [2] 判断目标是否已经被打过补丁（幂等）
 *
 * AFEI 只比较第一个 u32；我们比较完整 8 字节更稳。
 * 注意：读取内核 text 用普通解引用即可（内核线性映射，text 可读）。
 * ------------------------------------------------------------------------*/
static inline bool cfi_is_patched(void *target)
{
	u32 *w = (u32 *)target;
	return (w[0] == CFI_STUB_MOV_X0_1) && (w[1] == CFI_STUB_RET);
}

/* --------------------------------------------------------------------------
 * [3] 写 text —— 对应 AFEI hook_write_range @0x800c
 *
 * 逐 4 字节 aarch64_insn_write() + 收尾 flush_icache_range()。
 *   aarch64_insn_write() 内部：
 *       raw_spin_lock(patch_lock)
 *       waddr = patch_map(addr, FIX_TEXT_POKE0)      ← fixmap 临时映射为可写
 *       copy_to_kernel_nofault(waddr, &insn, 4)      ← 原子写
 *       patch_unmap()
 *       raw_spin_unlock()
 *   ⇒ 不需要 set_memory_rw/ro，且自带锁保护。
 *
 * ★与 AFEI 的差异：AFEI 把两个函数的地址存 .bss 全局指针；
 *   本移植同样存静态全局（g_insn_write / g_flush_icache），语义一致。
 *
 * 返回 0 成功，负值失败。
 * ------------------------------------------------------------------------*/
static int cfi_write_text(void *target, const u32 *words, size_t n_words)
{
	unsigned long addr = (unsigned long)target;
	size_t i;

	if (cfi_resolve_arch_syms() != 0)
		return -1;

	for (i = 0; i < n_words; i++) {
		int r = g_insn_write((void *)(addr + i * sizeof(u32)), words[i]);
		if (r) {
			printk(KERN_INFO "[rwcfi] insn_write @%lx failed: %d\n",
			       addr + i * sizeof(u32), r);
			return r;
		}
	}

	/* 写完同步 icache：否则 CPU 可能执行到旧指令 */
	g_flush_icache(addr, addr + n_words * sizeof(u32));
	return 0;
}

/* --------------------------------------------------------------------------
 * [4] cfi_bypass —— 主入口（对应 AFEI cfi_bypass @0x156c）
 *
 * 返回值语义（沿用 AFEI）：
 *   >=0  实际成功 patch 的条数
 *   -1   一条都没 patch 成功（但不算致命，调用方可继续）
 * ------------------------------------------------------------------------*/
static int cfi_bypass(void)
{
	const u32 stub[2] = { CFI_STUB_MOV_X0_1, CFI_STUB_RET };
	int patched = 0;
	int i;

	if (cfi_resolve_arch_syms() != 0) {
		printk(KERN_INFO "[rwcfi] arch symbols unavailable, cfi_bypass skipped\n");
		return -1;
	}

	for (i = 0; g_cfi_target_names[i] != NULL; i++) {
		const char *name = g_cfi_target_names[i];
		void *addr = (void *)rw_sym_lookup(name);

		if (!addr) {
			printk_debug(KERN_INFO "[rwcfi] symbol not found: %s\n", name);
			continue;
		}

		if (cfi_is_patched(addr)) {
			printk_debug(KERN_INFO "[rwcfi] %s already patched\n", name);
			continue;
		}

		if (cfi_write_text(addr, stub, 2) == 0) {
			patched++;
			printk_debug(KERN_INFO "[rwcfi] patch %s @%p succeeded\n", name, addr);
		} else {
			printk_debug(KERN_INFO "[rwcfi] patch %s @%p FAILED\n", name, addr);
		}
	}

	printk(KERN_INFO "[rwcfi] cfi_bypass patched: %d\n", patched);
	return patched;
}

/* --------------------------------------------------------------------------
 * [5] disable_kprobe_blacklist —— 对应 AFEI disable_kprobe_blacklist @0x0da0
 *
 * 目的：内核 kprobe 有一个 blacklist（位于 __start_kprobe_blacklist ..
 * __end_kprobe_blacklist），被列入的符号不允许 register_kprobe。
 * AFEI 直接把每条 entry 的 name 清空，于是 register_kprobe 时匹配不到，
 * 黑名单形同虚设。这样就能对 __cfi_* / kallsyms 等敏感符号下 kprobe。
 *
 * 结构体（内核 include/linux/kprobes.h）：
 *     struct kprobe_blacklist_entry { const char *name; unsigned long start_addr; };
 *   数组按 name/start_addr 顺序排，末尾以 name==NULL 结束。
 *
 * 日志："Disabled %d kprobe blacklist entries"（AFEI 原文）
 * ------------------------------------------------------------------------*/
struct rw_kprobe_blacklist_entry {
	const char *name;
	unsigned long start_addr;
};

static int disable_kprobe_blacklist(void)
{
	struct rw_kprobe_blacklist_entry *e;
	int n = 0;

	/*
	 * @start/@end 是内核在 vmlinux.lds.h 里生成的符号：
	 *     __start_kprobe_blacklist / __end_kprobe_blacklist
	 * 它们是否导出取决于内核配置；用 rw_sym_lookup 解析。
	 */
	unsigned long start = rw_sym_lookup("__start_kprobe_blacklist");
	unsigned long end   = rw_sym_lookup("__end_kprobe_blacklist");

	if (!start || !end || end <= start) {
		/* AFEI 原版此处日志为 "kprobe_blacklist not found" */
		printk_debug(KERN_INFO "[rwcfi] kprobe_blacklist not found\n");
		return -1;
	}

	for (e = (struct rw_kprobe_blacklist_entry *)start;
	     (unsigned long)e < end; e++) {
		if (!e->name)
			break;
		/* 清空名字：把指针指向一个静态空串，register_kprobe 的
		 * strcmp 就永远匹配不上。直接写 NULL 会让遍历提前结束，
		 * 所以不能写 NULL。 */
		e->name = "";
		n++;
	}

	printk(KERN_INFO "[rwcfi] Disabled %d kprobe blacklist entries\n", n);
	return n;
}

#endif /* CONFIG_ENABLE_CFI_BYPASS */
#endif /* CFI_BYPASS_H_ */
