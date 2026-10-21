/* itest.c — 设备端集成测试：内核引擎全链路
 *
 * 场景（在 root 设备上运行）:
 *   1. fork 子进程: mmap 匿名内存, 写入已知 dword 0x12345678,
 *      收到 SIGUSR1 后才开始每 50ms 自增（避免搜索前值已变）
 *   2. 父进程: driver_init + self_test（打印驱动通道状态）
 *   3. attach 子进程 → search_exact(dword 0x12345678) → 断言命中 >= 1
 *   4. 取首个地址读回验证 → write 0x99999999 → 读回验证
 *   5. 发 SIGUSR1（子进程开始自增）→ freeze_add(0x99999999)
 *      → 等待 300ms → 读回仍为 0x99999999（冻结覆盖自增）
 *   6. unfreeze → 等待 150ms → 读回 != 0x99999999（自增恢复）
 * 任何路径都 kill 子进程, 避免孤儿进程悬挂 adb 管道。
 *
 * 编译: aarch64-linux-android28-clang -O2 -I<root> itest.c agentd 下全部 .c + third_party/cJSON.c -pie
 * 运行: adb shell su -c '/data/local/tmp/ggmcp/itest'
 */
#include "../../agentd/driver.h"
#include "../../agentd/freeze.h"
#include "../../agentd/process.h"
#include "../../agentd/search.h"
#include "../../agentd/version.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>

/* driver.c 引用的是 main.c 里的 log_msg；集成测试不链接 main.c，
 * 这里补一个同签名实现（输出到 stderr，便于定位失败步骤）。 */
void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}


static int fails = 0;
static pid_t g_child = -1;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else { printf("ok  : %s\n", msg); } \
} while (0)

static volatile int g_start = 0;
static void on_sigusr1(int s) { (void)s; g_start = 1; }

static void cleanup(void) {
    if (g_child > 0) {
        kill(g_child, SIGKILL);
        waitpid(g_child, NULL, 0);
        g_child = -1;
    }
}

