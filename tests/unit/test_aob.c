/* test_aob.c — search_aob 跨块/故障页语义单测（mock driver 同 test_search） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../../agentd/search.h"
#include "../../agentd/process.h"

#define TEST_MEM_SZ    (8u << 20)
#define TEST_MEM_BASE  0x10000ull
static uint8_t g_mem[TEST_MEM_SZ];
static uint8_t g_fault[TEST_MEM_SZ / 4096];
static unsigned long g_read_calls = 0;
static gg_map_region_t g_regions[8];
static size_t g_nregions = 0;

static void mem_reset(void) {
    memset(g_mem, 0, sizeof(g_mem));
    memset(g_fault, 0, sizeof(g_fault));
    g_read_calls = 0;
    g_nregions = 0;   /* region 表也要重置（每用例独立） */
}
static void fault_page_at(uint64_t off) { g_fault[off >> 12] = 1; }
static void region_add(uint64_t start, uint64_t end);

static void region_add(uint64_t start, uint64_t end) {
    g_regions[g_nregions].start = start;
    g_regions[g_nregions].end   = end;
    snprintf(g_regions[g_nregions].perms, sizeof(g_regions[g_nregions].perms), "rw-p");
    g_nregions++;
}
gg_map_region_t *process_search_regions(pid_t pid, size_t *out_count, int region_type) {
    (void)pid; (void)region_type;
    *out_count = g_nregions;
    gg_map_region_t *c = malloc(g_nregions ? g_nregions * sizeof(gg_map_region_t) : 1);
    memcpy(c, g_regions, g_nregions * sizeof(gg_map_region_t));
    return c;
}
ssize_t driver_read(gg_driver_t *d, pid_t pid, uint64_t addr, void *buf, size_t len) {
    (void)d; (void)pid;
    g_read_calls++;
    if (addr < TEST_MEM_BASE) { errno = EFAULT; return -1; }
    uint64_t rel = addr - TEST_MEM_BASE;
    if (rel >= TEST_MEM_SZ)  { errno = EFAULT; return -1; }
    uint64_t rel_end = rel + len;
    if (rel_end > TEST_MEM_SZ) rel_end = TEST_MEM_SZ;
    uint64_t p0 = rel >> 12, p1 = (rel_end + 4095) >> 12;
    for (uint64_t p = p0; p < p1; p++) {
        if (g_fault[p]) {
            uint64_t fault_rel = (uint64_t)p << 12;
            if (fault_rel <= rel) { errno = EFAULT; return -1; }
            uint64_t readable = fault_rel - rel;
            memcpy(buf, g_mem + rel, (size_t)readable);
            return (ssize_t)readable;
        }
    }
    memcpy(buf, g_mem + rel, len);
    return (ssize_t)len;
}
ssize_t driver_write(gg_driver_t *d, pid_t pid, uint64_t addr, const void *buf, size_t len) {
    (void)d; (void)pid; (void)addr; (void)buf; (void)len; return -1;
}

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("ok  : %s\n", msg); pass++; } \
    else { printf("FAIL: %s\n", msg); fail++; } \
} while (0)

int main(void) {
    const uint8_t pat[] = "ABCDEFGH";   /* 8 字节 */
    const size_t plen = 8;
    /* 三个特征位置: 块内 / 跨块边界 / 跨块边界+偏移 */
    uint64_t p_in  = TEST_MEM_BASE + 0x100;            /* 块内 */
    uint64_t p_cross = TEST_MEM_BASE + 0x40000 - 4;    /* 跨第一块(256KB)边界 */
    uint64_t p_cross2 = TEST_MEM_BASE + 0x80000 - 2;   /* 跨第二块边界 */
    mem_reset();
    memcpy(g_mem + (p_in - TEST_MEM_BASE), pat, plen);
    memcpy(g_mem + (p_cross - TEST_MEM_BASE), pat, plen);
    memcpy(g_mem + (p_cross2 - TEST_MEM_BASE), pat, plen);
    region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);

    gg_results_t r;
    results_init(&r, 100000);
    gg_driver_t drv;
    memset(&drv, 0, sizeof(drv));
    int64_t n = search_aob(&drv, 1, &r, pat, plen, REGION_ANONYMOUS);
    CHECK(n == 3, "3 个特征全命中 (块内+跨块x2)");
    int has_in = 0, has_cross = 0, has_cross2 = 0;
    for (size_t i = 0; i < r.count; i++) {
        if (r.addrs[i] == p_in) has_in = 1;
        if (r.addrs[i] == p_cross) has_cross = 1;
        if (r.addrs[i] == p_cross2) has_cross2 = 1;
    }
    CHECK(has_in && has_cross && has_cross2, "命中地址正确 (块内+跨块边界)");
    CHECK(r.count == 3, "无重复命中");
    results_clear(&r);

    /* 故障页断裂: 特征跨故障页 → 不应命中 */
    mem_reset();
    uint64_t p_fault = TEST_MEM_BASE + 0x10000 - 4;    /* 跨 0x10000 页边界 */
    memcpy(g_mem + (p_fault - TEST_MEM_BASE), pat, plen);
    fault_page_at(0x10000);                             /* 页 0x10000 不可读 */
    region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
    results_init(&r, 100000);
    n = search_aob(&drv, 1, &r, pat, plen, REGION_ANONYMOUS);
    CHECK(n == 0, "特征跨故障页不命中 (断裂语义)");
    results_clear(&r);

    /* 短 pattern 跨块 */
    mem_reset();
    const uint8_t pat2[] = "XYZ";
    uint64_t p3 = TEST_MEM_BASE + 0x40000 - 1;   /* 3 字节跨块边界 */
    memcpy(g_mem + (p3 - TEST_MEM_BASE), pat2, 3);
    region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
    results_init(&r, 100000);
    n = search_aob(&drv, 1, &r, pat2, 3, REGION_ANONYMOUS);
    CHECK(n == 1 && r.count == 1 && r.addrs[0] == p3, "短 pattern 跨块命中");
    results_clear(&r);

    printf("\n== %s (%d pass, %d fail) ==\n", fail ? "FAILED" : "ALL PASS", pass, fail);
    return fail ? 1 : 0;
}
