/* posix_stub/sys/wait.h — MinGW 缺 sys/wait.h（仅测试编译期用）。
 * watch.c watch_loop/watch_start 用到；waitpid 由测试提供 mock（链接期）。
 */
#pragma once
#include <sys/types.h>

#define __WALL 0x40000000

pid_t waitpid(pid_t pid, int *status, int options);

#define WIFEXITED(st)    (((st) & 0xff) == 0)
#define WIFSIGNALED(st)  (((signed char)((((st) & 0x7f) + 1) >> 1)) > 0)
#define WIFSTOPPED(st)   (((st) & 0xff) == 0x7f)
#define WSTOPSIG(st)     (((st) >> 8) & 0xff)
