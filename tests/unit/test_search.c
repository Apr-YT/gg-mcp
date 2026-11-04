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
static void mem_write_u64(uint64_t addr, uint64_t v) {
    uint64_t rel = addr - TEST_MEM_BASE;
    memcpy(g_mem + rel, &v, 8);
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
        /* lookback=0（精确）、aligned=1：与旧行为一致 */
        size_t nh = pointer_scan(NULL, 0, &target, 1, VT_DWORD, 0, 1, hits, 16);
        check(nh >= 1, "6) pointer_scan: found container holding target");
        int found_exact = 0;
        for (size_t i = 0; i < nh; i++)
            if (hits[i].container == container) found_exact = 1;
        check(found_exact, "6) pointer_scan: 精确命中 container 地址正确");
        results_clear(&r);

        /* === 6b) lookback 区间匹配（P1-11 回归）===
         * 真实 C++ 对象场景：持有者存对象首址，目标字段在对象内部
         * （+0x14 之类）—— 精确匹配必然 0 命中，lookback 才能抓到。 */
        mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
        {
            uint64_t obj_base = TEST_MEM_BASE + 0x3000;      /* 对象首址 */
            uint64_t field    = obj_base + 0x14;             /* 金币字段在对象内 */
            uint64_t holder   = TEST_MEM_BASE + 0x6000;      /* 持有者存的是对象首址 */
            mem_write_u32(holder, (uint32_t)(obj_base & 0xFFFFFFFFu));
            gg_pointer_hit_t hits2[16];
            /* 精确匹配字段地址：必然 0 命中（持有者存的是首址不是字段地址） */
            size_t n_exact = pointer_scan(NULL, 0, &field, 1, VT_DWORD, 0, 1, hits2, 16);
            check(n_exact == 0, "6b) 精确匹配对象内字段: 0 命中（证明缺口存在）");
            /* lookback=0x200: 命中持有者, target 报告为被匹配的目标本身 */
            size_t n_lb = pointer_scan(NULL, 0, &field, 1, VT_DWORD, 0x200, 1, hits2, 16);
            int found_lb = 0;
            for (size_t i = 0; i < n_lb; i++)
                if (hits2[i].container == holder) found_lb = 1;
            check(n_lb >= 1 && found_lb,
                  "6b) lookback=0x200: 抓到存对象首址的持有者");
            /* 边界外差 0x201 的持有者不该被算进来（find_near_target 边界） */
            uint64_t holder2 = TEST_MEM_BASE + 0x7000;
            mem_write_u32(holder2, (uint32_t)((obj_base - 0x201) & 0xFFFFFFFFu));
            size_t n_lb2 = pointer_scan(NULL, 0, &field, 1, VT_DWORD, 0x200, 1, hits2, 16);
            int leaked = 0;
            for (size_t i = 0; i < n_lb2; i++)
                if (hits2[i].container == holder2) leaked = 1;
            check(!leaked, "6b) 差 0x201 超出 lookback: 不命中（边界正确）");
        }
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

    /* === 9z) exact/range 跨 256KB 块边界（2026-10-21 审查 A8 回归）===
     * 值起始于块末尾、剩余字节落在下一块。旧实现只在单块内扫 i+sz<=rd，
     * 这条匹配会整条漏掉（aob 早就有 carry 缓冲，exact/range 没有）。 */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + (1u << 20));
    {
        uint64_t v64 = 0x1122334455667788ull;
        uint64_t hit = TEST_MEM_BASE + GG_SEARCH_CHUNK - 4;   /* 8 字节里 4 字节跨块 */
        memcpy(g_mem + (hit - TEST_MEM_BASE), &v64, 8);
        results_init(&r, 0);
        int64_t n = search_exact(NULL, 0, &r, VT_QWORD, v64, 0);
        check(n == 1, "9z) 跨块 exact: QWORD 骑在 256KB 边界仍命中");
        check(results_has(&r, hit), "9z) 跨块 exact: 命中地址正确");
        results_clear(&r);
    }
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + (1u << 20));
    {
        uint32_t v32 = 0xCAFEBABEu;
        uint64_t hit = TEST_MEM_BASE + GG_SEARCH_CHUNK - 2;   /* 4 字节里 2 字节跨块 */
        memcpy(g_mem + (hit - TEST_MEM_BASE), &v32, 4);
        results_init(&r, 0);
        int64_t n = search_range(NULL, 0, &r, VT_DWORD,
                                 (double)0xCAFEBABEu, (double)0xCAFEBABEu, 0, 0);
                                 /* 最后一个 0 = aligned：本用例故意把 dword 放在
                                  * 非对齐位置（CHUNK-2，2 字节跨块），所以必须
                                  * 用逐字节模式才能命中。对齐模式下这种值本来就
                                  * 扫不到（真实内存里 dword 不会这么摆）。 */
        check(n >= 1 && results_has(&r, hit), "9z) 跨块 range: 边界上的值仍命中");
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
        /* P1-10: 截断不再是静默的 —— 被丢掉的字节数和起点都要报出来 */
        check(r.fuzzy_skipped_bytes == (300ull << 20) - GG_FUZZY_MAX_BYTES,
              "9) skipped_bytes = 300MB-256MB = 44MB");
        check(r.fuzzy_skipped_from == TEST_MEM_BASE + GG_FUZZY_MAX_BYTES,
              "9) skipped_from = 截断点地址");
        /* 截断后 refine 不崩、不误报 */
        int64_t kept = search_refine(NULL, 0, &r, REFINE_UNCHANGED);
        check(kept >= 0, "9) refine 截断快照不崩");
        results_clear(&r);
    }
    {
        /* P1-10: 未超限时 skipped 报告必须为零（不能误报） */
        mem_reset(); g_nregions = 0;
        region_add(TEST_MEM_BASE, TEST_MEM_BASE + 0x10000);
        results_init(&r, 0);
        int64_t elems = search_unknown_init(NULL, 0, &r, VT_DWORD, 0);
        check(elems > 0 && r.fuzzy_skipped_bytes == 0 && r.fuzzy_skipped_from == 0,
              "9b) 小 region: 无截断报告");
        results_clear(&r);
    }

    /* === 10) 区间搜索: exact_vt 生效 + refine 首轮建基线（2026-11-03 P0-5 回归）
     * 旧缺陷: search_range 不设 exact_vt → get_results 恒按 dword 读值,
     * qword 指针只显示低 32 位; 且 refine 会拿单一值(0)填快照, 候选全灭。 */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + 0x10000);
    {
        uint64_t a = TEST_MEM_BASE + 0x1000, b = TEST_MEM_BASE + 0x2000;
        uint64_t va = 150, vb = 180;
        memcpy(g_mem + (a - TEST_MEM_BASE), &va, 8);
        memcpy(g_mem + (b - TEST_MEM_BASE), &vb, 8);
        results_init(&r, 0);
        int64_t n = search_range(NULL, 0, &r, VT_QWORD, 100, 200, 0, 1);
        check(n == 2, "10) range: 两候选命中");
        check(r.exact_vt == VT_QWORD, "10) range 后 exact_vt=qword (get_results 按宽度读值)");
        check(r.exact_vals_valid == 0, "10) range 后基线未建立 (valid=0)");
        /* 首次 refine = 建基线: 不筛选, count 不变 */
        int64_t n1 = search_refine_exact(NULL, 0, &r, REFINE_CHANGED);
        check(n1 == 2, "10) 首次 refine 建基线: count 不变 (op 被忽略)");
        check(r.exact_vals_valid == 1, "10) 基线已建立 (valid=1)");
        /* 第二次 refine 才比较: 改 a, CHANGED 只留 a, UNCHANGED 只留 b */
        uint64_t va2 = 999;
        memcpy(g_mem + (a - TEST_MEM_BASE), &va2, 8);
        int64_t n2 = search_refine_exact(NULL, 0, &r, REFINE_CHANGED);
        check(n2 == 1 && results_has(&r, a), "10) 二次 refine CHANGED: 只留变化的 a");
        results_clear(&r);
    }
    /* 精确搜索路径不受建基线轮影响: 搜索即基线, 首次 refine 直接比较 */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + 0x10000);
    {
        uint64_t a = TEST_MEM_BASE + 0x1000;
        uint64_t v = 150;
        memcpy(g_mem + (a - TEST_MEM_BASE), &v, 8);
        results_init(&r, 0);
        search_exact(NULL, 0, &r, VT_QWORD, v, 0);
        check(r.exact_vals_valid == 1, "10b) exact: 搜索即基线 (valid=1)");
        uint64_t v2 = 999;
        memcpy(g_mem + (a - TEST_MEM_BASE), &v2, 8);
        int64_t n = search_refine_exact(NULL, 0, &r, REFINE_CHANGED);
        check(n == 1, "10b) exact: 首次 refine 直接比较 (不走建基线轮)");
        results_clear(&r);
    }
    }   /* 用例 6 的块（这个块一直开到 main 尾部，中间用例都在里面） */

    /* === 11) 字符串搜索: gg_str_to_pattern 转码器（UTF-8→UTF-16LE）=== */
    {
        uint8_t b[64];
        /* ASCII + 终止符: "abc" → 61 62 63 00 00 (UTF-16LE 每字符2字节+NUL2字节) */
        int n = gg_str_to_pattern("abc", 1, 1, b, sizeof(b));
        check(n == 8 && b[0]==0x61 && b[1]==0 && b[2]==0x62 && b[3]==0 &&
              b[4]==0x63 && b[5]==0 && b[6]==0 && b[7]==0,
              "11) utf16 ascii+NUL: 8 字节");
        /* 中文 U+4E2D (中): LE = 2D 4E */
        n = gg_str_to_pattern("\xE4\xB8\xAD", 1, 0, b, sizeof(b));
        check(n == 2 && b[0] == 0x2D && b[1] == 0x4E, "11) utf16 中文 U+4E2D → 2D 4E (LE)");
        /* emoji U+1F600 (😀): 代理对 D83D DE00 → LE: 3D D8 00 DE */
        n = gg_str_to_pattern("\xF0\x9F\x98\x80", 1, 0, b, sizeof(b));
        check(n == 4 && b[0]==0x3D && b[1]==0xD8 && b[2]==0x00 && b[3]==0xDE,
              "11) utf16 emoji U+1F600 → 代理对 D83D DE00 (LE)");
        /* UTF-8 直通模式 */
        n = gg_str_to_pattern("AB", 0, 1, b, sizeof(b));
        check(n == 3 && b[0]=='A' && b[1]=='B' && b[2]==0, "11) utf8 直通+单字节NUL");
        /* 非法序列必须拒绝: 孤立续字节 / 截断序列 / 代理区码点 */
        check(gg_str_to_pattern("\x80", 1, 0, b, sizeof(b)) == -1, "11) 拒绝孤立续字节");
        check(gg_str_to_pattern("\xE4\xB8", 1, 0, b, sizeof(b)) == -1, "11) 拒绝截断序列");
        check(gg_str_to_pattern("\xED\xA0\x80", 1, 0, b, sizeof(b)) == -1, "11) 拒绝代理区码点 U+D800");
        check(gg_str_to_pattern("\xC0\x80", 1, 0, b, sizeof(b)) == -1, "11) 拒绝过长编码 U+0000");
        /* 上限检查 */
        check(gg_str_to_pattern("abcdef", 0, 0, b, 4) == -1, "11) 超上限返回 -1");
    }

    /* === 11b) 端到端: UTF-16LE 字符串进内存后能搜到 === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + 0x10000);
    {
        uint64_t where = TEST_MEM_BASE + 0x1000;
        /* "中" U+4E2D + NUL: 2D 4E 00 00 */
        uint8_t utf16[4] = {0x2D, 0x4E, 0x00, 0x00};
        memcpy(g_mem + (where - TEST_MEM_BASE), utf16, 4);
        uint8_t pat[64], mask[64];
        int plen = gg_str_to_pattern("\xE4\xB8\xAD", 1, 1, pat, sizeof(pat));
        check(plen == 4, "11b) 转码出 4 字节 pattern");
        memset(mask, 0xFF, (size_t)plen);
        results_init(&r, 0);
        int64_t nh = search_aob(NULL, 0, &r, pat, mask, (size_t)plen, 0);
        check(nh == 1 && results_has(&r, where), "11b) UTF-16LE 中文搜索命中正确地址");
        results_clear(&r);
    }

    /* === 12) 组合搜索（group search）：顺序无关 + 位置占用 + span 边界 === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + 0x10000);
    {
        uint64_t base = TEST_MEM_BASE + 0x1000;
        /* 布局: [+0x1000]=100 [+0x1004]=200 [+0x1008]=300 [+0x100C]=100
         *        [+0x2000]=100 (孤立锚点，组不齐，必须被剔除) */
        mem_write_u32(base,        100u);
        mem_write_u32(base + 0x4,  200u);
        mem_write_u32(base + 0x8,  300u);
        mem_write_u32(base + 0xC,  100u);
        mem_write_u32(base + 0x1000, 100u);

        uint64_t vals[8];
        results_init(&r, 0);

        /* 顺序无关：锚点 200，窗口里 100/300 在锚点两侧 */
        vals[0] = 200; vals[1] = 100; vals[2] = 300;
        int64_t n = search_group(NULL, 0, &r, vals, 3, VT_DWORD, 16, 0);
        check(n == 1 && results_has(&r, base + 0x4),
              "12) 顺序无关: 锚点 200 命中, 窗口含两侧值");
        /* refine 链路可用：结果集元数据继承锚点搜索 */
        check(r.exact_vt == VT_DWORD && r.exact_vals_valid == 1,
              "12) 锚点元数据: refine 可直接收缩");
        results_clear(&r);

        /* 重复值占用: "100;100::8" 命中（+0x1000 与 +0x100C 距 12 > 8，但
         * 窗口是锚点两侧各 span —— +0x100C 距锚点 12 ≤ span? span=8 不命中，
         * span=16 命中）。这条验证重复值必须占不同位置。
         * 注意多锚点互相命中：span=16 时 +0x1000 与 +0x100C 互为对方的
         * 第二个 100，两个锚点都命中；+0x2000 孤立，必须被剔除。 */
        vals[0] = 100; vals[1] = 100;
        results_init(&r, 0);
        n = search_group(NULL, 0, &r, vals, 2, VT_DWORD, 8, 0);
        check(n == 0, "12) 重复值 span=8: 两个 100 距 12 > 8 不命中");
        results_clear(&r);
        results_init(&r, 0);
        n = search_group(NULL, 0, &r, vals, 2, VT_DWORD, 16, 0);
        check(n == 2 && results_has(&r, base) && results_has(&r, base + 0xC) &&
              !results_has(&r, base + 0x1000),
              "12) 重复值 span=16: 两个 100 互认命中, 孤立锚点剔除");
        results_clear(&r);

        /* span 边界: vals={100,300}。+0x100C 锚点看 +0x1008 的 300 距 4 →
         * span=4 命中；base 锚点看 +0x1008 距 8 → span=8 命中。 */
        vals[0] = 100; vals[1] = 300;
        results_init(&r, 0);
        n = search_group(NULL, 0, &r, vals, 2, VT_DWORD, 4, 0);
        check(n == 1 && results_has(&r, base + 0xC) && !results_has(&r, base),
              "12) span=4: 只有距 4 的锚点命中");
        results_clear(&r);
        results_init(&r, 0);
        n = search_group(NULL, 0, &r, vals, 2, VT_DWORD, 8, 0);
        check(n == 2 && results_has(&r, base) && results_has(&r, base + 0xC),
              "12) span=8: 两个锚点都在边界内命中");
        results_clear(&r);

        /* 参数防御: nvals<2 / span=0 / 超上限 */
        results_init(&r, 0);
        n = search_group(NULL, 0, &r, vals, 1, VT_DWORD, 16, 0);
        check(n == -1, "12) nvals<2 拒绝");
        n = search_group(NULL, 0, &r, vals, 2, VT_DWORD, 0, 0);
        check(n == -1, "12) span=0 拒绝");
        n = search_group(NULL, 0, &r, vals, 2, VT_DWORD, 99999, 0);
        check(n == -1, "12) span 超上限拒绝");
        results_clear(&r);
    }

    /* === 13) refine 增量比较（increased_by / decreased_by）=== */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + 0x10000);
    {
        uint64_t a = TEST_MEM_BASE + 0x1000, b = TEST_MEM_BASE + 0x2000,
                 c = TEST_MEM_BASE + 0x3000;
        mem_write_u32(a, 100u); mem_write_u32(b, 200u); mem_write_u32(c, 100u);
        /* 精确搜索 100 → 两候选；扣血后 a→75 (Δ-25), c→80 (Δ-20) */
        results_init(&r, 0);
        search_exact(NULL, 0, &r, VT_DWORD, 100, 0);
        mem_write_u32(a, 75u); mem_write_u32(c, 80u);
        int64_t n = search_refine_exact_by(NULL, 0, &r, REFINE_DECREASED_BY, 25.0);
        check(n == 1 && results_has(&r, a),
              "13) decreased_by=25: 只留 Δ 恰为 -25 的 a（c 的 -20 被剔除）");
        results_clear(&r);

        /* 增量方向: b 增 50 */
        mem_write_u32(a, 100u); mem_write_u32(b, 200u);
        results_init(&r, 0);
        search_exact(NULL, 0, &r, VT_DWORD, 200, 0);
        mem_write_u32(b, 250u);
        n = search_refine_exact_by(NULL, 0, &r, REFINE_INCREASED_BY, 50.0);
        check(n == 1 && results_has(&r, b), "13) increased_by=50: 命中增 50 的 b");
        results_clear(&r);

        /* float 容差: 100.1f → 90.1f, Δ=10.0 (double 域有转换误差, 容差须容忍) */
        float f0 = 100.1f, f1 = 90.1f;
        memcpy(g_mem + (a - TEST_MEM_BASE), &f0, 4);
        results_init(&r, 0);
        {
            uint64_t fv;
            memcpy(&fv, &f0, 4);
            search_exact(NULL, 0, &r, VT_FLOAT, fv, 0);
        }
        memcpy(g_mem + (a - TEST_MEM_BASE), &f1, 4);
        n = search_refine_exact_by(NULL, 0, &r, REFINE_DECREASED_BY, 10.0);
        check(n == 1 && results_has(&r, a),
              "13) float Δ=10.0 容差命中（100.1f→90.1f）");
        results_clear(&r);
    }

    /* 模糊路径: unknown_init 基线 + decreased_by */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + 0x10000);
    {
        uint64_t a = TEST_MEM_BASE + 0x1000, b = TEST_MEM_BASE + 0x2000;
        mem_write_u32(a, 100u); mem_write_u32(b, 100u);
        results_init(&r, 0);
        search_unknown_init(NULL, 0, &r, VT_DWORD, 0);
        mem_write_u32(a, 75u); mem_write_u32(b, 70u);
        int64_t kept = search_refine_by(NULL, 0, &r, REFINE_DECREASED_BY, 25.0);
        check(kept == 1, "13) 模糊 decreased_by=25: 只留 a（b 的 -30 剔除）");
        results_clear(&r);
    }

    /* === 14) 结果集 save/load round-trip + 文件名校验 === */
    mem_reset(); g_nregions = 0; region_add(TEST_MEM_BASE, TEST_MEM_BASE + 0x10000);
    {
        uint64_t a = TEST_MEM_BASE + 0x1000;
        mem_write_u32(a, 0xCAFEBABEu);
        results_init(&r, 0);
        search_exact(NULL, 0, &r, VT_DWORD, 0xCAFEBABEu, 0);
        r.exact_vals[0] = 0x12345678ull;   /* 模拟"最近一轮值"≠搜索值 */
        char *json = results_serialize(&r, VT_DWORD);
        check(json != NULL, "14) serialize 非空");
        /* round-trip 到新结果集 */
        gg_results_t r2; memset(&r2, 0, sizeof(r2));
        int64_t n = results_deserialize(&r2, json);
        free(json);
        check(n == 1 && r2.count == 1, "14) deserialize 恢复 1 条");
        check(r2.addrs[0] == a, "14) 地址一致");
        check(r2.exact_vt == VT_DWORD && r2.exact_vals_valid == 1,
              "14) 类型与基线标记恢复 (refine 可直接用)");
        check(r2.exact_vals[0] == 0x12345678ull, "14) 最近一轮值一致");
        results_clear(&r2);
        results_clear(&r);

        /* 坏输入必须拒绝（不许半边加载） */
        gg_results_t r3; memset(&r3, 0, sizeof(r3));
        check(results_deserialize(&r3, "{}") == -1, "14) 缺字段拒绝");
        check(results_deserialize(&r3, "not json") == -1, "14) 非 JSON 拒绝");
        check(results_deserialize(&r3,
              "{\"vtype\":3,\"count\":2,\"addrs\":[\"0x1000\"],\"vals\":[\"0x1\",\"0x2\"]}")
              == -1, "14) addrs/vals 长度不一致拒绝");
        check(results_deserialize(&r3,
              "{\"vtype\":99,\"count\":1,\"addrs\":[\"0x1000\"],\"vals\":[\"0x1\"]}")
              == -1, "14) 非法 vtype 拒绝");
        results_clear(&r3);

        /* 文件名校验：路径穿越与控制字符必须挡掉 */
        check(gg_safe_result_name("hp_stage1") == 1, "14) 正常名通过");
        check(gg_safe_result_name("a.b-c_1") == 1, "14) 点横线下划线通过");
        check(gg_safe_result_name("../etc/passwd") == 0, "14) ../ 路径穿越拒绝");
        check(gg_safe_result_name("a/b") == 0, "14) 斜杠拒绝");
        check(gg_safe_result_name("") == 0, "14) 空名拒绝");
        check(gg_safe_result_name("has space") == 0, "14) 空格拒绝");
    }

    {
        /* === 15) 多级指针链反向搜索（gg_pointer_path） ===
         *
         * 造一条三级链：
         *   [M+0x0100] = M+0x2200      ← 锚点（在 libtest.so 段内）
         *   [M+0x2200] = M+0x2300      ← obj1
         *   [M+0x2318] = M+0x3000      ← obj2 + 0x18
         *   [M+0x3024] = 999           ← 目标字段（obj3 + 0x24）
         *
         * 期望 nodes[0]=M+0x0100(浅) → nodes[1]=M+0x2200 → nodes[2]=M+0x2318，
         * offsets: 0 / 0x18 / 0x24。 */
        mem_reset(); g_nregions = 0;
        region_add(TEST_MEM_BASE, TEST_MEM_BASE + TEST_MEM_SZ);
        mem_write_u64(TEST_MEM_BASE + 0x0100, TEST_MEM_BASE + 0x2200);
        mem_write_u64(TEST_MEM_BASE + 0x2200, TEST_MEM_BASE + 0x2300);
        mem_write_u64(TEST_MEM_BASE + 0x2318, TEST_MEM_BASE + 0x3000);
        mem_write_u32(TEST_MEM_BASE + 0x3024, 999);

        gg_map_region_t anchor;
        memset(&anchor, 0, sizeof(anchor));
        anchor.start = TEST_MEM_BASE;
        anchor.end   = TEST_MEM_BASE + 0x1000;
        snprintf(anchor.perms, sizeof(anchor.perms), "rw-p");
        snprintf(anchor.path, sizeof(anchor.path), "libtest.so");

        gg_pointer_chain_t ch[32];
        size_t nc = pointer_chain_scan(NULL, 0, TEST_MEM_BASE + 0x3024,
                                       VT_QWORD, 4, 0x400, &anchor, 1, ch, 32);
        int found3 = 0;
        for (size_t i = 0; i < nc; i++) {
            if (ch[i].depth != 3) continue;
            if (ch[i].nodes[0].addr != TEST_MEM_BASE + 0x0100) continue;
            if (ch[i].nodes[1].addr != TEST_MEM_BASE + 0x2200) continue;
            if (ch[i].nodes[2].addr != TEST_MEM_BASE + 0x2318) continue;
            found3 = 1;
            check(ch[i].nodes[0].offset == 0,     "15) 链 offset[0] = 0（obj1 首址）");
            check(ch[i].nodes[1].offset == 0x18,  "15) 链 offset[1] = 0x18");
            check(ch[i].nodes[2].offset == 0x24,  "15) 链 offset[2] = 0x24（字段偏移）");
            check(ch[i].nodes[2].value == TEST_MEM_BASE + 0x3000, "15) 最深层 value = obj3 首址");
            break;
        }
        check(found3, "15) 三级链命中（锚点 → obj1 → obj2 → 字段）");

        /* 锚点段不含 M+0x0100：链应被过滤掉（同一条链，锚点条件不满足） */
        anchor.start = TEST_MEM_BASE + 0x4000;
        anchor.end   = TEST_MEM_BASE + 0x5000;
        nc = pointer_chain_scan(NULL, 0, TEST_MEM_BASE + 0x3024,
                                VT_QWORD, 4, 0x400, &anchor, 1, ch, 32);
        check(nc == 0, "15b) 锚点段不覆盖链首 → 0 条");

        /* 不给锚点（nanchors=0）时不过滤：至少能出一条（最深、仅 1 级也可） */
        nc = pointer_chain_scan(NULL, 0, TEST_MEM_BASE + 0x3024,
                                VT_QWORD, 4, 0x400, NULL, 0, ch, 32);
        int has_any = 0;
        for (size_t i = 0; i < nc; i++) if (ch[i].nodes[0].addr == TEST_MEM_BASE + 0x2318) has_any = 1;
        check(nc > 0 && has_any, "15c) 无锚点过滤时输出链（含最深一级 M+0x2318）");

        /* lookback=0（精确）：每级都是偏移指向，链应当断在第一级 */
        nc = pointer_chain_scan(NULL, 0, TEST_MEM_BASE + 0x3024,
                                VT_QWORD, 4, 0, &anchor, 1, ch, 32);
        check(nc == 0, "15d) lookback=0 精确模式：偏移链断掉 → 0 条");

        /* 参数防御：dw 之外的宽度、depth=0、target=0 都返回 0 */
        check(pointer_chain_scan(NULL, 0, TEST_MEM_BASE + 0x3024, VT_BYTE, 3, 0x400, NULL, 0, ch, 32) == 0,
              "15e) 非 dword/qword 宽度拒绝");
        check(pointer_chain_scan(NULL, 0, 0, VT_QWORD, 3, 0x400, NULL, 0, ch, 32) == 0,
              "15f) target=0 拒绝");
        nc = pointer_chain_scan(NULL, 0, TEST_MEM_BASE + 0x3024, VT_QWORD, 1, 0x400, NULL, 0, ch, 32);
        check(nc == 1 && ch[0].depth == 1 && ch[0].nodes[0].addr == TEST_MEM_BASE + 0x2318,
              "15g) depth=1 只回溯一级");
    }

    printf(fails ? "\n== FAILED: %d ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
