/* 多 region 并行分片验证: 3 个 region 各放目标值, search_exact 应全命中 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../../agentd/search.h"
#include "../../agentd/process.h"

static uint8_t g_mem[3][0x10000];
static gg_map_region_t g_regions[8];
static size_t g_nregions = 0;
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
    static const uint64_t bases[3] = {0x10000, 0x20000, 0x30000};
    for (int r = 0; r < 3; r++) {
        if (addr >= bases[r] && addr + len <= bases[r] + 0x10000) {
            memcpy(buf, g_mem[r] + (addr - bases[r]), len);
            return (ssize_t)len;
        }
    }
    errno = EFAULT;
    return -1;
}
ssize_t driver_write(gg_driver_t *d, pid_t pid, uint64_t addr, const void *buf, size_t len) {
    (void)d; (void)pid; (void)addr; (void)buf; (void)len; return -1;
}
int main(void) {
    memset(g_mem, 0, sizeof(g_mem));
    /* region 0: 2 个目标; region 1: 1 个; region 2: 1 个 → 共 4 */
    uint32_t target = 0xDEADBEEF;
    memcpy(g_mem[0] + 0x100, &target, 4);
    memcpy(g_mem[0] + 0x200, &target, 4);
    memcpy(g_mem[1] + 0x50, &target, 4);
    memcpy(g_mem[2] + 0xABC, &target, 4);
    region_add(0x10000, 0x20000);
    region_add(0x20000, 0x30000);
    region_add(0x30000, 0x40000);
    gg_results_t r;
    results_init(&r, 100000);
    gg_driver_t drv;
    memset(&drv, 0, sizeof(drv));
    int64_t n = search_exact(&drv, 1, &r, VT_DWORD, target, REGION_ANONYMOUS);
    printf("found=%lld count=%zu (期望 4)\n", (long long)n, r.count);
    int hit[4] = {0, 0, 0, 0};
    uint64_t want[4] = {0x10100, 0x10200, 0x20050, 0x30ABC};
    for (size_t i = 0; i < r.count; i++)
        for (int j = 0; j < 4; j++)
            if (r.addrs[i] == want[j]) hit[j] = 1;
    int ok = (n == 4 && hit[0] && hit[1] && hit[2] && hit[3]);
    printf("%s\n", ok ? "== ALL PASS (多 region 并行分片正确) ==" : "== FAIL ==");
    results_clear(&r);
    return ok ? 0 : 1;
}
