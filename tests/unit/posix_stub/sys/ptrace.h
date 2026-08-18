/* posix_stub/sys/ptrace.h — MinGW 缺 sys/ptrace.h（仅测试编译期用）。
 * 只声明 watch.c 用到的符号；ptrace/waitpid 等由测试文件提供 mock 实现（链接期）。
 */
#pragma once
#include <stdint.h>
#include <sys/types.h>

#define PTRACE_ATTACH      16
#define PTRACE_DETACH      17
#define PTRACE_CONT        7
#define PTRACE_GETREGSET   0x4204
#define PTRACE_SETREGSET   0x4205
#define PTRACE_GETSIGINFO  0x4205

/* arm64 寄存器视图（NDK 由 asm/ptrace.h 提供；测试 stub 最小集） */
struct user_pt_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

/* ARM64 硬件断点寄存器视图（watch.c set_watchpoint 用） */
struct user_hwdebug_state {
    uint32_t dbg_info;
    struct {
        uint64_t addr;
        uint64_t ctrl;
    } dbg_regs[16];
};

long ptrace(int request, pid_t pid, void *addr, void *data);
