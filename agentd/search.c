/* search.c — 内存搜索引擎实现
 *
 * 精确/区间搜索：分块读 + 逐字节模式匹配（任意对齐）。
 * 模糊搜索：位图（1 bit/元素）+ 全量快照；refine 逐元素数值比较收缩。
 * 快照内存上限 GG_FUZZY_MAX_BYTES（默认 256MB），超出部分不纳入扫描。
 */
#include "search.h"
#include "process.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* 内存页大小（用于搜索时容错跳过不可读页，见下方各 got<=0 分支，C2） */
#define GG_PAGE_SIZE 4096u
#define GG_PAGE_MASK  (GG_PAGE_SIZE - 1u)

/* 故障推进：恒逐页前进（最多漏一页 4KB，C2 语义：同 region 内故障页之后的可读字节
 * 不可漏）。X4 曾尝试"连续 64 页失败后大步 1MB 跳过"，但大步会过冲混合 region
 * （可读+连续故障，如 rw-p 堆中段未驻留洞）末端可读页，违背 C2"两端可读都找到"；
 * 真实性能问题已由根因优化解决：region_matches 排除 ---p(PROT_NONE) 区域
 * （zygote64 实测 12.9GB/17GB 是 PROT_NONE，搜索它们纯浪费，782 万次驱动读
 *   → 过滤后大幅下降）。fail_streak 参数保留以最小化调用处改动。 */
static inline uint64_t gg_fault_advance(uint64_t addr, uint64_t len, size_t rd,
                                        int *fail_streak) {
    (void)len; (void)fail_streak;
    uint64_t adv = GG_PAGE_SIZE - (addr & GG_PAGE_MASK);
    if (adv > rd) adv = rd;
    return adv;
}

/* ---------- 内部工具 ---------- */

int vt_size(int vtype) {
    switch (vtype) {
    case VT_BYTE:   return 1;
    case VT_WORD:   return 2;
    case VT_DWORD:  return 4;
    case VT_QWORD:  return 8;
    case VT_FLOAT:  return 4;
    case VT_DOUBLE: return 8;
    default:        return 0;
    }
}

/* 把 uint64 数值编码为内存字节（LE）。float/double 按位模式编码。 */
void encode_value_local(int vtype, uint64_t value, uint8_t out[8]) {
    memset(out, 0, 8);
    int sz = vt_size(vtype);
    if (vtype == VT_FLOAT) {
        float f;
        memcpy(&f, &value, 4);
        memcpy(out, &f, 4);
    } else if (vtype == VT_DOUBLE) {
        double d;
        memcpy(&d, &value, 8);
        memcpy(out, &d, 8);
    } else {
        memcpy(out, &value, (size_t)sz);
    }
}

static void encode_value(int vtype, uint64_t value, uint8_t out[8]) {
    encode_value_local(vtype, value, out);
}

/* 按类型把内存字节读成数值 */
static double read_as_double(const uint8_t *p, int vtype) {
    switch (vtype) {
    case VT_BYTE:   return (double)(*p);
    case VT_WORD:   { uint16_t v; memcpy(&v, p, 2); return (double)v; }
    case VT_DWORD:  { uint32_t v; memcpy(&v, p, 4); return (double)v; }
    case VT_QWORD:  { uint64_t v; memcpy(&v, p, 8); return (double)v; }
    case VT_FLOAT:  { float v; memcpy(&v, p, 4); return (double)v; }
    case VT_DOUBLE: { double v; memcpy(&v, p, 8); return v; }
    default:        return 0.0;
    }
}

uint64_t read_bits(const uint8_t *p, int vtype) {
    uint64_t v = 0;
    switch (vtype) {
    case VT_BYTE:   v = *p; break;
    case VT_WORD:   memcpy(&v, p, 2); break;
    case VT_DWORD:  memcpy(&v, p, 4); break;
    case VT_QWORD:  memcpy(&v, p, 8); break;
    case VT_FLOAT:  memcpy(&v, p, 4); break;
    case VT_DOUBLE: memcpy(&v, p, 8); break;
    default: break;
    }
    return v;
}

