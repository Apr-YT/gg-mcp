/* test_watch.c — watch_poll 单测（T6）
 *
 * 核心: rwproc33 路径的命中去重游标（rwproc_hit_consumed[]）。
 * 真机验证过的行为: poll#1=12 → poll#2=6（只新增）→ poll#3=6，此处固化为回归护栏。
 * 另覆盖: cap 截断/剩余补齐、多 bp 独立游标、active 门控、QX 失败、ptrace 内部队列。
 *
 * 编译: gcc -O2 -Wall -I posix_stub -I ../../ -I ../../agentd \
 *       test_watch.c ../../agentd/watch.c -lpthread
 * posix_stub 提供 sys/ptrace.h/sys/uio.h/sys/wait.h/signal.h/unistd.h（MinGW 缺）。
 * ptrace/waitpid/usleep 与驱动/解析/反汇编符号由本文件 mock（链接期覆盖）。
 */
#include "../../agentd/watch.h"
#include "../../agentd/driver.h"
#include "../../agentd/disasm.h"
#include "../../agentd/process.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void check(int cond, const char *m) {
    printf("%s %s\n", cond ? "ok  :" : "FAIL:", m);
    if (!cond) fails++;
}

/* ---------- mock: rwproc 驱动命中源（测试可编程） ---------- */
#define MOCK_MAX 512
static rwproc_hwbp_hit_item_t g_hits[MOCK_MAX];
static int g_real = 0;
static int g_get_calls = 0;

int rwproc_hwbp_get_hits(gg_driver_t *d, uint64_t bp_id,
                         rwproc_hwbp_hit_item_t *out, int cap, int *real) {
    (void)d; (void)bp_id;
    g_get_calls++;
    int take = g_real < cap ? g_real : cap;
    memcpy(out, g_hits, (size_t)take * sizeof(*out));
    *real = g_real;
    return 0;
}

int qx_hwbp_get_hits(gg_driver_t *d, pid_t pid, uint64_t addr,
                     qx_hwbp_hit_item_t *out, int cap, int *real) {
    (void)d; (void)pid; (void)addr; (void)out; (void)cap; (void)real;
    errno = EINVAL;              /* QX 路径不测: 恒失败 */
    return -1;
}

/* watch_start/watch_stop 引用但测试不走的符号: 恒失败/无操作 */
int rwproc_hwbp_start(gg_driver_t *d, pid_t pid, uint64_t addr, int type, int len,
                      uint64_t *bp_id) {
    (void)d; (void)pid; (void)addr; (void)type; (void)len; (void)bp_id;
    errno = ENOSYS;
    return -1;
}
int rwproc_hwbp_stop(gg_driver_t *d, uint64_t bp_id) {
    (void)d; (void)bp_id;
    errno = ENOSYS;
    return -1;
}
int qx_hwbp_add(gg_driver_t *d, pid_t pid, uint64_t addr, int type, int len) {
    (void)d; (void)pid; (void)addr; (void)type; (void)len;
    errno = ENOSYS;
    return -1;
}
int qx_hwbp_enable(gg_driver_t *d, pid_t pid) {
    (void)d; (void)pid;
    errno = ENOSYS;
    return -1;
}
int qx_hwbp_clear(gg_driver_t *d, pid_t pid) {
    (void)d; (void)pid;
    errno = ENOSYS;
    return -1;
}

int process_resolve_addr(pid_t pid, uint64_t addr, char *path, size_t pathsz,
                         uint64_t *base, uint64_t *off) {
    (void)pid; (void)addr; (void)path; (void)pathsz; (void)base; (void)off;
    return -1;                   /* enrich 跳过模块解析 */
}

ssize_t driver_read(gg_driver_t *d, pid_t pid, uint64_t addr, void *buf, size_t len) {
    (void)d; (void)pid; (void)addr; (void)buf; (void)len;
    errno = EFAULT;
    return -1;                   /* backtrace_fp/disasm 走空路径 */
}

int disasm_a64(const uint8_t *code, size_t size, uint64_t addr,
               char (*mnem)[64], char (*ops)[128], char (*bytes)[32],
               char (*comment)[64], int max) {
    (void)code; (void)size; (void)addr; (void)mnem; (void)ops;
    (void)bytes; (void)comment; (void)max;
    return 0;
}

long ptrace(int request, pid_t pid, void *addr, void *data) {
    (void)request; (void)pid; (void)addr; (void)data;
    errno = ENOSYS;
    return -1;
}
pid_t waitpid(pid_t pid, int *status, int options) {
    (void)pid; (void)status; (void)options;
    errno = ECHILD;
    return -1;
}
int usleep(unsigned int usec) { (void)usec; return 0; }

/* ---------- 辅助 ---------- */
static void mock_clear(void) {
    g_real = 0;
    g_get_calls = 0;
    memset(g_hits, 0, sizeof(g_hits));
}

static void mock_add(int idx, uint64_t tid, uint64_t pc, uint64_t r0) {
    rwproc_hwbp_hit_item_t *h = &g_hits[idx];
    h->task_id = tid;
    h->hit_addr = pc;            /* 模块实现里 hit_addr = 命中指令 pc */
    h->regs_info.pc = pc;
    h->regs_info.sp = 0x7ff0;
    h->regs_info.regs[0] = r0;
    h->regs_info.regs[29] = 0x7f00;   /* fp */
    h->regs_info.regs[30] = 0x1234;   /* lr */
}

