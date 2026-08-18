/* watchtest.c — 设备端受控硬件断点测试目标
 *
 * 用法: watchtest <秒数>
 *   - 打印: "WATCHTEST pid=<pid> addr=0x<全局变量地址>"
 *   - 每 10ms 对全局变量 g_counter 做一次 4 字节写（与 gg_watch_set 默认
 *     type=W(len=4) 完全匹配），持续 <秒数> 秒后退出。
 *
 * 配合流程（agentd rwproc 后端 + 修复后的 hwBreakpointProc_module）:
 *   1. 设备端:  ./watchtest 60            # 拿到 pid 与 addr
 *   2. PC:      gg_attach <pid>
 *   3. PC:      gg_watch_set {addr: "0x..."}        # 不再崩溃
 *   4. PC:      gg_watch_poll {}                    # 应返回命中 (tid/pc/module...)
 *   5. PC:      gg_watch_clear {}
 *   6. 设备端:  观察 dmesg 每命中一条 "hwbp HIT bp:... pc:..."，无重启
 *
 * 变量特意不用 volatile 之外的手段优化掉: 写操作通过 *(volatile int*) 完成。
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

static volatile uint32_t g_counter = 0;

int main(int argc, char **argv) {
    int secs = 60;
    if (argc > 1) secs = atoi(argv[1]);
    if (secs <= 0) secs = 60;

    printf("WATCHTEST pid=%d addr=0x%llx counter=%u\n",
           (int)getpid(), (unsigned long long)(uintptr_t)&g_counter, g_counter);
    fflush(stdout);

    struct timespec ts = {0, 10 * 1000 * 1000};   /* 10ms */
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        g_counter++;                    /* 4 字节写, 每次命中一次写监视点 */
        nanosleep(&ts, NULL);
    }
    printf("WATCHTEST done counter=%u\n", g_counter);
    return 0;
}