/* 位图操作 */
static inline int bm_get(const uint8_t *bm, uint64_t i) { return (bm[i >> 3] >> (i & 7)) & 1; }
static inline void bm_clr(uint8_t *bm, uint64_t i)      { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static uint64_t bm_count(const uint8_t *bm, uint64_t bits) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < bits; i++) n += (uint64_t)bm_get(bm, i);
    return n;
}

uint64_t results_fuzzy_count(const gg_results_t *r) {
    if (!r->fuzzy_active || !r->fuzzy_bitmap) return 0;
    return bm_count(r->fuzzy_bitmap, r->fuzzy_elem_count);
}

/* ---------- 结果集 ---------- */

void results_init(gg_results_t *r, size_t cap) {
    memset(r, 0, sizeof(*r));
    r->cap = cap ? cap : GG_RESULT_CAP_DEFAULT;
}

void results_clear(gg_results_t *r) {
    free(r->addrs);
    free(r->exact_vals);
    free(r->fuzzy_bitmap);
    free(r->fuzzy_snapshot);
    free(r->fuzzy_regions);
    memset(r, 0, sizeof(*r));
}

static int results_push(gg_results_t *r, uint64_t addr) {
    if (r->count >= r->cap) return -1;   /* 截断 */
    if (!r->addrs) {
        r->addrs = malloc(r->cap * sizeof(uint64_t));
        if (!r->addrs) return -1;
    }
    r->addrs[r->count++] = addr;
    return 0;
}

/* ---------- 精确搜索 ---------- */

int64_t search_exact(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     int vtype, uint64_t value, int region_type) {
    int sz = vt_size(vtype);
    if (sz <= 0) return -1;

    size_t nreg = 0;
    gg_map_region_t *regs = process_search_regions(pid, &nreg, region_type);
    if (!regs) return 0;

    size_t keep_cap = r->cap ? r->cap : GG_RESULT_CAP_DEFAULT;
    results_clear(r);
    results_init(r, keep_cap);

    uint8_t pat[8];
    encode_value(vtype, value, pat);

    static uint8_t chunk[GG_SEARCH_CHUNK + 8];
    uint64_t found = 0;
    for (size_t k = 0; k < nreg; k++) {
        uint64_t addr = regs[k].start;
        uint64_t len = regs[k].end - regs[k].start;
        int fail_streak = 0;
        while (len > 0) {
            size_t rd = len > GG_SEARCH_CHUNK ? GG_SEARCH_CHUNK : (size_t)len;
            ssize_t got = driver_read(d, pid, addr, chunk, rd);
            if (got <= 0) {
                /* 不可读推进: 孤立故障页逐页(最多漏一页 4KB, C2); 连续 64 页失败大步跳过(X4) */
                uint64_t adv = gg_fault_advance(addr, len, rd, &fail_streak);
                addr += adv; len -= adv;
                continue;
            }
            fail_streak = 0;
            rd = (size_t)got;
            for (size_t i = 0; i + (size_t)sz <= rd; i++) {
                if (memcmp(chunk + i, pat, (size_t)sz) == 0) {
                    if (results_push(r, addr + i) < 0) { free(regs); return (int64_t)found; }
                    found++;
                }
            }
            len -= rd;
            addr += rd;
        }
    }
    free(regs);

    /* 记录精确搜索元数据（供 search_refine_exact 用）:
     * 搜索时所有候选的值都 == value，快照直接填 value 即可，无需逐地址读回。 */
    r->exact_vt = vtype;
    r->exact_value = value;
    if (r->count > 0) {
        r->exact_vals = malloc(r->count * sizeof(uint64_t));
        if (r->exact_vals) {
            for (size_t i = 0; i < r->count; i++) r->exact_vals[i] = value;
        }
    }
    return (int64_t)found;
}

/* ---------- AoB 特征码搜索 ---------- */