static void watch_init(gg_watch_t *w) {
    memset(w, 0, sizeof(*w));
    w->active = 1;
    w->use_driver = 2;           /* rwproc33 路径 */
    w->drv = (gg_driver_t *)w;   /* mock 不 deref, 任意非空 */
    w->rwproc_bp_count = 1;
    w->rwproc_bp_ids[0] = 0xAAAA;
    w->rwproc_hit_consumed[0] = 0;
}

static int field_ok(const gg_watch_hit_t *h, int idx) {
    return h->tid == (pid_t)g_hits[idx].task_id &&
           h->pc == g_hits[idx].regs_info.pc &&
           h->addr == g_hits[idx].hit_addr &&
           h->r0 == g_hits[idx].regs_info.regs[0] &&
           h->sp == g_hits[idx].regs_info.sp &&
           h->lr == g_hits[idx].regs_info.regs[30];
}

int main(void) {
    gg_watch_t w;
    gg_watch_hit_t out[64];
    int n;

    /* === 1) 基础取命中: 1 bp real=3, 字段完整透传 === */
    mock_clear();
    mock_add(0, 1001, 0x777000, 0xDEAD);
    mock_add(1, 1001, 0x777004, 0xBEEF);
    mock_add(2, 1002, 0x777008, 0xCAFE);
    g_real = 3;
    watch_init(&w);
    n = watch_poll(&w, w.drv, out, 64);
    check(n == 3, "1) 1bp real=3 -> poll 返回 3");
    check(field_ok(&out[0], 0) && field_ok(&out[1], 1) && field_ok(&out[2], 2),
          "1) 命中字段透传 (tid/pc/addr/r0/sp/lr)");
    check(w.rwproc_hit_consumed[0] == 3, "1) consumed 推进到 3");

    /* === 2) cap 截断: 剩余命中下次 poll 补齐 === */
    mock_clear();
    for (int i = 0; i < 10; i++) mock_add(i, 2000 + i, 0x5000 + (uint64_t)i * 4, i);
    g_real = 10;
    watch_init(&w);
    n = watch_poll(&w, w.drv, out, 4);
    check(n == 4, "2) real=10 cap=4 -> 返回 4");
    check(w.rwproc_hit_consumed[0] == 4, "2) consumed 只推进 4");
    n = watch_poll(&w, w.drv, out, 64);
    check(n == 6 && field_ok(&out[0], 4), "2) 再 poll -> 返回剩余 6 (从 idx4 起)");
    check(w.rwproc_hit_consumed[0] == 10, "2) consumed 推进到 10");

    /* === 3) 去重游标（真机行为固化）: 第二次 poll 只返回新增 === */
    mock_clear();
    for (int i = 0; i < 12; i++) mock_add(i, 3000 + i, 0x9000 + (uint64_t)i * 4, i);
    g_real = 12;
    watch_init(&w);
    n = watch_poll(&w, w.drv, out, 64);
    check(n == 12, "3) poll#1 返回 12");
    for (int i = 12; i < 17; i++) mock_add(i, 4000 + i, 0x9000 + (uint64_t)i * 4, i);
    g_real = 17;
    n = watch_poll(&w, w.drv, out, 64);
    check(n == 5 && field_ok(&out[0], 12), "3) poll#2 只返回新增 5 (idx12..16)");
    n = watch_poll(&w, w.drv, out, 64);
    check(n == 0, "3) poll#3 无新增 -> 0");

    /* === 4) 多 bp 独立游标 === */
    mock_clear();
    for (int i = 0; i < 3; i++) mock_add(i, 5000 + i, 0xB000 + (uint64_t)i * 4, i);
    g_real = 3;
    watch_init(&w);
    w.rwproc_bp_count = 2;
    w.rwproc_bp_ids[1] = 0xBBBB;
    w.rwproc_hit_consumed[1] = 0;
    n = watch_poll(&w, w.drv, out, 64);
    check(n == 6, "4) 2bp 各 3 条 -> 返回 6");
    check(w.rwproc_hit_consumed[0] == 3 && w.rwproc_hit_consumed[1] == 3,
          "4) 两个 bp 游标各自推进 3");

    /* === 5) active=0 门控 === */
    watch_init(&w);
    w.active = 0;
    n = watch_poll(&w, w.drv, out, 64);
    check(n == 0, "5) active=0 -> 0");

    /* === 6) real=0 无命中 === */
    mock_clear();
    watch_init(&w);
    n = watch_poll(&w, w.drv, out, 64);
    check(n == 0, "6) real=0 -> 0");

    /* === 7) QX 路径失败 -> 0（不崩） === */
    watch_init(&w);
    w.use_driver = 1;
    n = watch_poll(&w, w.drv, out, 64);
    check(n == 0, "7) qx get_hits 失败 -> 0");

    /* === 8) ptrace 路径: 内部队列取走 + 清空 + cap 截断 === */
    memset(&w, 0, sizeof(w));
    w.active = 1;
    w.use_driver = 0;
    for (int i = 0; i < 3; i++) {
        w.hits[i].tid = 6000 + i;
        w.hits[i].pc = 0xC000 + (uint64_t)i * 4;
        w.hits[i].addr = 0x1000 + (uint64_t)i * 4;
    }
    w.hit_count = 3;
    n = watch_poll(&w, w.drv, out, 2);
    check(n == 2 && w.hit_count == 1, "8a) cap=2 取 2, 剩余 1 保留");
    n = watch_poll(&w, w.drv, out, 64);
    check(n == 1 && w.hit_count == 0, "8b) 再 poll 取剩余 1 并清空");

    printf(fails ? "\n== FAILED: %d ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