/* 采样 count_equals: 在 delay_ms 间隔内采样 samples 次, 统计值等于 target 的次数 */
static int count_equals(gg_driver_t *d, pid_t pid, uint64_t addr, uint32_t target,
                        int samples, int delay_ms) {
    int hits = 0;
    for (int i = 0; i < samples; i++) {
        uint8_t b[4];
        if (driver_read(d, pid, addr, b, 4) == 4 &&
            read_bits(b, VT_DWORD) == (uint64_t)target) hits++;
        usleep((useconds_t)delay_ms * 1000);
    }
    return hits;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* 无缓冲, 崩溃时保留进度输出 */
    atexit(cleanup);
    printf("== itest: GG-MCP 引擎集成测试 ==\n");

    /* 1) 子进程: 已知值 + 收到 SIGUSR1 后自增 */
    g_child = fork();
    if (g_child == 0) {
        /* 脱离终端并关闭 stdio, 避免父退出后 adb 管道悬挂 */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        signal(SIGUSR1, on_sigusr1);
        void *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) _exit(2);
        volatile uint32_t *v = (volatile uint32_t *)((char *)p + 4096);
        *v = 0x12345678u;
        while (!g_start) usleep(10000);       /* 等 SIGUSR1 */
        for (;;) {
            *v = *v + 1u;                     /* 每 50ms 自增 */
            usleep(50000);
        }
    }
    if (g_child < 0) { printf("FATAL: fork failed\n"); return 1; }
    usleep(200000);

    /* 2) 驱动初始化 + 自检（仅 rwproc33；hook/dev/qx 三型已于 2026-10-21 删除）
     * key 从环境变量取，未设置时由 driver_init_rwproc 自动扫描 /proc
     * （模块未隐藏时可见）。 */
    gg_driver_t drv;
    const char *mem_key = getenv("GG_MEM_KEY");
    const char *bp_key = getenv("GG_BP_KEY");
    if (driver_init_rwproc(&drv, mem_key, bp_key) != 0) {
        printf("FATAL: driver_init_rwproc failed: %s\n", drv.last_error);
        return 1;
    }
    /* driver_init_rwproc 只做字段初始化，节点打开 + CMD_INIT_DEVICE_INFO
     * 在 rwproc_attach 里（与 agentd main.c 的顺序一致，漏了它 self-test 必然 unavailable）。 */
    if (rwproc_attach(&drv) != 0) {
        printf("FATAL: rwproc_attach failed: %s\n", drv.last_error);
        return 1;
    }
    int st = driver_self_test(&drv);
    printf("driver self-test: %d (%s)\n", st, driver_state_str(drv.state));
    CHECK(st > 0, "rwproc33 channel available");
    if (st <= 0) {
        /* 通道不可用则后续搜索/读写全部无意义，早退比一路报错清楚 */
        printf("FATAL: rwproc channel unavailable: %s\n", drv.last_error);
        return 1;
    }

    /* 3) 精确搜索（子进程尚未自增, 值应为 0x12345678） */
    gg_results_t res;
    results_init(&res, 100000);
    int64_t n = search_exact(&drv, g_child, &res, VT_DWORD, 0x12345678u, REGION_ANONYMOUS);
    printf("search 0x12345678 -> %lld hits\n", (long long)n);
    CHECK(n >= 1, "exact search found the value");
    if (n < 1) return 1;
    uint64_t addr = res.addrs[0];
    printf("  first hit: 0x%llx\n", (unsigned long long)addr);

    /* 4) 读回验证 + 写入 */
    uint8_t buf[8];
    CHECK(driver_read(&drv, g_child, addr, buf, 4) == 4, "read 4 bytes");
    CHECK(read_bits(buf, VT_DWORD) == 0x12345678u, "read value matches");
    uint8_t wbuf[8];   /* encode_value_local 契约: 缓冲区 ≥ 8 字节 */
    encode_value_local(VT_DWORD, 0x99999999u, wbuf);
    CHECK(driver_write(&drv, g_child, addr, wbuf, 4) == 4, "write 4 bytes");
    CHECK(driver_read(&drv, g_child, addr, buf, 4) == 4 &&
          read_bits(buf, VT_DWORD) == 0x99999999u, "write took effect");

    /* 5) 冻结: 先让子进程开始自增（每 10ms 一次, 加快发散） */
    kill(g_child, SIGUSR1);
    usleep(50000);
    gg_freeze_t fz;
    freeze_init(&fz, &drv);
    freeze_start(&fz, g_child);
    freeze_add(&fz, addr, wbuf, 4);
    /* 冻结循环每 100ms 写回; 采样 10 次×20ms: 应多次命中 0x99999999 */
    int frozen_hits = count_equals(&drv, g_child, addr, 0x99999999u, 10, 20);
    printf("  frozen samples hit 0x99999999: %d/10\n", frozen_hits);
    CHECK(frozen_hits >= 2, "freeze holds value (self-increment suppressed)");

    /* 6) 解冻: 等 >2 个自增周期(50ms)后, 值必然已离开冻结值 */
    freeze_remove(&fz, addr);
    usleep(120000);
    uint8_t b8[8];
    uint32_t after = 0;
    if (driver_read(&drv, g_child, addr, b8, 4) == 4) after = (uint32_t)read_bits(b8, VT_DWORD);
    printf("  value after unfreeze+120ms: 0x%x\n", after);
    CHECK(after != 0x99999999u, "unfreeze -> value changes again");
    freeze_stop(&fz);

    /* 7) 硬件断点：注册 / 查询 / 卸载
     *
     * 先探测模块的命令码基准：自编译模块 GET_NUM_BRPS(3) 返回执行断点槽数
     * （本机 6）；afei 的 prebuilt 是 0 起始枚举，同一条命令实际是
     * GET_NUM_WRPS，返回 4 —— 对它发 INST 会把 put_pid 打到用户态整数上
     * 直接崩内核。所以探不到 6 就跳过，绝不上手。
     *
     * 这里只覆盖 注册/查询/卸载 三个环节，不制造命中：命中路径（含单步挪位）
     * 由 dev 用的自命中探针单独验证，集成测试不碰执行断点的触发。 */
    {
        int brps = rwproc_hwbp_probe(&drv);
        if (brps != 6) {
            printf("  [skip] hwbp: 模块命令码基准不符 (GET_NUM_BRPS=%d, 期望 6)\n", brps);
        } else {
            /* 对自身代码段某处设写监视点：不会命中，只验证通道 */
            static volatile uint32_t hw_guard = 0x11223344u;
            uint64_t bp_id = 0;
            int st = rwproc_hwbp_start(&drv, (pid_t)getpid(), (uint64_t)(uintptr_t)&hw_guard,
                                       HW_BP_TYPE_W, 4, &bp_id);
            printf("  hwbp start -> %d (bp_id=0x%llx)\n", st, (unsigned long long)bp_id);
            CHECK(st == 0 && bp_id != 0, "hwbp INST returns bp id");

            if (st == 0) {
                rwproc_hwbp_hit_item_t hits[2];
                int real = -1;
                int g = rwproc_hwbp_get_hits(&drv, bp_id, hits, 2, &real);
                printf("  hwbp get_hits -> %d (real=%d)\n", g, real);
                CHECK(g == 0 && real >= 0, "hwbp GET_HIT_COUNT works");

                int s2 = rwproc_hwbp_stop(&drv, bp_id);
                printf("  hwbp stop -> %d\n", s2);
                CHECK(s2 == 0, "hwbp UNINST succeeds");
            }
        }
    }

    results_clear(&res);
    driver_close(&drv);

    printf(fails ? "\n== ITEST FAILED: %d ==\n" : "\n== ITEST ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
