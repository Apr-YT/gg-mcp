/* test_search.c — 内存搜索引擎单测（mock driver + mock regions）
 *
 * 目的: 锁定搜索正确性, 防 C1/C2 类回归悄悄合入。
 * 关键回归守护: 单个 maps 段内若存在未映射洞(guard page / 未提交内存),
 *   process_vm_readv 从洞起点读会返回 -1 EFAULT, 搜索若按整块(256KB)跳过
 *   会漏掉同 region 内该洞之后的可读字节。修复后改为页对齐前进(最多漏一页)。
 *
 * 本文件提供 mock 的 driver_read / driver_write / process_search_regions,
 * 因此编译时只链 search.c, 不链 driver.c(避开 process_vm_readv 平台依赖)与 process.c。
 *
 * 编译(Windows MinGW / Linux gcc 通用):
 *   gcc -O2 -Wall -I<gg> -I<gg>/agentd -I<gg>/shared \
 *       test_search.c <gg>/agentd/search.c -o test_search
 */
#include "../../agentd/search.h"   /* gg_driver_t, search_*, VT_*, REFINE_* */
#include "../../agentd/process.h"  /* gg_map_region_t, process_search_regions 声明 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---------- mock 进程内存 + 故障页 ---------- */
#define TEST_MEM_SZ    (8u << 20)          /* 8MB 线性内存（X4 大步跳过需大区域验证） */
#define TEST_MEM_BASE  0x10000ull
static uint8_t  g_mem[TEST_MEM_SZ];
static uint8_t  g_fault[TEST_MEM_SZ / 4096];   /* 1 = 该页不可读 */
static unsigned long g_read_calls = 0;         /* driver_read 调用计数（X4 大步验证） */

static void mem_reset(void) {
    memset(g_mem, 0, sizeof(g_mem));
    memset(g_fault, 0, sizeof(g_fault));
    g_read_calls = 0;
}
static void mem_write_u32(uint64_t addr, uint32_t v) {
    uint64_t rel = addr - TEST_MEM_BASE;
    memcpy(g_mem + rel, &v, 4);
}
/* 标记相对偏移 off(须页对齐) 所在页为不可读 */
static void fault_page_at(uint64_t off) { g_fault[off >> 12] = 1; }

/* ---------- mock process_search_regions ---------- */
static gg_map_region_t g_regions[8];
static size_t          g_nregions = 0;
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

/* ---------- mock driver_read (精确模拟 process_vm_readv 语义) ---------- */
ssize_t driver_read(gg_driver_t *d, pid_t pid, uint64_t addr, void *buf, size_t len) {
    (void)d; (void)pid;
    __atomic_fetch_add(&g_read_calls, 1, __ATOMIC_RELAXED);  /* 并行搜索时原子计数 */
    if (addr < TEST_MEM_BASE) { errno = EFAULT; return -1; }
    uint64_t rel = addr - TEST_MEM_BASE;
    if (rel >= TEST_MEM_SZ)  { errno = EFAULT; return -1; }
    uint64_t rel_end = rel + len;
    if (rel_end > TEST_MEM_SZ) rel_end = TEST_MEM_SZ;
    uint64_t p0 = rel >> 12;
    uint64_t p1 = (rel_end + 4095) >> 12;
    for (uint64_t p = p0; p < p1; p++) {
        if (g_fault[p]) {
            uint64_t fault_rel = (uint64_t)p << 12;
            if (fault_rel <= rel) {            /* 起始即落在故障页 */
                errno = EFAULT;
                return -1;
            }
            uint64_t readable = fault_rel - rel;  /* 故障页之前可读字节数 */
            memcpy(buf, g_mem + rel, (size_t)readable);
            return (ssize_t)readable;            /* 短读, 与 process_vm_readv 一致 */
        }
    }
    memcpy(buf, g_mem + rel, len);
    return (ssize_t)len;
}

/* mock driver_write (仅满足链接, 搜索路径不调用) */
ssize_t driver_write(gg_driver_t *d, pid_t pid, uint64_t addr, const void *buf, size_t len) {
    (void)d; (void)pid;
    if (addr < TEST_MEM_BASE) return -1;
    uint64_t rel = addr - TEST_MEM_BASE;
    if (rel + len > TEST_MEM_SZ) return -1;
    memcpy(g_mem + rel, buf, len);
    return (ssize_t)len;
}