/* 解析 hex pattern: 支持 "48 65 4C 4C" / "48654C4C"（空格/逗号可选）。返回字节数; -1 解析失败。 */
int parse_hex_pattern(const char *s, uint8_t *out, size_t max) {
    size_t n = 0;
    if (!s) return -1;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == ',') s++;
        if (!*s) break;
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return -1;
        int hi = s[0] <= '9' ? s[0] - '0' : (s[0] | 0x20) - 'a' + 10;
        int lo = s[1] <= '9' ? s[1] - '0' : (s[1] | 0x20) - 'a' + 10;
        if (n >= max) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return (int)n;
}

/* AoB 搜索: 在区域内滑动匹配 pat[plen] 字节序列, 命中记录起始地址。
 * 结果条目值类型记 VT_BYTE。 */
int64_t search_aob(gg_driver_t *d, pid_t pid, gg_results_t *r,
                   const uint8_t *pat, size_t plen, int region_type) {
    if (!pat || plen == 0 || plen > 256) return -1;

    size_t nreg = 0;
    gg_map_region_t *regs = process_search_regions(pid, &nreg, region_type);
    if (!regs) return 0;

    size_t keep_cap = r->cap ? r->cap : GG_RESULT_CAP_DEFAULT;
    results_clear(r);
    results_init(r, keep_cap);

    static uint8_t chunk[GG_SEARCH_CHUNK + 256];
    uint64_t found = 0;
    for (size_t k = 0; k < nreg; k++) {
        uint64_t addr = regs[k].start;
        uint64_t len = regs[k].end - regs[k].start;
        int fail_streak = 0;
        while (len > 0) {
            size_t rd = len > GG_SEARCH_CHUNK ? GG_SEARCH_CHUNK : (size_t)len;
            ssize_t got = driver_read(d, pid, addr, chunk, rd);
            if (got <= 0) {
                uint64_t adv = gg_fault_advance(addr, len, rd, &fail_streak);
                addr += adv; len -= adv;
                continue;
            }
            fail_streak = 0;
            rd = (size_t)got;
            /* 块内滑动匹配; 块尾若可能跨块则补读补齐 */
            for (size_t i = 0; i + plen <= rd; i++) {
                if (memcmp(chunk + i, pat, plen) == 0) {
                    if (results_push(r, addr + i) < 0) { free(regs); return (int64_t)found; }
                    found++;
                }
            }
            /* 跨块特征: 块尾不足 plen 时, 若还有后续数据则补读 */
            if (rd >= plen && len > rd) {
                /* 检查块尾 plen-1 个字节处是否可能开始特征 */
                for (size_t i = rd - (plen - 1); i + plen <= rd + plen - 1 && i < rd; i++) {
                    size_t need = plen - (rd - i);
                    if (need == 0) continue;
                    ssize_t got2 = driver_read(d, pid, addr + i, chunk + i, need);
                    if (got2 > 0 && memcmp(chunk + i, pat, plen) == 0) {
                        if (results_push(r, addr + i) < 0) { free(regs); return (int64_t)found; }
                        found++;
                    }
                }
            }
            len -= rd;
            addr += rd;
        }
    }
    free(regs);

    r->exact_vt = VT_BYTE;
    r->exact_value = 0;
    if (r->count > 0) {
        r->exact_vals = malloc(r->count * sizeof(uint64_t));
        if (r->exact_vals) {
            for (size_t i = 0; i < r->count; i++) r->exact_vals[i] = 0;
        }
    }
    return (int64_t)found;
}

/* ---------- 区间搜索 ---------- */

/* 区间搜索：min <= value <= max。
 * 精度：整数类型（BYTE/WORD/DWORD/QWORD）走 u64 整数比较——read_as_double
 * 对 >2^53 的 u64 会丢精度（大数值区间搜索出错误结果）。float/double 走 double。 */
