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
#include "../../agentd/dump.h"
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
#include <uchar.h>
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

/* 同上：g_ring_enabled 定义在 main.c（--ring 开关）。itest 走纯 ioctl 路径，
 * 恒 0 即可 —— 只需要符号存在让 driver.c 链接通过。 */
int g_ring_enabled = 0;


static int fails = 0;
static pid_t g_child = -1;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else { printf("ok  : %s\n", msg); } \
} while (0)

static volatile int g_start = 0;
static void on_sigusr1(int s) { (void)s; g_start = 1; }

/* 用例 12/13 的靶子：必须落在 itest 模块的文件映射段（.data），
 * 匿名 mmap 里的值测不了 module 过滤与锚点判定。
 * volatile 防被优化掉（父进程只取地址/做搜索目标，从不直接读写）。 */
static volatile uint64_t g_chain_anchor = 0;
static volatile uint32_t g_mod_marker = 0x5AFE0001u;

/* 用例 9 的字符串靶址（相对 mmap 基址的偏移）。**不在子进程里放常量**：
 * 函数内字面量/局部数组会被编译器物化到栈上（栈也是匿名段），搜索就会多
 * 命中一份副本 —— 真机实测第二命中落在 0x7fd0... 栈范围，"恰好 1 次"恒失败。
 * 现在改成子进程只 fault-in 页面、父进程经驱动把 pattern 写进去，
 * 整个进程里只有 mmap 中那一份。 */
#define STR_A_OFF  8192u
#define STR_B_OFF  8256u
#define STR_BYTES  64u

/* 用例 15（区域差异）的区间收集：diff 回调在扫描热循环里被调用，
 * 这里只记录地址与跨度，避免在回调里做任何 IO。 */
typedef struct {
    uint64_t addr;
    uint64_t span;
} idiff_rec_t;
static idiff_rec_t g_drecs[16];
static int g_ndrecs;

