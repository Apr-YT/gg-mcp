#ifndef RW_SYMBOL_LOOKUP_H_
#define RW_SYMBOL_LOOKUP_H_
/* ============================================================================
 * rw_symbol_lookup.h —— 运行期内核符号解析（含未导出符号）
 *
 * ── 为什么需要它 ──────────────────────────────────────────────────────────
 *  CFI 绕过要写内核 text，需要两个**未导出**符号：
 *      aarch64_insn_write      ← arm64 官方"安全改内核指令"API
 *      flush_icache_range      ← 写完同步 icache
 *  它们都不是 EXPORT_SYMBOL，编译期拿不到地址，只能运行期解析。
 *
 *  证据（AFEI same-sdk36-6.12.ko 反汇编，init_arch @0x7f9c）：
 *      aarch64_insn_write_ptr = kallsyms_lookup_name_ex("aarch64_insn_write")
 *      flush_icache_range_ptr = kallsyms_lookup_name_ex("flush_icache_range")
 *      if (!ptr) ptr = kallsyms_lookup_name_ex("__flush_icache_range")   // 备选
 *  且设备 device_symvers.txt 里**搜不到** aarch64_insn_write
 *  （4409 条导出符号全表无此项）⇒ 必须走 kallsyms，不能靠导出表。
 *
 * ── 三级解析策略（由稳到强，逐级兜底）──────────────────────────────────────
 *   L1  直接调用导出的 kallsyms_lookup_name()      —— 仅 < 5.7（之后不再导出）
 *   L2  kprobe 注册同名探针拿 kp.addr               —— 要求符号可 kprobe
 *                                                     且不在 kprobe_blacklist
 *   L3  ★先解析出 kallsyms_lookup_name 本身，再用它查任意符号
 *        （AFEI 5.10 就是这么干的：register_kprobe("kallsyms_lookup_name")
 *          → 拿到函数地址 → 用它查 aarch64_insn_write / flush_icache_range）
 *        L3 能覆盖 L2 覆盖不到的（如被 kprobe_blacklist 拦下的符号）。
 *
 *  ★失败一律返回 0，调用方自行决定是否致命。
 * ==========================================================================*/

#include "ver_control.h"

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

#if defined(CONFIG_KPROBES) || defined(CONFIG_KPROBES_MODULE)
#define RW_HAVE_KPROBE_LOOKUP 1
#include <linux/kprobes.h>
#endif

/* --------------------------------------------------------------------------
 * L1：内核仍导出 kallsyms_lookup_name（< 5.7）。
 * ------------------------------------------------------------------------*/
#if MY_LINUX_VERSION_CODE < KERNEL_VERSION(5,7,0)
extern unsigned long kallsyms_lookup_name(const char *name);
#define RW_HAVE_KALLSYMS_DIRECT 1
#endif

/* --------------------------------------------------------------------------
 * L2：kprobe 注册同名探针。
 *   注册成功即得 kp.addr；立刻注销，不产生副作用。
 *   ★若目标在 kprobe_blacklist，注册失败 —— 需先 disable_kprobe_blacklist()
 *     （见 cfi_bypass.h），调用顺序上应先清黑名单再解析敏感符号。
 * ------------------------------------------------------------------------*/
#if defined(RW_HAVE_KPROBE_LOOKUP) && !defined(RW_HAVE_KALLSYMS_DIRECT)

static __maybe_unused unsigned long rw_sym_lookup_kprobe(const char *name)
{
	unsigned long addr = 0;
	struct kprobe kp;

	memset(&kp, 0, sizeof(kp));
	kp.symbol_name = name;

	if (register_kprobe(&kp) == 0) {
		addr = (unsigned long)kp.addr;
		unregister_kprobe(&kp);
	}
	return addr;
}

/* --------------------------------------------------------------------------
 * L3：解析出 kallsyms_lookup_name 本身（经 L2），缓存后用它查任意符号。
 *
 *   kallsyms_lookup_name 是**导出符号**（在 <5.7 的 Module.symvers 里），
 *   但它自身可被 kprobe 探到 ⇒ 先用 L2 拿它的地址，
 *   之后所有符号都能用它解析（不受"是否导出/是否黑名单"限制）。
 *
 *   ★这是 AFEI 在 5.10 上的实际做法（init_arch @0x9614 调 register_kprobe）。
 * ------------------------------------------------------------------------*/
typedef unsigned long (*rw_kallsyms_lookup_fn)(const char *name);

static __maybe_unused rw_kallsyms_lookup_fn g_rw_kallsyms_lookup;

static __maybe_unused unsigned long rw_sym_lookup_via_kallsyms(const char *name)
{
	if (!g_rw_kallsyms_lookup) {
		unsigned long p =
			rw_sym_lookup_kprobe("kallsyms_lookup_name");
		if (!p)
			return 0;
		g_rw_kallsyms_lookup = (rw_kallsyms_lookup_fn)p;
	}
	return g_rw_kallsyms_lookup(name);
}

#endif /* RW_HAVE_KPROBE_LOOKUP && !RW_HAVE_KALLSYMS_DIRECT */

/* --------------------------------------------------------------------------
 * 统一入口 rw_sym_lookup()：按 L1 → L3 → L2 顺序尝试。
 *
 * ★__maybe_unused 是必需的：本函数是 static，调用方只有 cfi_bypass.h 和
 *   anon_channel.h。当这两个开关都关（CONFIG_ENABLE_CFI_BYPASS 与
 *   CONFIG_USE_ANON_CHANNEL 均未定义）时，它就是"未使用的 static 函数"，
 *   而内核编译带 -Werror,-Wunused-function ⇒ 直接编译失败。
 *   （2026-10-10 由"三开关 8 组合矩阵"验证抓出：CFI=OFF 的 4 个组合全炸。）
 * ------------------------------------------------------------------------*/
static __maybe_unused unsigned long rw_sym_lookup(const char *name)
{
	if (!name || !*name)
		return 0;

#ifdef RW_HAVE_KALLSYMS_DIRECT
	{
		/* L1：< 5.7，直接调导出的 kallsyms_lookup_name */
		unsigned long a = kallsyms_lookup_name(name);
		if (a)
			return a;
	}
#endif

#if defined(RW_HAVE_KPROBE_LOOKUP) && !defined(RW_HAVE_KALLSYMS_DIRECT)
	{
		/* L3：先用 kprobe 拿到 kallsyms_lookup_name，再查目标 */
		unsigned long a = rw_sym_lookup_via_kallsyms(name);
		if (a)
			return a;
	}
	{
		/* L2 兜底：直接 kprobe 目标符号（kallsyms 也查不到时）*/
		unsigned long a = rw_sym_lookup_kprobe(name);
		if (a)
			return a;
	}
#endif

	printk_debug(KERN_INFO "[rwsym] lookup '%s' failed\n", name);
	return 0;
}

#endif /* RW_SYMBOL_LOOKUP_H_ */