int64_t search_range(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     int vtype, double vmin, double vmax, int region_type) {
    int sz = vt_size(vtype);
    if (sz <= 0) return -1;

    int is_int = (vtype == VT_BYTE || vtype == VT_WORD ||
                  vtype == VT_DWORD || vtype == VT_QWORD);
    /* double 输入本身受 2^53 限制；但内存值比较必须走整数路径才不丢精度 */
    uint64_t ilo = (uint64_t)vmin, ihi = (uint64_t)vmax;

    size_t nreg = 0;
    gg_map_region_t *regs = process_search_regions(pid, &nreg, region_type);
    if (!regs) return 0;

    size_t keep_cap = r->cap ? r->cap : GG_RESULT_CAP_DEFAULT;
    results_clear(r);
    results_init(r, keep_cap);

    static uint8_t chunk[GG_SEARCH_CHUNK];
    uint64_t found = 0;
    for (size_t k = 0; k < nreg; k++) {
        uint64_t addr = regs[k].start;
        uint64_t len = regs[k].end - regs[k].start;
        int fail_streak = 0;
        while (len > 0) {
            size_t rd = len > GG_SEARCH_CHUNK ? GG_SEARCH_CHUNK : (size_t)len;
            ssize_t got = driver_read(d, pid, addr, chunk, rd);
            if (got <= 0) {
                /* 区间搜索同样做容错推进(见 search_exact 注释: C2 逐页 + X4 大步) */
                uint64_t adv = gg_fault_advance(addr, len, rd, &fail_streak);
                addr += adv; len -= adv;
                continue;
            }
            fail_streak = 0;
            rd = (size_t)got;
            if (is_int) {
                for (size_t i = 0; i + (size_t)sz <= rd; i++) {
                    uint64_t v = read_bits(chunk + i, vtype);
                    if (v >= ilo && v <= ihi) {
                        if (results_push(r, addr + i) < 0) { free(regs); return (int64_t)found; }
                        found++;
                    }
                }
            } else {
                for (size_t i = 0; i + (size_t)sz <= rd; i++) {
                    double v = read_as_double(chunk + i, vtype);
                    if (v >= vmin && v <= vmax) {
                        if (results_push(r, addr + i) < 0) { free(regs); return (int64_t)found; }
                        found++;
                    }
                }
            }
            len -= rd;
            addr += rd;
        }
    }
    free(regs);
    return (int64_t)found;
}

/* ---------- 模糊搜索 ---------- */