static void idiff_cb(void *ud, uint64_t addr, uint64_t span,
                     const uint8_t *a, const uint8_t *b, size_t pre) {
    (void)ud; (void)a; (void)b; (void)pre;
    if (g_ndrecs < (int)(sizeof(g_drecs) / sizeof(g_drecs[0]))) {
        g_drecs[g_ndrecs].addr = addr;
        g_drecs[g_ndrecs].span = span;
        g_ndrecs++;
    }
}

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
        /* 用例 9-13 的靶子数据。父进程 fork 后 200ms 才开始搜，这里早已写完。
         * 注意fork 时父进程内存里绝不能出现这些值，否则"唯一命中"断言会被
         * 父进程侧副本打破 —— 所有魔数只在下面的父进程用例里以字面量出现，
         * 而 fork 在它们之前。 */
        /* 用例 9 的靶页由父进程经驱动写入（见 STR_A_OFF 注释），这里只负责
         * fault-in：rwproc 写按物理地址落盘，页未驻留时写会静默返回 0。 */
        memset((char *)p + STR_A_OFF, 0, STR_BYTES);
        {
            volatile uint32_t *g3 = (volatile uint32_t *)((char *)p + 12288);
            g3[0] = 0x1111AAAAu; g3[1] = 0x2222BBBBu; g3[2] = 0x3333CCCCu;
        }
        *(volatile uint32_t *)((char *)p + 16384) = 0x0D15EA5Eu;  /* refine 靶 */
        *(volatile uint32_t *)((char *)p + 28672) = 0xF00DFACEu;  /* refresh 靶（用例 14） */
        /* 用例 15 的 diff 页必须由子进程自己先 fault-in：rwproc 写是按物理地址
         * 直接落盘的，页未驻留时 get_proc_phy_addr 返回 0 → 驱动 break 返回 0
         * （不报错），父进程的整页种子写会静默变成 0 字节。 */
        *(volatile uint8_t *)((char *)p + 32768) = 0x5A;
        /* 两级指针链：anchor(.data) → mid → target */
        *(volatile uint64_t *)((char *)p + 20480) = (uint64_t)(uintptr_t)((char *)p + 24576);
        *(volatile uint64_t *)((char *)p + 24576) = 0x7E57C0DEull;
        g_chain_anchor = (uint64_t)(uintptr_t)((char *)p + 20480);
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
    int64_t n = search_exact(&drv, g_child, &res, VT_DWORD, 0x12345678u, REGION_ANONYMOUS, NULL);
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
    /* 冻结循环每 100ms 写回一次，子进程每 50ms 自增一次 —— 冻结只能把值
     * "按回"到 0x99999999，两次写回之间必然有一段时间是漂移值。取 30 个样本
     * 才足够稳定（10 个样本真机实测有过 1/10，断言 ≥2 会偶发假红）。 */
    int frozen_hits = count_equals(&drv, g_child, addr, 0x99999999u, 30, 20);
    printf("  frozen samples hit 0x99999999: %d/30\n", frozen_hits);
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

    /* 8) region dump/restore 全链路（核心循环在 dump.c，这里直接驱动，不走 wire）
     *
     * 子进程此刻仍在每 50ms 自增，刚好充当"值会自己变"的对照：
     *   dump 记录值 v1 → 等值变 → restore → 立即读回必须等于 v1
     *   → 再等 150ms（>2 个自增周期）值必须离开 v1（证明 restore 真写了，
     *     不是"碰巧读到相等"）。
     * 之后翻 dump 文件一个字节 → restore 必须被 FNV 拒掉；
     * gg_dump_list 必须能列出刚 dump 的条目。 */
    {
        const char *dpath = "/data/local/tmp/ggmcp/itest_dump.bin";
        remove(dpath);
        char derr[192];
        uint8_t pre[8];
        if (driver_read(&drv, g_child, addr, pre, 4) != 4) {
            CHECK(0, "dump: pre-read failed");
        } else {
            uint32_t v1 = (uint32_t)read_bits(pre, VT_DWORD);
            CHECK(gg_region_dump_to_file(&drv, g_child, addr, 4096, dpath,
                                         derr, sizeof(derr)) == 0,
                  "dump 4096 bytes ok");
            usleep(200000);   /* 等 ≥4 个自增周期，值必然已离开 v1 */
            uint8_t cur[8];
            uint32_t v2 = 0;
            if (driver_read(&drv, g_child, addr, cur, 4) == 4)
                v2 = (uint32_t)read_bits(cur, VT_DWORD);
            CHECK(v2 != v1, "dump: value drifted while waiting (sanity)");

            uint64_t ra = 0, rl = 0;
            CHECK(gg_region_restore_from_file(&drv, g_child, dpath, 0,
                                              &ra, &rl, derr, sizeof(derr)) == 0,
                  "restore ok");
            CHECK(ra == addr && rl == 4096, "restore returns orig addr/len");
            uint8_t post[8];
            uint32_t v3 = 0;
            if (driver_read(&drv, g_child, addr, post, 4) == 4)
                v3 = (uint32_t)read_bits(post, VT_DWORD);
            CHECK(v3 == v1, "restore wrote back the dumped value");

            usleep(150000);   /* >2 个自增周期：值应再次离开 v1 */
            uint32_t v4 = v3;
            if (driver_read(&drv, g_child, addr, post, 4) == 4)
                v4 = (uint32_t)read_bits(post, VT_DWORD);
            CHECK(v4 != v1, "post-restore value drifts again (restore was real)");

            /* 坏文件必须被拒：翻数据区一个字节，FNV 兜底 */
            FILE *df = fopen(dpath, "r+b");
            if (df) {
                fseek(df, GG_DUMP_HEADER_LEN, SEEK_SET);
                int byte = fgetc(df);
                fseek(df, GG_DUMP_HEADER_LEN, SEEK_SET);
                fputc(byte ^ 0x01, df);
                fclose(df);
                int rc = gg_region_restore_from_file(&drv, g_child, dpath, 0,
                                                     NULL, NULL, derr, sizeof(derr));
                CHECK(rc != 0, "corrupted dump rejected by checksum");
            } else {
                CHECK(0, "cannot reopen dump file for corruption test");
            }

            /* list 必须能列出该条目（name 去 .bin 后缀） */
            gg_dump_entry_t ents[8];
            int n = gg_dump_list("/data/local/tmp/ggmcp", ents, 8);
            int found = 0;
            for (int i = 0; i < n; i++)
                if (strcmp(ents[i].name, "itest_dump") == 0 &&
                    ents[i].orig_addr == addr && ents[i].payload_len == 4096)
                    found = 1;
            CHECK(found, "dump_list finds the entry with correct metadata");
            remove(dpath);
        }
    }

    /* 9) 字符串搜索：gg_str_to_pattern + search_aob（UTF-8 与 UTF-16LE 两路）。
     * 断言"恰好 1 次"：pattern 由父进程经驱动写进子进程 mmap，整个子进程里
     * 只有这一份（详见 STR_A_OFF 注释）。 */
    {
        const uint64_t base = addr - 4096;
        gg_results_t rs;
        results_init(&rs, 100000);
        uint8_t pat[64];
        int pl = gg_str_to_pattern("gg-findme-ascii", 0, 1, pat, sizeof(pat));
        CHECK(pl == 16, "str_to_pattern utf-8: 15 chars + NUL");
        CHECK(driver_write(&drv, g_child, base + STR_A_OFF, pat, (size_t)pl) == pl,
              "utf-8 pattern seeded");
        int64_t n = search_aob(&drv, g_child, &rs, pat, NULL, (size_t)pl,
                               REGION_ANONYMOUS, NULL);
        printf("  aob utf-8 hits: %lld\n", (long long)n);
        for (int i = 0; i < (int)n && i < 4; i++)
            printf("    utf-8 hit[%d] = 0x%llx\n", i, (unsigned long long)rs.addrs[i]);
        CHECK(n == 1, "utf-8 string found exactly once");
        if (n == 1) {
            uint8_t vb[16];
            CHECK(driver_read(&drv, g_child, rs.addrs[0], vb, 16) == 16 &&
                  memcmp(vb, "gg-findme-ascii", 16) == 0,
                  "utf-8 hit address reads back the string");
        }
        results_clear(&rs);

        results_init(&rs, 100000);
        pl = gg_str_to_pattern("GG测试Str", 1, 1, pat, sizeof(pat));
        CHECK(pl == 16, "str_to_pattern utf-16: 7 BMP chars + NUL = 16 bytes");
        CHECK(driver_write(&drv, g_child, base + STR_B_OFF, pat, (size_t)pl) == pl,
              "utf-16 pattern seeded");
        n = search_aob(&drv, g_child, &rs, pat, NULL, (size_t)pl,
                       REGION_ANONYMOUS, NULL);
        printf("  aob utf-16 hits: %lld\n", (long long)n);
        for (int i = 0; i < (int)n && i < 4; i++)
            printf("    utf-16 hit[%d] = 0x%llx\n", i, (unsigned long long)rs.addrs[i]);
        CHECK(n == 1, "utf-16 string found exactly once");
        results_clear(&rs);
    }

    /* 10) 组合搜索：三值共现 + 顺序无关（锚点跟随 vals[0]） */
    {
        gg_results_t rs;
        results_init(&rs, 100000);
        uint64_t vals[3] = {0x1111AAAAull, 0x2222BBBBull, 0x3333CCCCull};
        int64_t n = search_group(&drv, g_child, &rs, vals, 3, VT_DWORD, 64,
                                 REGION_ANONYMOUS, NULL);
        CHECK(n == 1, "group search finds the trio exactly once");
        if (n == 1) {
            uint8_t vb[4];
            CHECK(driver_read(&drv, g_child, rs.addrs[0], vb, 4) == 4 &&
                  read_bits(vb, VT_DWORD) == 0x1111AAAAull,
                  "group anchor address holds vals[0]");
        }
        results_clear(&rs);

        /* 顺序打乱：锚点应换到 rev[0] 所在地址（顺序无关的实证） */
        results_init(&rs, 100000);
        uint64_t rev[3] = {0x3333CCCCull, 0x2222BBBBull, 0x1111AAAAull};
        n = search_group(&drv, g_child, &rs, rev, 3, VT_DWORD, 64,
                         REGION_ANONYMOUS, NULL);
        CHECK(n == 1, "group search is order-independent");
        if (n == 1) {
            uint8_t vb[4];
            CHECK(driver_read(&drv, g_child, rs.addrs[0], vb, 4) == 4 &&
                  read_bits(vb, VT_DWORD) == 0x3333CCCCull,
                  "reversed order anchors at rev[0]");
        }
        results_clear(&rs);
    }

    /* 11) refine 增量比较：Δ 由父进程 driver_write 精确控制（不受自增时序影响）。
     * by=25 恰好保留；紧接 by=24 必须清零 —— 增量不是"大致变大"而是精确匹配。 */
    {
        gg_results_t rs;
        results_init(&rs, 100000);
        int64_t n = search_exact(&drv, g_child, &rs, VT_DWORD, 0x0D15EA5Eull,
                                 REGION_ANONYMOUS, NULL);
        CHECK(n == 1, "refine target found exactly once");
        if (n == 1) {
            uint64_t a11 = rs.addrs[0];
            uint8_t wb[8];
            encode_value_local(VT_DWORD, 0x0D15EA5Eull + 25, wb);
            CHECK(driver_write(&drv, g_child, a11, wb, 4) == 4,
                  "bump target value by 25");
            int64_t rem = search_refine_exact_by(&drv, g_child, &rs,
                                                 REFINE_INCREASED_BY, 25);
            CHECK(rem == 1, "increased_by 25 keeps exactly the bumped one");
            rem = search_refine_exact_by(&drv, g_child, &rs,
                                         REFINE_INCREASED_BY, 24);
            CHECK(rem == 0, "increased_by 24 (wrong delta) clears all");
        }
        results_clear(&rs);
    }

    /* 12) module 限定：g_mod_marker 在 itest 的 .data（文件映射段，path 含 "itest"）。
     * 正向：module="itest" 命中；反向：不存在模块 → 严格 0；对照：模块过滤只会收窄。 */
    {
        CHECK(g_mod_marker == 0x5AFE0001u, "mod marker alive in .data");
        gg_results_t rs;
        results_init(&rs, 100000);
        int64_t n_all = search_exact(&drv, g_child, &rs, VT_DWORD, 0x5AFE0001ull,
                                     REGION_ALL, NULL);
        results_clear(&rs);
        results_init(&rs, 100000);
        int64_t n_mod = search_exact(&drv, g_child, &rs, VT_DWORD, 0x5AFE0001ull,
                                     REGION_ALL, "itest");
        results_clear(&rs);
        results_init(&rs, 100000);
        int64_t n_no = search_exact(&drv, g_child, &rs, VT_DWORD, 0x5AFE0001ull,
                                    REGION_ALL, "no_such_module_xyz");
        results_clear(&rs);
        printf("  module filter: all=%lld itest=%lld none=%lld\n",
               (long long)n_all, (long long)n_mod, (long long)n_no);
        CHECK(n_mod >= 1, "module=itest finds the .data marker");
        CHECK(n_mod <= n_all, "module filter narrows (never widens)");
        CHECK(n_no == 0, "unknown module filters everything out");
    }

    /* 13) pointer_chain_scan：构造 g_chain_anchor(.data) → mid → target 两级链。
     * fork 后父子地址布局一致，父进程的 &g_chain_anchor 即子进程内该变量地址。
     * 期望存在一条 depth==2 的链且两段 offset 均为 0（链上指针都是精确指向）。
     * 栈上可能残留指向 mid/target 的旧指针值 —— 它们的 container 在匿名段，
     * 进不了锚点，只是噪声候选，不影响断言。 */
    {
        gg_results_t rs;
        results_init(&rs, 100000);
        int64_t n = search_exact(&drv, g_child, &rs, VT_QWORD, 0x7E57C0DEull,
                                 REGION_ANONYMOUS, NULL);
        CHECK(n == 1, "chain target value found exactly once");
        if (n == 1) {
            uint64_t t = rs.addrs[0];
            size_t nmaps = 0;
            gg_map_region_t *maps = process_maps(g_child, &nmaps);
            CHECK(maps != NULL && nmaps > 0, "process_maps returns regions");
            size_t na = 0;
            gg_map_region_t *anchors = malloc(nmaps * sizeof(*anchors));
            for (size_t i = 0; maps && i < nmaps; i++)
                if (maps[i].path[0]) anchors[na++] = maps[i];
            printf("  %zu of %zu regions are file-mapped anchors\n", na, nmaps);

            gg_pointer_chain_t chains[16];
            size_t nc = pointer_chain_scan(&drv, g_child, t, VT_QWORD, 3, 0,
                                           anchors, na, chains, 16);
            printf("  pointer_chain_scan -> %zu chains\n", nc);
            CHECK(nc >= 1, "chain scan finds >=1 anchored chain");
            int found = 0;
            for (size_t i = 0; i < nc; i++) {
                if (chains[i].depth == 2 &&
                    chains[i].nodes[0].addr == (uint64_t)(uintptr_t)&g_chain_anchor &&
                    chains[i].nodes[1].addr == chains[i].nodes[0].value &&
                    chains[i].nodes[1].value == t &&
                    chains[i].nodes[0].offset == 0 &&
                    chains[i].nodes[1].offset == 0)
                    found = 1;
            }
            CHECK(found, "expected 2-level chain via g_chain_anchor (offsets 0)");
            free(maps);
            free(anchors);
        }
        results_clear(&rs);
    }

    /* 14) results_refresh：手动刷新重置 refine 基线。
     * 流程：搜唯一魔数 → 父进程写新值 → refresh 报 changed==1（基线更新为现值）
     *       → refine(UNCHANGED) 恰好 1 条 —— 判别点：当前值 0xFEEDFACE
     *         对老基线(0xF00DFACE)是"变化"，会被清零；只有基线真的被
     *         refresh 重置成 0xFEEDFACE，UNCHANGED 才会保留它。
     *       → gg_undo 回退到 refresh 前的状态（undo 栈在 refresh 时也压了一份）。 */
    {
        gg_results_t rs;
        results_init(&rs, 100000);
        int64_t n = search_exact(&drv, g_child, &rs, VT_DWORD, 0xF00DFACEu,
                                 REGION_ANONYMOUS, NULL);
        CHECK(n == 1, "refresh target found exactly once");
        if (n == 1) {
            uint64_t t = rs.addrs[0];
            uint32_t nv = 0xFEEDFACEu;
            CHECK(driver_write(&drv, g_child, t, &nv, 4) == 4, "write new value");
            size_t dropped = 99;
            int64_t changed = results_refresh(&drv, g_child, &rs, &dropped);
            printf("  refresh -> changed=%lld dropped=%zu count=%zu\n",
                   (long long)changed, dropped, rs.count);
            CHECK(changed == 1, "refresh reports exactly 1 change");
            CHECK(dropped == 0, "refresh drops nothing (target readable)");
            CHECK(rs.exact_vals_valid && rs.exact_vals &&
                  rs.exact_vals[0] == 0xFEEDFACEu,
                  "baseline now holds refreshed value");

            /* 值保持 0xFEEDFACE：只有基线被重置，UNCHANGED 才留得住它 */
            int64_t kept = search_refine_exact(&drv, g_child, &rs, REFINE_UNCHANGED);
            printf("  refine UNCHANGED after refresh -> %lld\n", (long long)kept);
            CHECK(kept == 1, "refine compares against REFRESHED baseline");

            int64_t undo = search_refine_undo_exact(&rs);
            printf("  undo -> count=%lld vals[0]=0x%llx\n",
                   (long long)undo, rs.exact_vals ? (unsigned long long)rs.exact_vals[0] : 0ull);
            CHECK(undo == 1 && rs.exact_vals &&
                  rs.exact_vals[0] == 0xFEEDFACEu,
                  "undo restores post-refresh pre-refine state");
        }
        results_clear(&rs);
    }

    /* 15) 区域差异比较（gg_region_diff）：dump → 改 4 字节 → diff vs live
     *     精确定位到 +100 / span 4 → 两 dump 互比结论一致 → 改回原值 diff 归零。
     * 地址选 base+32768，避开其他用例的全部靶点；先把整页写成 0x5A，
     * 既保证页已分配（否则 dump 可能因未落页短读失败），也给了确定基线。 */
    {
        const char *pa = "/data/local/tmp/ggmcp/itest_diff_a.bin";
        const char *pb = "/data/local/tmp/ggmcp/itest_diff_b.bin";
        remove(pa); remove(pb);
        const uint64_t daddr = (addr - 4096) + 32768;   /* addr = base+4096 */
        const uint64_t dlen = 4096;
        char derr[192];
        uint8_t *page = malloc(dlen);
        if (!page) {
            CHECK(0, "diff: malloc failed");
        } else {
            memset(page, 0x5A, (size_t)dlen);
            ssize_t sw = driver_write(&drv, g_child, daddr, page, (size_t)dlen);
            printf("  diff seed: daddr=0x%llx wr=%lld (%s)\n",
                   (unsigned long long)daddr, (long long)sw, drv.last_error);
            CHECK(sw == (ssize_t)dlen,
                  "diff: seed page with 0x5A");
            CHECK(gg_region_dump_to_file(&drv, g_child, daddr, dlen, pa,
                                         derr, sizeof(derr)) == 0, "diff: dump A");
            uint32_t nv = 0xDEADBEEFu;
            CHECK(driver_write(&drv, g_child, daddr + 100, &nv, 4) == 4,
                  "diff: modify 4 bytes at +100");

            uint64_t regions = 0, dbytes = 0;
            int trunc = 0;
            g_ndrecs = 0;
            int rc = gg_region_diff_live(&drv, g_child, pa, 0, 0, idiff_cb, NULL,
                                         &regions, &dbytes, &trunc, derr, sizeof(derr));
            CHECK(rc == 0, "diff vs live ok");
            CHECK(regions == 1 && g_ndrecs == 1, "diff vs live: exactly 1 region");
            CHECK(g_ndrecs == 1 && g_drecs[0].addr == daddr + 100 &&
                  g_drecs[0].span == 4, "diff vs live pinpoints +100 span 4");
            CHECK(dbytes == 4, "diff vs live: 4 changed bytes");

            CHECK(gg_region_dump_to_file(&drv, g_child, daddr, dlen, pb,
                                         derr, sizeof(derr)) == 0, "diff: dump B");
            g_ndrecs = 0;
            rc = gg_region_diff_files(pa, pb, 0, 0, idiff_cb, NULL,
                                      &regions, &dbytes, &trunc, derr, sizeof(derr));
            CHECK(rc == 0 && regions == 1 && g_ndrecs == 1,
                  "diff two dumps: exactly 1 region");
            CHECK(g_ndrecs == 1 && g_drecs[0].addr == daddr + 100 &&
                  g_drecs[0].span == 4, "diff two dumps: same location");

            /* 不同区域的两份 dump 必须被拒（长度/原址一致才可比） */
            rc = gg_region_diff_files(pa, "/data/local/tmp/ggmcp/no_such_dump.bin",
                                      0, 0, idiff_cb, NULL, &regions, &dbytes,
                                      &trunc, derr, sizeof(derr));
            CHECK(rc != 0, "diff with missing file rejected");

            /* 改回原值 → diff 归零（diff 反映实际内容，不是缓存） */
            uint32_t ov = 0x5A5A5A5Au;
            CHECK(driver_write(&drv, g_child, daddr + 100, &ov, 4) == 4,
                  "diff: restore original bytes");
            g_ndrecs = 0;
            rc = gg_region_diff_live(&drv, g_child, pa, 0, 0, idiff_cb, NULL,
                                     &regions, &dbytes, &trunc, derr, sizeof(derr));
            CHECK(rc == 0 && regions == 0 && dbytes == 0,
                  "diff vs live back to zero after restoring bytes");

            free(page);
            remove(pa); remove(pb);
        }
    }

    results_clear(&res);
    driver_close(&drv);

    printf(fails ? "\n== ITEST FAILED: %d ==\n" : "\n== ITEST ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
