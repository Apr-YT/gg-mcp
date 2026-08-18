/* posix_stub/signal.h — MinGW 缺 siginfo_t / SIGTRAP（仅测试编译期用）。
 * #include_next 先引入 MinGW 原生 signal.h 再补缺项。
 */
#pragma once
#include_next <signal.h>

#ifndef SIGTRAP
#define SIGTRAP 5
#endif

#ifndef TRAP_HWBKPT
#define TRAP_HWBKPT 4
#endif

/* watch.c watch_loop 用到的最小 siginfo_t 子集 */
typedef struct gg_stub_siginfo {
    int      si_signo;
    int      si_code;
    void    *si_addr;
} siginfo_t;