int64_t search_unknown_init(gg_driver_t *d, pid_t pid, gg_results_t *r,
                            int vtype, int region_type) {
    int sz = vt_size(vtype);
    if (sz <= 0) return -1;

    size_t nreg = 0;
    gg_map_region_t *regs = process_search_regions(pid, &nreg, region_type);
    if (!regs) return -1;

    size_t keep_cap = r->cap ? r->cap : GG_RESULT_CAP_DEFAULT;
    results_clear(r);
    results_init(r, keep_cap);

    /* 收集区域直到快照上限 */
    uint64_t total = 0;
    uint64_t n_used = 0;
    for (size_t k = 0; k < nreg; k++) {
        uint64_t l = regs[k].end - regs[k].start;
        if (total + l > GG_FUZZY_MAX_BYTES) {
            l = GG_FUZZY_MAX_BYTES - total;
            if (l < (uint64_t)sz) break;
        }
        total += l;
        n_used++;
        if (total >= GG_FUZZY_MAX_BYTES) break;
    }
    if (n_used == 0 || total == 0) { free(regs); return -1; }

    uint64_t elem_total = total / (uint64_t)sz;
    r->fuzzy_vt = vtype;
    r->fuzzy_active = 1;
    r->fuzzy_elem_count = elem_total;
    r->fuzzy_snapshot_len = (size_t)total;
    r->fuzzy_bitmap = calloc((elem_total + 7) / 8, 1);
    r->fuzzy_snapshot = malloc((size_t)total);
    r->fuzzy_regions = malloc(n_used * sizeof(fz_region_t));
    if (!r->fuzzy_bitmap || !r->fuzzy_snapshot || !r->fuzzy_regions) {
        results_clear(r);
        free(regs);
        return -1;
    }

    /* 全置候选（清尾部多余位） */
    memset(r->fuzzy_bitmap, 0xff, (elem_total + 7) / 8);
    if (elem_total % 8) {
        uint8_t mask = (uint8_t)((1u << (elem_total % 8)) - 1);
        r->fuzzy_bitmap[elem_total / 8] &= mask;
    }

    uint64_t snap_off = 0, elem_idx = 0;
    for (size_t k = 0; k < n_used; k++) {
        uint64_t l = regs[k].end - regs[k].start;
        if (snap_off + l > total) l = total - snap_off;
        fz_region_t *fr = &r->fuzzy_regions[k];
        fr->start = regs[k].start;
        fr->len = l;
        fr->elem_begin = elem_idx;
        fr->snap_off = snap_off;
        int fail_streak = 0;
        uint64_t off = 0;
        while (off < l) {
            size_t rd = (l - off) > GG_SEARCH_CHUNK ? GG_SEARCH_CHUNK : (size_t)(l - off);
            ssize_t got = driver_read(d, pid, regs[k].start + off, r->fuzzy_snapshot + snap_off + off, rd);
            if (got <= 0) {
                /* 故障推进(见 search_exact: C2 逐页 + X4 大步)；跳过区快照填 0 */
                uint64_t abs = regs[k].start + off;
                uint64_t adv = gg_fault_advance(abs, l - off, rd, &fail_streak);
                memset(r->fuzzy_snapshot + snap_off + off, 0, adv);
                off += adv;
                continue;
            }
            fail_streak = 0;
            if ((size_t)got < rd) memset(r->fuzzy_snapshot + snap_off + off + (size_t)got, 0, rd - (size_t)got);
            off += (size_t)got;
        }
        elem_idx += l / (uint64_t)sz;
        snap_off += l;
    }
    r->fuzzy_region_count = (size_t)n_used;
    free(regs);
    return (int64_t)elem_total;
}

int64_t search_refine(gg_driver_t *d, pid_t pid, gg_results_t *r, int op) {
    if (!r->fuzzy_active || !r->fuzzy_bitmap || !r->fuzzy_snapshot || !r->fuzzy_regions) return -1;
    int sz = vt_size(r->fuzzy_vt);
    if (sz <= 0) return -1;

    static uint8_t chunk[GG_SEARCH_CHUNK];
    for (size_t k = 0; k < r->fuzzy_region_count; k++) {
        fz_region_t *fr = &r->fuzzy_regions[k];
        uint64_t off = 0;
        int fail_streak = 0;
        while (off < fr->len) {
            size_t rd = (fr->len - off) > GG_SEARCH_CHUNK ? GG_SEARCH_CHUNK : (size_t)(fr->len - off);
            ssize_t got = driver_read(d, pid, fr->start + off, chunk, rd);
            if (got <= 0) {
                /* 模糊 refine 读: 容错推进(见 search_exact 注释: C2 逐页 + X4 大步) */
                uint64_t abs = fr->start + off;
                uint64_t adv = gg_fault_advance(abs, fr->len - off, rd, &fail_streak);
                off += adv;
                continue;
            }
            fail_streak = 0;
            rd = (size_t)got;
            uint64_t elem_in_chunk = rd / (uint64_t)sz;
            for (uint64_t e = 0; e < elem_in_chunk; e++) {
                uint64_t gi = fr->elem_begin + off / (uint64_t)sz + e;
                if (!bm_get(r->fuzzy_bitmap, gi)) continue;
                const uint8_t *cur = chunk + e * (size_t)sz;
                const uint8_t *prev = r->fuzzy_snapshot + fr->snap_off + off + e * (size_t)sz;
                double a = read_as_double(prev, r->fuzzy_vt);
                double b = read_as_double(cur, r->fuzzy_vt);
                int keep = 0;
                switch (op) {
                case REFINE_INCREASED: keep = (b > a); break;
                case REFINE_DECREASED: keep = (b < a); break;
                case REFINE_UNCHANGED: keep = (b == a); break;
                case REFINE_CHANGED:   keep = (b != a); break;
                default: keep = 0; break;
                }
                if (!keep) bm_clr(r->fuzzy_bitmap, gi);
            }
            off += rd;
        }
        /* 更新快照为该区域当前字节（下一轮比较基准） */
        off = 0;
        int fail_streak2 = 0;
        while (off < fr->len) {
            size_t rd = (fr->len - off) > GG_SEARCH_CHUNK ? GG_SEARCH_CHUNK : (size_t)(fr->len - off);
            ssize_t got = driver_read(d, pid, fr->start + off, r->fuzzy_snapshot + fr->snap_off + off, rd);
            if (got <= 0) {
                uint64_t abs = fr->start + off;
                uint64_t adv = gg_fault_advance(abs, fr->len - off, rd, &fail_streak2);
                memset(r->fuzzy_snapshot + fr->snap_off + off, 0, adv);
                off += adv;
                continue;
            }
            fail_streak2 = 0;
            if ((size_t)got < rd) memset(r->fuzzy_snapshot + fr->snap_off + off + (size_t)got, 0, rd - (size_t)got);
            off += (size_t)got;
        }
    }
    return (int64_t)bm_count(r->fuzzy_bitmap, r->fuzzy_elem_count);
}

