#ifndef API_PROXY_H_
#define API_PROXY_H_
#include "ver_control.h"
#include "linux_kernel_api.h"
#include <linux/ctype.h>
#include <asm/uaccess.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

//声明
//////////////////////////////////////////////////////////////////////////
static inline int x_atoi(const char arr[]);
static inline bool x_isdigit(char c);
static inline struct task_struct* x_get_current(void);
static inline void * x_kmalloc(size_t size, gfp_t flags);
static inline unsigned long x_copy_from_user(void *to, const void __user *from, unsigned long n);
static inline unsigned long x_copy_to_user(void __user *to, const void *from, unsigned long n);

//实现
//////////////////////////////////////////////////////////////////////////
static inline bool x_isdigit(char c) { return (unsigned)(c - '0') < 10; }
static inline int x_atoi(const char arr[]) {
	int index = 0;
	int flag = 1;
	int num = 0;

	if (arr == NULL) { return -1; }
	while (isspace(arr[index])) { index++; }
	if (arr[index] == '-') { flag = -1; }
	if (arr[index] == '-' || arr[index] == '+') { index++; }
	while (arr[index] >= '0' && arr[index] <= '9') { num = num * 10 + arr[index] - '0';	index++; }
	return flag * num;
}

static struct task_struct *x_get_current(void) {
	unsigned long sp_el0;
	asm ("mrs %0, sp_el0" : "=r" (sp_el0));
	return (struct task_struct *)sp_el0;
}

static void * x_kmalloc(size_t size, gfp_t flags) {
	/*【本地新增·6.12 适配】
	 * 依据 2026-08-18 日志（L349）：当年改法是直接用 kmalloc()。
	 * kmalloc 在旧内核是函数、在 6.12 变成宏
	 *   #define kmalloc(...) alloc_hooks(kmalloc_noprof(__VA_ARGS__))
	 * 宏展开后仍能正常调用，比我先前写的 __kmalloc_noprof 更贴合原意，
	 * 且跨版本都成立（旧内核走函数分支）。
	 */
	return kmalloc(size, flags);
}

static unsigned long x_copy_from_user(void *to, const void __user *from, unsigned long n) {
	return __arch_copy_from_user(to, from, n);
}

static unsigned long x_copy_to_user(void __user *to, const void *from, unsigned long n) {
	return __arch_copy_to_user(to, from, n);
}
#endif /* API_PROXY_H_ */


