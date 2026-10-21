/* test_results_page.c — results_get_page 分页边界回归（堆溢出守护）
 *
 * 锁定的缺陷（2026-10-21 修复）：
 *   bulk_read_segment() 计算连续段长度时只看两件事 ——
 *     (a) idx + seg_n < 结果集总数
 *     (b) (seg_n + 1) * sz <= GG_SEARCH_CHUNK(256KB)
 *   完全不看调用方的 limit。而 out_addrs / out_values 是 malloc(limit * 8) 的，
 *   于是 vt=DWORD 时 seg_n 可涨到 65536，一次性写 512KB 进 800 字节的缓冲。
 *   搜索结果天然是大量连续地址（扫数组/结构体几乎全连续），
 *   一次 gg_get_results(limit=100) 就能触发，agentd 还是 root 进程。
 *
 * 判据：
 *   1) out_count <= limit（不能多写）
 *   2) 缓冲区前后各 NCAN 个 u64 的 canary 必须原样（真正的越界证据）
 *
 * 编译（Windows MinGW / Linux gcc 通用）：
 *   gcc -O2 -Wall -I<gg> -I<gg>/agentd -I<gg>/shared \
 *       test_results_page.c <gg>/agentd/search.c -o test_results_page
 */
#include "../../agentd/search.h"
#include "../../agentd/process.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- mock 目标进程内存 ---------- */
#define MEM_BASE    0x10000ull
#define MEM_SZ      (1u << 20)
static uint8_t g_mem[MEM_SZ];

ssize_t driver_read(gg_driver_t *d, pid_t pid, uint64_t addr, void *buf, size_t len) {
    (void)d; (void)pid;
    if (addr < MEM_BASE) { errno = EFAULT; return -1; }
    uint64_t rel = addr - MEM_BASE;
    if (rel + len > MEM_SZ) { errno = EFAULT; return -1; }
    memcpy(buf, g_mem + rel, len);
    return (ssize_t)len;
}

ssize_t driver_write(gg_driver_t *d, pid_t pid, uint64_t addr, const void *buf, size_t len) {
    (void)d; (void)pid;
    if (addr < MEM_BASE) { errno = EFAULT; return -1; }
    uint64_t rel = addr - MEM_BASE;
    if (rel + len > MEM_SZ) { errno = EFAULT; return -1; }
    memcpy(g_mem + rel, buf, len);
    return (ssize_t)len;
}

/* search.c 里其它函数引用到，但本用例不启用搜索路径 */
gg_map_region_t *process_search_regions(pid_t pid, size_t *out_count, int region_type) {
    (void)pid; (void)region_type;
    *out_count = 0;
    return (gg_map_region_t *)malloc(sizeof(gg_map_region_t));
}

/* ---------- 用例 ---------- */
#define CANARY_VAL  0xDEADBEEFCAFEBABEull
#define NCAN        8
#define NRES        1000

static uint64_t g_addrs[NRES];
static int      g_fail = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("ok  : %s\n", what);
    } else {
        printf("FAIL: %s\n", what);
        g_fail++;
    }
}

static void fill_addrs(int contiguous, int elem_sz) {
    if (contiguous) {
        for (size_t i = 0; i < NRES; i++) g_addrs[i] = MEM_BASE + i * (uint64_t)elem_sz;
    } else {
        /* 故意制造不连续：每项间隔 8 或 16 字节交替，迫使走单地址回退分支 */
        uint64_t off = 0;
        for (size_t i = 0; i < NRES; i++) {
            g_addrs[i] = MEM_BASE + off;
            off += (i & 1) ? 64 : 40;
            if (off + 16 >= MEM_SZ) off = 0;
        }
    }
}