/* 精确搜索 refine：候选在搜索时都 == exact_value（已入快照），
 * 逐地址读当前值对比快照，命中 op 的保留并更新快照（支持多轮 refine）。 */
int64_t search_refine_exact(gg_driver_t *d, pid_t pid, gg_results_t *r, int op) {
    if (!r->addrs || r->count == 0) return 0;
    if (r->exact_vt <= 0 || r->exact_vt > VT_DOUBLE) return -1;
    int sz = vt_size(r->exact_vt);
    if (sz <= 0) return -1;
    if (!r->exact_vals) {
        r->exact_vals = malloc(r->count * sizeof(uint64_t));
        if (!r->exact_vals) return -1;
        for (size_t i = 0; i < r->count; i++) r->exact_vals[i] = r->exact_value;
    }

    uint64_t *dst = r->addrs;
    uint64_t *dstv = r->exact_vals;
    size_t n = 0;
    uint8_t *bigbuf = malloc(GG_SEARCH_CHUNK);
    if (!bigbuf) return -1;
    for (size_t i = 0; i < r->count; ) {
        size_t seg_n = 0;
        uint64_t cur = r->addrs[i];
        while (i + seg_n < r->count && r->addrs[i + seg_n] == cur &&
               (seg_n + 1) * (size_t)sz <= GG_SEARCH_CHUNK) {
            seg_n++;
            cur += (uint64_t)sz;
        }
        ssize_t got = driver_read(d, pid, r->addrs[i], bigbuf, seg_n * (size_t)sz);
        if (got == (ssize_t)(seg_n * (size_t)sz)) {
            for (size_t j = 0; j < seg_n; j++) {
                uint64_t addr = r->addrs[i + j];
                double curv = read_as_double(bigbuf + j * (size_t)sz, r->exact_vt);
                double prev = read_as_double((const uint8_t *)&r->exact_vals[i + j], r->exact_vt);
                int keep = 0;
                switch (op) {
                case REFINE_INCREASED: keep = (curv > prev); break;
                case REFINE_DECREASED: keep = (curv < prev); break;
                case REFINE_UNCHANGED: keep = (curv == prev); break;
                case REFINE_CHANGED:   keep = (curv != prev); break;
                default: keep = 0; break;
                }
                if (keep) {
                    dst[n] = addr;
                    dstv[n] = read_bits(bigbuf + j * (size_t)sz, r->exact_vt);
                    n++;
                }
            }
        } else {
            /* 段读失败：逐地址回退（保持原有容错语义） */
            for (size_t j = 0; j < seg_n; j++) {
                uint64_t addr = r->addrs[i + j];
                uint8_t buf[8] = {0};
                if (driver_read(d, pid, addr, buf, (size_t)sz) != sz) continue;
                double curv = read_as_double(buf, r->exact_vt);
                double prev = read_as_double((const uint8_t *)&r->exact_vals[i + j], r->exact_vt);
                int keep = 0;
                switch (op) {
                case REFINE_INCREASED: keep = (curv > prev); break;
                case REFINE_DECREASED: keep = (curv < prev); break;
                case REFINE_UNCHANGED: keep = (curv == prev); break;
                case REFINE_CHANGED:   keep = (curv != prev); break;
                default: keep = 0; break;
                }
                if (keep) {
                    dst[n] = addr;
                    dstv[n] = read_bits(buf, r->exact_vt);
                    n++;
                }
            }
        }
        i += seg_n;
    }
    free(bigbuf);
    r->count = n;
    return (int64_t)n;
}