/* ---------- 测试框架 ---------- */
static int fails = 0;
static void check(int cond, const char *m) {
    printf("%s %s\n", cond ? "ok  :" : "FAIL:", m);
    if (!cond) fails++;
}
static int results_has(const gg_results_t *r, uint64_t addr) {
    for (size_t i = 0; i < r->count; i++)
        if (r->addrs[i] == addr) return 1;
    return 0;
}

int main(void) {
    gg_results_t r;

    /* === 1) 全可读精确搜索: 所有匹配都找到 === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
    {
        int expect = 0;
        for (uint64_t a = TEST_MEM_BASE + 0x1000; a < TEST_MEM_BASE + 0x10000; a += 0x1000) {
            mem_write_u32(a, 0xDEADBEEFu); expect++;
        }
        results_init(&r, 0);
        int64_t n = search_exact(NULL, 0, &r, VT_DWORD, 0xDEADBEEFu, 0);
        check(n == expect, "1) exact: all readable hits found");
        results_clear(&r);
    }

    /* === 2) C2 回归: 单 region 内不可读洞之后可读字节不被漏 === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
    {
        fault_page_at(0x4000);                        /* 一个 guard-page 洞(页4) */
        uint64_t before = TEST_MEM_BASE + 0x3000;    /* 洞之前(可读) */
        uint64_t after  = TEST_MEM_BASE + 0x5000;    /* 洞之后(可读) — 修复前被整块256KB跳过漏掉 */
        mem_write_u32(before, 0xDEADBEEFu);
        mem_write_u32(after,  0xDEADBEEFu);
        results_init(&r, 0);
        int64_t n = search_exact(NULL, 0, &r, VT_DWORD, 0xDEADBEEFu, 0);
        check(n == 2, "2) C2: hits BEFORE and AFTER fault page both found (no 256KB skip)");
        check(results_has(&r, after), "2) C2: address AFTER fault page present in results");
        results_clear(&r);
    }

    /* === 3) 短读(>0)续读: 故障页两侧匹配都找到 === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
    {
        fault_page_at(0x8000);                        /* 页8 不可读 */
        uint64_t lo = TEST_MEM_BASE + 0x7000;         /* 故障页之前 */
        uint64_t hi = TEST_MEM_BASE + 0x9000;         /* 故障页之后 */
        mem_write_u32(lo, 0x12345678u);
        mem_write_u32(hi, 0x12345678u);
        results_init(&r, 0);
        int64_t n = search_exact(NULL, 0, &r, VT_DWORD, 0x12345678u, 0);
        check(n == 2, "3) short-read: hits on BOTH sides of fault page found");
        check(results_has(&r, lo) && results_has(&r, hi), "3) short-read: both side addresses present");
        results_clear(&r);
    }

    /* === 4) 连续不可读大区: 不卡死、两端可读命中都找到 === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
    {
        for (int p = 5; p <= 250; p++) g_fault[p] = 1;   /* 页5..250 连续不可读 */
        uint64_t a_lo = TEST_MEM_BASE + 0x1000;          /* 页1 */
        uint64_t a_hi = TEST_MEM_BASE + 0xFF000;         /* 页255 */
        mem_write_u32(a_lo, 0xCAFEBABEu);
        mem_write_u32(a_hi, 0xCAFEBABEu);
        results_init(&r, 0);
        int64_t n = search_exact(NULL, 0, &r, VT_DWORD, 0xCAFEBABEu, 0);
        check(n == 2, "4) continuous-fault: both ends found, no infinite loop");
        results_clear(&r);
    }

    /* === 5) 模糊 init + refine 基本正确性 (smoke) === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + 0x10000);
    {
        mem_write_u32(TEST_MEM_BASE + 0x1000, 100u);
        mem_write_u32(TEST_MEM_BASE + 0x2000, 200u);
        results_init(&r, 0);
        int64_t elems = search_unknown_init(NULL, 0, &r, VT_DWORD, 0);
        check(elems > 0, "5) fuzzy: unknown_init produced candidates");
        mem_write_u32(TEST_MEM_BASE + 0x1000, 150u);     /* 0x1000 增大, 0x2000 不变 */
        int64_t kept = search_refine(NULL, 0, &r, REFINE_INCREASED);
        check(kept >= 1, "5) fuzzy: refine INCREASED keeps grown candidate");
        results_clear(&r);
    }

    /* === 6) 指针扫描基本正确性 === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
    {
        uint64_t target   = TEST_MEM_BASE + 0x2000;
        uint64_t container = TEST_MEM_BASE + 0x5000;
        mem_write_u32(container, (uint32_t)(target & 0xFFFFFFFFu));  /* 容器持有指向 target 的指针 */
        gg_pointer_hit_t hits[16];
        size_t nh = pointer_scan(NULL, 0, &target, 1, VT_DWORD, hits, 16);
        check(nh >= 1, "6) pointer_scan: found container holding target");
        results_clear(&r);

    /* === 7) 恒逐页语义: 全不可读区逐页前进(无大步), 不漏、不卡死 === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
    {
        for (int p = 0; p < (int)(TEST_MEM_SZ / 4096); p++) g_fault[p] = 1;  /* 全 8MB 不可读 */
        results_init(&r, 0);
        int64_t n = search_exact(NULL, 0, &r, VT_DWORD, 0xCAFEBABEu, 0);
        check(n == 0, "7) reserve 全不可读: 无结果");
        /* X4 大步曾尝试 <300 次; 撤销后恒逐页 = 2048 次, C2 语义完整 */
        check(g_read_calls == (unsigned long)(TEST_MEM_SZ / 4096),
              "7) 恒逐页: 调用次数 = 总页数 2048");
        printf("    [info] reserve 8MB read_calls = %lu (逐页=2048)\n", g_read_calls);
        results_clear(&r);
    }

    /* === 8) 逐页穿越: 7MB 故障区后末端可读页仍被找到 === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
    {
        int total_pages = (int)(TEST_MEM_SZ / 4096);            /* 2048 */
        for (int p = 0; p < total_pages - 256; p++) g_fault[p] = 1;  /* 前 1792 页(7MB)故障 */
        uint64_t hit = TEST_MEM_BASE + (uint64_t)(total_pages - 200) * 4096;  /* 末端可读区 */
        mem_write_u32(hit, 0xCAFEBABEu);
        results_init(&r, 0);
        int64_t n = search_exact(NULL, 0, &r, VT_DWORD, 0xCAFEBABEu, 0);
        check(n == 1 && results_has(&r, hit), "8) 逐页穿越: 故障区后末端可读命中");
        printf("    [info] 7MB-fault read_calls = %lu (逐页=1792)\n", g_read_calls);
        results_clear(&r);
    }

    /* === 4b) 8MB 内存下小故障区(246 页)两端仍可读: 大步不破坏 C2 语义 === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
    {
        for (int p = 5; p <= 250; p++) g_fault[p] = 1;   /* 页5..250 连续不可读 */
        uint64_t a_lo = TEST_MEM_BASE + 0x1000;          /* 页1 */
        uint64_t a_hi = TEST_MEM_BASE + 0xFF000;         /* 页255 */
        mem_write_u32(a_lo, 0xCAFEBABEu);
        mem_write_u32(a_hi, 0xCAFEBABEu);
        results_init(&r, 0);
        int64_t n = search_exact(NULL, 0, &r, VT_DWORD, 0xCAFEBABEu, 0);
        check(n == 2, "4b) 8MB 下小故障区: 两端仍找到");
        results_clear(&r);
    }

    /* === 9) fuzzy 快照上限截断（GG_FUZZY_MAX_BYTES=256MB） === */
    mem_reset(); g_nregions = 0;
    region_add(TEST_MEM_BASE, TEST_MEM_BASE + (300ull << 20));   /* 300MB region */
    {
        results_init(&r, 0);
        int64_t elems = search_unknown_init(NULL, 0, &r, VT_DWORD, 0);
        check(elems == (int64_t)(GG_FUZZY_MAX_BYTES / 4),
              "9) fuzzy 快照截断到 256MB (67M 元素, 300MB region 只取前 256MB)");
        check(r.fuzzy_elem_count == (size_t)(GG_FUZZY_MAX_BYTES / 4), "9) fuzzy_elem_count 正确");
        /* 截断后 refine 不崩、不误报 */
        int64_t kept = search_refine(NULL, 0, &r, REFINE_UNCHANGED);
        check(kept >= 0, "9) refine 截断快照不崩");
        results_clear(&r);
    }
    }

    printf(fails ? "\n== FAILED: %d ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