/* 返回 0 表示本用例通过 */
static int run_case(const char *name, int vtype, size_t limit, size_t offset, size_t expect_got) {
    gg_results_t r;
    memset(&r, 0, sizeof(r));
    r.addrs = g_addrs;
    r.count = NRES;
    r.cap = NRES;
    r.exact_vt = vtype;

    const size_t slots = NCAN + limit + NCAN;
    uint64_t *raw = (uint64_t *)malloc(slots * sizeof(uint64_t));
    uint64_t *out_addrs = (uint64_t *)malloc((limit ? limit : 1) * sizeof(uint64_t));
    if (!raw || !out_addrs) { printf("FAIL: %s (alloc)\n", name); g_fail++; free(raw); free(out_addrs); return 1; }

    /* 整块先填 canary，再只把「业务区」清零，越界写才会改到 canary */
    for (size_t i = 0; i < slots; i++) raw[i] = CANARY_VAL;
    uint64_t *vals = raw + NCAN;
    memset(vals, 0, limit * sizeof(uint64_t));
    memset(out_addrs, 0, (limit ? limit : 1) * sizeof(uint64_t));

    gg_driver_t drv;
    memset(&drv, 0, sizeof(drv));
    size_t got = 0;
    int rc = results_get_page(&drv, 4242, &r, offset, limit, out_addrs, vals, vtype, &got);

    int canary_ok = 1;
    for (size_t i = 0; i < NCAN; i++) {
        if (raw[i] != CANARY_VAL) { canary_ok = 0; break; }
        if (raw[NCAN + limit + i] != CANARY_VAL) { canary_ok = 0; break; }
    }
    if (!canary_ok)
        printf("     [%s] CANARY CLOBBERED — 越界写已发生 (limit=%zu, got=%zu)\n", name, limit, got);

    char msg[192];
    snprintf(msg, sizeof(msg), "%s: rc=0", name);
    check(rc == 0, msg);
    snprintf(msg, sizeof(msg), "%s: out_count=%zu <= limit=%zu", name, got, limit);
    check(got <= limit, msg);
    if (expect_got != (size_t)-1) {
        snprintf(msg, sizeof(msg), "%s: out_count == %zu (实际 %zu)", name, expect_got, got);
        check(got == expect_got, msg);
    }
    snprintf(msg, sizeof(msg), "%s: 缓冲前后 canary 完好（无越界写）", name);
    check(canary_ok, msg);

    free(raw);
    free(out_addrs);
    return canary_ok ? 0 : 1;
}

int main(void) {
    memset(g_mem, 0, sizeof(g_mem));
    printf("=== results_get_page 分页边界 ===\n");

    /* 自检：证明 canary 判定不是恒真 —— 手动破坏一个槽位必须被检出，
     * 否则下面所有「canary 完好」都等于没测。 */
    {
        uint64_t probe[4] = {CANARY_VAL, CANARY_VAL, CANARY_VAL, CANARY_VAL};
        probe[1] = 0;
        int detected = 0;
        for (int i = 0; i < 4; i++) if (probe[i] != CANARY_VAL) { detected = 1; break; }
        check(detected == 1, "0) 自检: canary 破坏能被检出（判定非恒真）");
    }

    /* 核心用例：连续地址 + 小 limit。修复前这里会写 (256KB/4)=65536 个 u64。 */
    fill_addrs(1, 4);
    run_case("1) 连续/limit=10", VT_DWORD, 10, 0, 10);
    run_case("2) 连续/limit=100", VT_DWORD, 100, 0, 100);
    run_case("3) 连续/limit=1000(等于结果集)", VT_DWORD, 1000, 0, NRES);

    /* 大元素类型：段长上限更小，但同样不能越过 limit */
    fill_addrs(1, 8);
    run_case("4) 连续/limit=7/vt=QWORD", VT_QWORD, 7, 0, 7);

    /* 不连续 → 走单地址回退分支 */
    fill_addrs(0, 4);
    run_case("5) 不连续/limit=50", VT_DWORD, 50, 0, 50);

    /* 边界：offset 越界、offset 正常但剩余不足 */
    fill_addrs(1, 4);
    run_case("6) offset 越界", VT_DWORD, 10, NRES + 100, 0);
    run_case("7) offset=990 剩余 10", VT_DWORD, 50, 990, 10);
    run_case("8) limit=1", VT_DWORD, 1, 0, 1);

    printf(g_fail ? "\n== %d FAIL ==\n" : "\n== ALL PASS ==\n", g_fail);
    return g_fail ? 1 : 0;
}