size_t results_materialize(gg_results_t *r) {
    if (!r->fuzzy_active) return r->count;
    if (!r->fuzzy_bitmap) return 0;
    size_t n = 0;
    int sz = vt_size(r->fuzzy_vt);
    if (sz <= 0) return 0;
    for (size_t k = 0; k < r->fuzzy_region_count && n < r->cap; k++) {
        fz_region_t *fr = &r->fuzzy_regions[k];
        uint64_t elems = fr->len / (uint64_t)sz;
        for (uint64_t e = 0; e < elems && n < r->cap; e++) {
            uint64_t gi = fr->elem_begin + e;
            if (bm_get(r->fuzzy_bitmap, gi)) {
                if (results_push(r, fr->start + e * (uint64_t)sz) < 0) break;
                n++;
            }
        }
    }
    /* 物化后释放模糊状态 */
    free(r->fuzzy_bitmap); r->fuzzy_bitmap = NULL;
    free(r->fuzzy_snapshot); r->fuzzy_snapshot = NULL;
    free(r->fuzzy_regions); r->fuzzy_regions = NULL;
    r->fuzzy_active = 0;
    r->fuzzy_region_count = 0;
    return n;
}

/* 批量读连续地址段：addrs[0..n) 中从 idx 起收集连续段（步长 sz，段长 ≤ GG_SEARCH_CHUNK），
 * 一次 driver_read 拉整段，逐项写入 vals[]。返回段内元素数；段读失败返回 0（调用方回退逐项）。 */
static size_t bulk_read_segment(gg_driver_t *d, pid_t pid, const uint64_t *addrs,
                                size_t n, size_t idx, int vtype,
                                uint8_t *bigbuf, uint64_t *vals) {
    int sz = vt_size(vtype);
    size_t seg_n = 0;
    uint64_t cur = addrs[idx];
    while (idx + seg_n < n && addrs[idx + seg_n] == cur &&
           (seg_n + 1) * (size_t)sz <= GG_SEARCH_CHUNK) {
        seg_n++;
        cur += (uint64_t)sz;
    }
    if (seg_n == 0) return 0;
    if (driver_read(d, pid, addrs[idx], bigbuf, seg_n * (size_t)sz) != (ssize_t)(seg_n * (size_t)sz))
        return 0;   /* 段读失败（页保护等）→ 调用方逐项回退 */
    for (size_t j = 0; j < seg_n; j++)
        vals[j] = read_bits(bigbuf + j * (size_t)sz, vtype);
    return seg_n;
}

int results_get_page(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     size_t offset, size_t limit,
                     uint64_t *out_addrs, uint64_t *out_values, int vtype, size_t *out_count) {
    int sz = vt_size(vtype);
    if (sz <= 0) return -1;
    if (r->fuzzy_active) results_materialize(r);
    size_t n = 0;
    uint8_t *bigbuf = malloc(GG_SEARCH_CHUNK);
    if (!bigbuf) return -1;
    for (size_t i = offset; i < r->count && n < limit; ) {
        size_t seg = bulk_read_segment(d, pid, r->addrs, r->count, i, vtype,
                                       bigbuf, out_values + n);
        if (seg > 0) {
            for (size_t j = 0; j < seg; j++)
                out_addrs[n + j] = r->addrs[i + j];
            n += seg;
            i += seg;
        } else {
            /* 段读失败：单地址回退（保持原有容错语义） */
            uint8_t buf[8] = {0};
            ssize_t got = driver_read(d, pid, r->addrs[i], buf, (size_t)sz);
            if (got >= (ssize_t)sz) out_values[n] = read_bits(buf, vtype);
            else out_values[n] = 0;
            out_addrs[n] = r->addrs[i];
            n++;
            i++;
        }
    }
    free(bigbuf);
    *out_count = n;
    return 0;
}

size_t results_set_values(gg_driver_t *d, pid_t pid, gg_results_t *r,
                          const uint64_t *addrs, size_t n, int vtype, uint64_t value) {
    int sz = vt_size(vtype);
    if (sz <= 0) return 0;
    uint8_t buf[8];
    encode_value(vtype, value, buf);
    size_t ok = 0;
    for (size_t i = 0; i < n; i++) {
        if (driver_write(d, pid, addrs[i], buf, (size_t)sz) == (ssize_t)sz) ok++;
    }
    (void)r;
    return ok;
}

/* ---------- 指针扫描 ---------- */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

size_t pointer_scan(gg_driver_t *d, pid_t pid, const uint64_t *targets, size_t ntargets,
                    int vtype, gg_pointer_hit_t *out, size_t cap) {
    int psz = vt_size(vtype);
    if ((vtype != VT_DWORD && vtype != VT_QWORD) || psz <= 0) return 0;
    if (ntargets == 0 || cap == 0) return 0;

    /* 目标去重排序（上限 20000 个目标，防扫描爆炸） */
    size_t tn = ntargets > 20000 ? 20000 : ntargets;
    uint64_t *sorted = malloc(tn * sizeof(uint64_t));
    if (!sorted) return 0;
    memcpy(sorted, targets, tn * sizeof(uint64_t));
    qsort(sorted, tn, sizeof(uint64_t), cmp_u64);
    size_t uniq = 0;
    for (size_t i = 0; i < tn; i++) {
        if (uniq == 0 || sorted[uniq - 1] != sorted[i]) sorted[uniq++] = sorted[i];
    }

    size_t nreg = 0;
    gg_map_region_t *regs = process_search_regions(pid, &nreg, REGION_WRITABLE);
    size_t hits = 0;
    static uint8_t chunk[GG_SEARCH_CHUNK];

    for (size_t k = 0; k < nreg && hits < cap; k++) {
        uint64_t addr = regs[k].start;
        uint64_t len = regs[k].end - regs[k].start;
        int fail_streak = 0;
        while (len > 0 && hits < cap) {
            size_t rd = len > GG_SEARCH_CHUNK ? GG_SEARCH_CHUNK : (size_t)len;
            ssize_t got = driver_read(d, pid, addr, chunk, rd);
            if (got <= 0) {
                /* 指针扫描同样做容错推进(见 search_exact 注释: C2 逐页 + X4 大步) */
                uint64_t adv = gg_fault_advance(addr, len, rd, &fail_streak);
                addr += adv; len -= adv;
                continue;
            }
            fail_streak = 0;
            rd = (size_t)got;
            size_t max_i = rd >= (size_t)psz ? rd - (size_t)psz : 0;
            for (size_t i = 0; i <= max_i; i += (size_t)psz) {   /* 对齐扫描 */
                uint64_t v = 0;
                memcpy(&v, chunk + i, (size_t)psz);
                if (v == 0) continue;
                if (bsearch(&v, sorted, uniq, sizeof(uint64_t), cmp_u64)) {
                    out[hits].container = addr + i;
                    out[hits].target = v;
                    out[hits].ptype = vtype;
                    hits++;
                    if (hits >= cap) break;
                }
            }
            len -= rd;
            addr += rd;
        }
    }
    free(sorted);
    free(regs);
    return hits;
}
