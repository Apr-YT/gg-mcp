/* search.c — 内存搜索引擎实现
 *
 * 精确/区间搜索：分块读 + 逐字节模式匹配（任意对齐）。
 * 模糊搜索：位图（1 bit/元素）+ 全量快照；refine 逐元素数值比较收缩。
 * 快照内存上限 GG_FUZZY_MAX_BYTES（默认 256MB），超出部分不纳入扫描。
 */
#include "search.h"
#include "process.h"
#include "third_party/cJSON.h"   /* results_deserialize 的 JSON 解析 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>      /* close()：pagemap fd 的回收在两个平台都要用 */
#if defined(__linux__)
#include <fcntl.h>       /* O_CLOEXEC 只在 Linux 上有 */
#endif

/* 并行搜索: region 分片, 每 worker 扫一段 region 子集 + 本地结果缓冲。
 * driver_read 为独立 ioctl, 线程安全; handler 级 g_eng.mtx 已保证搜索互斥。 */
#define GG_SEARCH_NTHREADS 4

/* 实现在 main.c。只在诊断路径用（init/refine 的读失败统计），不引入额外依赖。
 *
 * 这里给一个 **weak** 默认实现：单元测试往往只链接 search.c、没有 main.c，
 * 否则每个链接 search.c 的测试都得各补一份 shim（已经踩了两次）。
 * 产品构建里 main.c 的强符号会覆盖它。 */
__attribute__((weak)) void log_msg(const char *fmt, ...) { (void)fmt; }

/* 前向声明（定义在本文件后部, worker 先引用） */
static inline uint64_t gg_fault_advance(uint64_t addr, uint64_t len, size_t rd, int *fail_streak);
static double read_as_double(const uint8_t *p, int vtype);
static int results_push(gg_results_t *r, uint64_t addr);

typedef struct {
    gg_driver_t *d;
    pid_t pid;
    const gg_map_region_t *regs;
    size_t rbegin, rend;
    int vtype;
    int mode;              /* 0=exact 1=range 2=aob */
    uint8_t pat[256];      /* exact: 编码 pattern; aob: 原始 pattern */
    uint8_t pat_mask[256]; /* aob: 逐位掩码，1=比较该 bit；全 0xFF 表示无通配 */
    int     pat_has_wild;  /* aob: 1=含通配，走逐字节比较 */
    size_t pat_len;        /* aob 用 */
    uint64_t ilo, ihi;     /* range 整数边界 */
    double dlo, dhi;       /* range 浮点边界 */
    int aligned;           /* range: 1=只扫自然对齐位置（默认），0=逐字节 */
    uint64_t *hits;
    size_t hits_n, hit_cap;
} search_worker_t;

static inline int w_push(search_worker_t *w, uint64_t addr) {
    if (w->hits_n >= w->hit_cap) return -1;
    if (!w->hits) {
        w->hits = malloc(w->hit_cap * sizeof(uint64_t));
        if (!w->hits) return -1;
    }
    w->hits[w->hits_n++] = addr;
    return 0;
}

/* 内存页大小（用于搜索时容错跳过不可读页，见下方各 got<=0 分支，C2） */
#define GG_PAGE_SIZE 4096u
#define GG_PAGE_MASK  (GG_PAGE_SIZE - 1u)

/* ---- pagemap 驻留筛选（2026-10-23）----------------------------------------
 * 病灶：rw-p 堆里大量页是"映射了但从未 touch"，驱动读它们失败，而
 * gg_fault_advance 每次只前进一页 —— 一次全量搜索产生 25 万次 syscall
 * （驱动内实测 reads=258400 / pages=55002，平均每次只读到 0.21 页，
 *  而每页在驱动里只花 916ns：walk 357ns + 物理读 559ns）。
 * 所以瓶颈是请求次数，不是驱动内部。
 *
 * 用 /proc/<pid>/pagemap 的 present 位（bit63）批量判断：一次 pread
 * 8 字节/页、最多 64 页（256KB），把"逐页探测"变成"按驻留段整块读"。
 * 本机实测 present 准确（pfn 也没被屏蔽，但这里只用 present）。
 *
 * 为什么不用"连续失败就大步跳过"（X4 试过、已否决）：大步会过冲混合
 * region 末端可读页。pagemap 不猜 —— 只有确定为 absent 的页才跳，
 * 因此**不漏**：absent 页本来就读不到，跨它的模式匹配本来也不存在。
 * 打不开 pagemap 时（权限/内核配置），pmfd<0，退回原逐页探测逻辑。
 * ------------------------------------------------------------------------- */
#define GG_PM_BATCH_PAGES 64u

#if defined(__linux__)
/* 真实现只在 Linux/Android 上编译：它依赖 /proc/<pid>/pagemap、pread 和
 * O_CLOEXEC，主机上的 Windows 单测三者都没有。 */

static int pm_open(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/pagemap", (int)pid);
    return open(path, O_RDONLY | O_CLOEXEC);
}

/* 读 [va, va+npages*4K) 的 present 位，成功返回 0。 */
static int pm_read_vec(int fd, uint64_t va, uint64_t *vec, size_t npages) {
    off_t off = (off_t)((va >> 12) * 8);
    ssize_t want = (ssize_t)(npages * 8);
    ssize_t got = pread(fd, vec, (size_t)want, off);
    return got == want ? 0 : -1;
}

/* 从 va 起连续 present 的字节数（≤maxlen，最多 64 页）。0 = 首页 absent 或读失败。 */
static size_t pm_present_run(int fd, uint64_t va, size_t maxlen) {
    uint64_t vec[GG_PM_BATCH_PAGES];
    size_t npages = (maxlen + GG_PAGE_SIZE - 1) / GG_PAGE_SIZE;
    size_t n = 0, bytes;
    if (npages > GG_PM_BATCH_PAGES) npages = GG_PM_BATCH_PAGES;
    if (npages == 0) return 0;
    if (pm_read_vec(fd, va, vec, npages)) return 0;
    while (n < npages && ((vec[n] >> 63) & 1u)) n++;
    bytes = n * GG_PAGE_SIZE;
    return bytes > maxlen ? maxlen : bytes;
}

/* 跳过一段 absent，返回应前进的字节数（0 = 当前页 present，或 pagemap 读失败）。 */
static size_t pm_skip_absent(int fd, uint64_t va, size_t len) {
    uint64_t vec[GG_PM_BATCH_PAGES];
    size_t done = 0;
    while (done < len) {
        size_t npages = (len - done + GG_PAGE_SIZE - 1) / GG_PAGE_SIZE;
        size_t i;
        if (npages > GG_PM_BATCH_PAGES) npages = GG_PM_BATCH_PAGES;
        if (pm_read_vec(fd, va + done, vec, npages)) return done;
        for (i = 0; i < npages; i++) {
            if ((vec[i] >> 63) & 1u) return done + i * GG_PAGE_SIZE;
        }
        done += npages * GG_PAGE_SIZE;
    }
    return done;
}

/* 模糊路径（快照/refine）用：若 va 处是 absent，返回应跳过的字节数；
 * 返回 0 表示当前位置是 present（或 pagemap 不可用），调用方正常读。
 * 语义与 got<=0 分支一致 —— absent 段在快照里补 0、并从候选位图里清掉。 */
static size_t pm_try_skip(int pmfd, uint64_t va, size_t rd, size_t len) {
    size_t skip;
    if (pmfd < 0) return 0;
    if (pm_present_run(pmfd, va, rd) != 0) return 0;
    skip = pm_skip_absent(pmfd, va, len);
    if (skip == 0) skip = GG_PAGE_SIZE;
    return skip > len ? len : skip;
}

#else  /* !__linux__ —— 主机单测 */

/* 退化成"pagemap 不可用"：pm_open 返回 -1，三个查询函数返回 0。
 * 这条路径在设备上真实存在（pagemap 打不开时），单测跑的就是它，
 * 所以不是"假实现骗过测试"，而是**测 fallback 分支**。
 * 真正的 present 位判断只能靠真机 itest + 集合比对验证（见 AGENTS 30）。 */
static int pm_open(pid_t pid) { (void)pid; return -1; }
static size_t pm_present_run(int fd, uint64_t va, size_t maxlen) {
    (void)fd; (void)va; (void)maxlen;
    return 0;
}
static size_t pm_skip_absent(int fd, uint64_t va, size_t len) {
    (void)fd; (void)va; (void)len;
    return 0;
}
static size_t pm_try_skip(int pmfd, uint64_t va, size_t rd, size_t len) {
    (void)pmfd; (void)va; (void)rd; (void)len;
    return 0;
}

#endif /* __linux__ */

/* 单 worker 扫描 rbegin..rend 的 region 子集（模式由 w->mode 决定） */
static void *search_worker_main(void *arg) {
    search_worker_t *w = arg;
    gg_driver_t *d = w->d;
    int sz = vt_size(w->vtype);
    if (sz <= 0) return NULL;
    uint8_t *chunk = malloc(GG_SEARCH_CHUNK + 256);
    if (!chunk) return NULL;
    int pmfd = pm_open(w->pid);   /* <0 = pagemap 不可用，退回逐页探测 */
    uint64_t found = 0;
    for (size_t k = w->rbegin; k < w->rend; k++) {
        uint64_t addr = w->regs[k].start;
        uint64_t len = w->regs[k].end - w->regs[k].start;
        int fail_streak = 0;
        size_t carry = 0;
        while (len > 0) {
            size_t rd = len > GG_SEARCH_CHUNK ? GG_SEARCH_CHUNK : (size_t)len;
            ssize_t got;
            /* 驻留筛选：只对 present 的连续页发驱动读。absent 页直接跳过，
             * 并把 carry 清零（字节流断开，跨 absent 的匹配不存在）。 */
            if (pmfd >= 0) {
                size_t run = pm_present_run(pmfd, addr, rd);
                if (run == 0) {
                    size_t skip = pm_skip_absent(pmfd, addr, len);
                    if (skip == 0) skip = GG_PAGE_SIZE;   /* pagemap 读失败：保守前进一页 */
                    if (skip > len) skip = (size_t)len;
                    carry = 0;
                    addr += skip;
                    len -= skip;
                    continue;
                }
                if (run < rd) rd = run;
            }
            /* 所有模式都读进 chunk+carry：跨块匹配需要把上一块尾部保留下来 */
            got = driver_read(d, w->pid, addr, chunk + carry, rd);
            if (got <= 0) {
                carry = 0;
                uint64_t adv = gg_fault_advance(addr, len, rd, &fail_streak);
                addr += adv; len -= adv;
                continue;
            }
            fail_streak = 0;
            rd = (size_t)got;
            if (w->mode == 0) {
                /* exact: 首字节 memchr 预筛 + carry 跨块（原实现只在单块内扫
                 * i+sz<=rd，起始于块尾、剩余字节在下一块的匹配会整条漏掉） */
                size_t total = carry + rd;
                const uint8_t first = w->pat[0];
                for (size_t i = 0; i + (size_t)sz <= total; ) {
                    const uint8_t *hit = memchr(chunk + i, first, total - i - (size_t)sz + 1);
                    if (!hit) break;
                    size_t off = (size_t)(hit - chunk);
                    if (memcmp(hit, w->pat, (size_t)sz) == 0) {
                        if (w_push(w, addr - carry + off) < 0) { free(chunk); if (pmfd >= 0) close(pmfd); return NULL; }
                        found++;
                    }
                    i = off + 1;
                }
                size_t keep = total >= (size_t)sz ? (size_t)sz - 1 : total;
                if (keep) memmove(chunk, chunk + total - keep, keep);
                carry = keep;
            } else if (w->mode == 1) {
                size_t total = carry + rd;
                /* 步进策略：
                 *   aligned=1（默认）只扫自然对齐位置。逐字节扫会让一个 8 字节
                 *     值在区间内被命中 8 次（i..i+7 的未对齐窗口都落在区间里），
                 *     结果数虚高 8 倍、cap 秒满，命中的还是"未对齐窗口起点"。
                 *     ARM64 的 int/指针/float 天然按 sz 对齐，社区实现（CE/ptrscan）
                 *     默认也是对齐扫描。
                 *   aligned=0 保留逐字节。packed 结构、以及跨块边界的非对齐元素
                 *     需要它 —— 单测 9z 就是构造了一个 2 字节跨块的 dword。
                 *
                 * 对齐模式下起点要对齐到真实地址（addr-carry 是 chunk[0] 的真实
                 * 地址）：块尾 carry 进来时 chunk[0] 不再是 sz 的倍数，直接 i+=sz
                 * 会整体错位，漏掉下一个对齐元素。 */
                size_t step = w->aligned ? (size_t)sz : 1;
                size_t i = 0;
                if (w->aligned) {
                    uint64_t base = addr - carry;
                    size_t rem = (size_t)(base & (uint64_t)(sz - 1));
                    i = rem ? (size_t)sz - rem : 0;
                }
                if (w->vtype == VT_BYTE || w->vtype == VT_WORD ||
                    w->vtype == VT_DWORD || w->vtype == VT_QWORD) {
                    for (; i + (size_t)sz <= total; i += step) {
                        uint64_t v = read_bits(chunk + i, w->vtype);
                        if (v >= w->ilo && v <= w->ihi) {
                            if (w_push(w, addr - carry + i) < 0) { free(chunk); if (pmfd >= 0) close(pmfd); return NULL; }
                            found++;
                        }
                    }
                } else {
                    for (; i + (size_t)sz <= total; i += step) {
                        double v = read_as_double(chunk + i, w->vtype);
                        if (v >= w->dlo && v <= w->dhi) {
                            if (w_push(w, addr - carry + i) < 0) { free(chunk); if (pmfd >= 0) close(pmfd); return NULL; }
                            found++;
                        }
                    }
                }
                size_t keep = total >= (size_t)sz ? (size_t)sz - 1 : total;
                if (keep) memmove(chunk, chunk + total - keep, keep);
                carry = keep;
            } else {
                /* aob: carry 缓冲跨块匹配
                 *
                 * 无通配时走 memcmp（快）；有通配时逐字节按位比较 ——
                 * 社区签名大量使用半字节通配（如 frida's "?8 ?? ?? ?? 08 01"），
                 * 不支持通配等于一个签名都用不了。 */
                size_t total = carry + rd;
                if (!w->pat_has_wild) {
                    for (size_t i = 0; i + w->pat_len <= total; i++) {
                        if (memcmp(chunk + i, w->pat, w->pat_len) == 0) {
                            if (w_push(w, addr + i - carry) < 0) { free(chunk); if (pmfd >= 0) close(pmfd); return NULL; }
                            found++;
                        }
                    }
                } else {
                    for (size_t i = 0; i + w->pat_len <= total; i++) {
                        size_t j = 0;
                        for (; j < w->pat_len; j++) {
                            uint8_t m = w->pat_mask[j];
                            if (m && (chunk[i + j] & m) != w->pat[j]) break;
                        }
                        if (j == w->pat_len) {
                            if (w_push(w, addr + i - carry) < 0) { free(chunk); if (pmfd >= 0) close(pmfd); return NULL; }
                            found++;
                        }
                    }
                }
                size_t keep = total >= w->pat_len - 1 ? w->pat_len - 1 : total;
                if (keep) memmove(chunk, chunk + total - keep, keep);
                carry = keep;
            }
            len -= rd;
            addr += rd;
        }
    }
    free(chunk);
    if (pmfd >= 0) close(pmfd);
    w->hits_n = found;
    return NULL;
}

/* 并行执行: 调用方已填好 ws[i].rbegin/rend/hit_cap; 创建线程 → join → 合并到 r。
 * 返回合并总数; err 置 1 表示结果截断（达到 cap）。 */
static uint64_t search_run_parallel(search_worker_t *ws, pthread_t *ths, size_t nt,
                                    gg_results_t *r, int *err) {
    for (size_t i = 0; i < nt; i++) {
        ws[i].hits = NULL;
        ws[i].hits_n = 0;
        pthread_create(&ths[i], NULL, search_worker_main, &ws[i]);
    }
    for (size_t i = 0; i < nt; i++) pthread_join(ths[i], NULL);
    uint64_t total = 0;
    for (size_t i = 0; i < nt; i++) {
        for (size_t j = 0; j < ws[i].hits_n; j++) {
            if (results_push(r, ws[i].hits[j]) < 0) { *err = 1; break; }
            total++;
        }
        free(ws[i].hits);
    }
    return total;
}

/* 把 nreg 个 region 均分到 nt 个 worker（填入 rbegin/rend） */
static void search_split_regions(search_worker_t *ws, size_t nt, size_t nreg) {
    size_t per = nreg / nt, rem = nreg % nt;
    size_t begin = 0;
    for (size_t i = 0; i < nt; i++) {
        ws[i].rbegin = begin;
        begin += per + (i < rem ? 1 : 0);
        ws[i].rend = begin;
    }
}


/* 故障推进：恒逐页前进（最多漏一页 4KB，C2 语义：同 region 内故障页之后的可读字节
 * 不可漏）。X4 曾尝试"连续 64 页失败后大步 1MB 跳过"，但大步会过冲混合 region
 * （可读+连续故障，如 rw-p 堆中段未驻留洞）末端可读页，违背 C2"两端可读都找到"；
 * 真实性能问题已由根因优化解决：region_matches 排除 ---p(PROT_NONE) 区域
 * （zygote64 实测 12.9GB/17GB 是 PROT_NONE，搜索它们纯浪费，782 万次驱动读
 *   → 过滤后大幅下降）。fail_streak 参数保留以最小化调用处改动。
 *
 * ⚠ 2026-10-23 再修正：过滤 PROT_NONE 后仍有 25 万次 read —— 真因是 rw-p 堆里
 *   大量页"映射了但从未 touch"（absent），每次失败只前进一页。
 *   现在由上面的 pm_* 系列先用 pagemap 的 present 位批量判断并跳过，
 *   **正常路径不会再走到这里**；本函数只作为 pagemap 不可用时的兜底。
 *   别再试图把它改成"连续失败就大步跳过"（X4 否决过，会过冲混合 region）。 */
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

/* 把 [byte_off, byte_off+len) 覆盖的元素从候选里清掉。
 *
 * 用于「这段字节读不到」：读不到就无法比较，而原来只把快照填 0、位图原封不动，
 * 这些元素就成了**永久候选** —— 每轮 refine 都会失败、每轮都不会被剔除，
 * 结果集永远收敛不到。真机实测（登山赛车 / sleep）：256MB 可写区里大量页
 * 未驻留（rwproc 的 is_force=0 不换入页面），refine 后 remaining 卡在
 * 5300 万，几乎等于没细化。
 *
 * 取舍：宁可漏（暂时读不到的页不参与）也不要假候选 —— 假候选会让整个
 * 模糊搜索失去意义，而漏掉的页在页驻留后重新 init 就能拿回来。 */
static void bm_clear_span(uint8_t *bm, uint64_t elem_base, uint64_t byte_off,
                          uint64_t elem_total, uint64_t len, int sz) {
    uint64_t e0 = byte_off / (uint64_t)sz;
    uint64_t e1 = (byte_off + len + (uint64_t)sz - 1) / (uint64_t)sz;
    for (uint64_t e = e0; e < e1; e++) {
        uint64_t gi = elem_base + e;
        if (gi >= elem_total) break;
        bm_clr(bm, gi);
    }
}

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
    r->fuzzy_undo_head = -1;   /* memset 后是 0，会被误当成「有一级可回退」 */
    r->exact_undo_head = -1;
}

void results_clear(gg_results_t *r) {
    free(r->addrs);
    free(r->exact_vals);
    free(r->fuzzy_bitmap);
    free(r->fuzzy_snapshot);
    free(r->fuzzy_regions);
    for (int i = 0; i < GG_FUZZY_UNDO_DEPTH; i++) free(r->fuzzy_undo[i]);
    for (int i = 0; i < GG_FUZZY_UNDO_DEPTH; i++) {
        free(r->exact_undo[i].addrs);
        free(r->exact_undo[i].vals);
    }
    memset(r, 0, sizeof(*r));
    r->fuzzy_undo_head = -1;
    r->exact_undo_head = -1;
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

    uint64_t total = 0;
    if (nreg > 0) {
        size_t nt = nreg < GG_SEARCH_NTHREADS ? nreg : GG_SEARCH_NTHREADS;
        search_worker_t ws[GG_SEARCH_NTHREADS];
        pthread_t ths[GG_SEARCH_NTHREADS];
        for (size_t i = 0; i < nt; i++) {
            memset(&ws[i], 0, sizeof(ws[i]));
            ws[i].d = d; ws[i].pid = pid; ws[i].regs = regs;
            ws[i].vtype = vtype; ws[i].mode = 0;
            memcpy(ws[i].pat, pat, sizeof(pat));
            ws[i].hit_cap = keep_cap;
        }
        search_split_regions(ws, nt, nreg);
        int err = 0;
        total = search_run_parallel(ws, ths, nt, r, &err);
    }
    free(regs);

    /* 记录精确搜索元数据（供 search_refine_exact 用）:
     * 搜索时所有候选的值都 == value，快照直接填 value 即可，无需逐地址读回。 */
    r->exact_vt = vtype;
    r->exact_value = value;
    r->exact_vals_valid = 1;
    if (r->count > 0) {
        r->exact_vals = malloc(r->count * sizeof(uint64_t));
        if (r->exact_vals) {
            for (size_t i = 0; i < r->count; i++) r->exact_vals[i] = value;
        }
    }
    return (int64_t)total;
}

/* ---------- 组合搜索（group search）----------
 *
 * "v1;v2;v3::span"：n 个值（同一类型）顺序无关地出现在 span 字节窗口内，
 * 对应真实世界里"血量/护盾/弹药在对象里并排摆放"。这是 GG 的招牌功能，
 * 也是 AI 用多次区间搜索近似不出来的（三次搜索各自几万候选，
 * 无法表达"它们挨着"这个约束）。
 *
 * 语义与实现要点：
 * - 锚点 = v1：结果集存的是 **v1 的地址**，refine/undo 全部按 v1 工作
 *   （GG 工作流：值变了 → refine 收缩；锚点语义让这条链路直接可用）。
 * - 窗口验证：对每个锚点候选 addr，读 [addr-span, addr+span+sz)，
 *   其余值各自找一个与锚点同对齐（x ≡ span mod sz）且未被占用的位置。
 *   "与锚点同对齐"是因为同一结构的字段按类型宽度对齐排放，
 *   顺带把窗口内检查次数压到 2*span/sz+1 个而不是逐字节。
 * - 重复值（"5;5;3::20"）靠位置占用保证对应不同槽位，不许一个位置
 *   同时认领两个值。
 * - exact_vals 是 addrs 的并行数组，压缩时必须同步。 */
int64_t search_group(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     const uint64_t *vals, size_t nvals, int vtype,
                     uint64_t span, int region_type) {
    int sz = vt_size(vtype);
    if (sz <= 0 || !vals || nvals < 2 || nvals > GG_GROUP_MAX) return -1;
    if (span < (uint64_t)sz || span > GG_GROUP_SPAN_MAX) return -1;

    /* 锚点搜索复用 search_exact（含结果集元数据） */
    int64_t total = search_exact(d, pid, r, vtype, vals[0], region_type);
    if (total <= 0) return total;

    uint8_t pats[GG_GROUP_MAX][8];
    for (size_t i = 0; i < nvals; i++) encode_value(vtype, vals[i], pats[i]);

    /* 窗口 = 锚点前后各 span 字节。2*span+sz <= 8KB+8B，栈外 malloc 一次复用。 */
    size_t win = (size_t)span * 2 + (size_t)sz;
    uint8_t *wbuf = malloc(win);
    if (!wbuf) return -1;   /* 不能把未验证的锚点当组合命中返回 */

    size_t kept = 0;
    for (size_t i = 0; i < r->count; i++) {
        uint64_t addr = r->addrs[i];
        if (addr < span) continue;                  /* 窗口起点下溢，跳过 */
        ssize_t got = driver_read(d, pid, addr - span, wbuf, win);
        if (got < (ssize_t)win) continue;           /* 窗口跨不可读页 */

        /* 位置占用表（窗口内偏移）。锚点在偏移 span 处，认领 vals[0]。 */
        size_t used[GG_GROUP_MAX];
        size_t nused = 0;
        used[nused++] = (size_t)span;
        int ok = 1;
        for (size_t j = 1; j < nvals && ok; j++) {
            int found = 0;
            /* 与锚点同对齐：x = span + k*sz, k ∈ [-span/sz, span/sz] */
            for (int64_t k = -(int64_t)(span / (uint64_t)sz);
                 k <= (int64_t)(span / (uint64_t)sz); k++) {
                int64_t x = (int64_t)span + k * sz;
                if (x < 0 || x + sz > (int64_t)win) continue;
                int occupied = 0;
                for (size_t u = 0; u < nused; u++)
                    if (used[u] == (size_t)x) { occupied = 1; break; }
                if (occupied) continue;
                if (!memcmp(wbuf + x, pats[j], (size_t)sz)) {
                    used[nused++] = (size_t)x;
                    found = 1;
                    break;
                }
            }
            if (!found) ok = 0;
        }
        if (ok) {
            /* 原地压缩，exact_vals 是并行数组必须同步 */
            r->addrs[kept] = addr;
            if (r->exact_vals) r->exact_vals[kept] = r->exact_vals[i];
            kept++;
        }
    }
    r->count = kept;
    free(wbuf);
    log_msg("group: %zu/%lld anchors kept (span=%llu, nvals=%zu)",
            kept, (long long)total,
            (unsigned long long)span, nvals);
    return (int64_t)kept;
}

/* 解析 hex pattern: 支持 "48 65 4C 4C" / "48654C4C"（空格/逗号可选）。返回字节数; -1 解析失败。 */
static inline int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int parse_hex_pattern_masked(const char *s, uint8_t *out, uint8_t *mask, size_t max) {
    size_t n = 0;
    if (!s) return -1;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == ',') s++;
        if (!*s) break;
        if (n >= max) return -1;

        int h = (s[0] == '?') ? -1 : hexval((unsigned char)s[0]);
        if (s[0] != '?' && h < 0) return -1;
        /* 单字符通配（"?" 后面直接是分隔符/结尾）：当成整字节通配 */
        if (s[0] == '?' && (s[1] == '\0' || s[1] == ' ' || s[1] == '\t' || s[1] == ',')) {
            out[n] = 0; mask[n] = 0x00; n++; s += 1; continue;
        }
        if (!*s || !s[1]) return -1;
        int l = (s[1] == '?') ? -1 : hexval((unsigned char)s[1]);
        if (s[1] != '?' && l < 0) return -1;

        /* 半字节组合成 pat + mask：
         *   8B → pat=0x8B mask=0xFF
         *   ?B → pat=0x0B mask=0x0F   （只比低半字节）
         *   8? → pat=0x80 mask=0xF0   （只比高半字节）
         *   ?? → pat=0x00 mask=0x00   （整字节通配） */
        uint8_t p = 0, m = 0;
        if (h >= 0) { p |= (uint8_t)(h << 4); m |= 0xF0; }
        if (l >= 0) { p |= (uint8_t)l;        m |= 0x0F; }
        out[n] = p; mask[n] = m; n++;
        s += 2;
    }
    return (int)n;
}

int parse_hex_pattern(const char *s, uint8_t *out, size_t max) {
    /* 旧接口：只接受纯 hex 字节。带通配的串交给 _masked 版本。 */
    uint8_t mask[256];
    int n = parse_hex_pattern_masked(s, out, mask, max);
    if (n <= 0) return n;
    for (int i = 0; i < n; i++)
        if (mask[i] != 0xFF) return -1;      /* 有通配 → 旧接口不认 */
    return n;
}

/* 字符串搜索的字节序列化（走 AoB 路径）。
 *
 * 为什么需要：搜玩家名/公会名/道具名是找对象头的经典入口，但把
 * "abc中文" 手工转成 UTF-16LE hex 极易错（大小端、终止符、代理对），
 * AI 调用方十次里错三次。转码放 agentd 侧做，调用方只发原始文本。
 *
 * utf16=0: UTF-8 原样字节；utf16=1: UTF-16LE（>0xFFFF 走代理对）。
 * append_nul=1 时附加终止符 —— 游戏内 C 字符串以 \0 收尾，
 * 不带终止符会命中长字符串中间的子串（"Gold" 命中 "GoldenSword"），
 * 带上才能把"完整等于这个词"的位置挑出来；要搜子串就传 0。
 * 返回字节数；非法 UTF-8 / 超 max 返回 -1。 */
int gg_str_to_pattern(const char *s, int utf16, int append_nul,
                      uint8_t *out, size_t max) {
    if (!s || !out) return -1;
    size_t n = 0;
    /* 单字节写入 helper：统一检查上限 */
#define PUT(b) do { if (n >= max) return -1; out[n++] = (uint8_t)(b); } while (0)

    while (*s) {
        unsigned char c = (unsigned char)*s;
        uint32_t cp;                     /* 解码出的码点 */
        int extra;                       /* 后续字节数 */
        if (c < 0x80)              { cp = c;          extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F;  extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F;  extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07;  extra = 3; }
        else return -1;                  /* 孤立的续字节 / 非法首字节 */
        if (extra == 0 && c >= 0x80) return -1;   /* 冗余编码（ASCII 用了多字节） */
        for (int i = 0; i < extra; i++) {
            unsigned char cc = (unsigned char)s[1 + i];
            if ((cc & 0xC0) != 0x80) return -1;   /* 续字节格式错 */
            cp = (cp << 6) | (cc & 0x3F);
        }
        /* 过长编码（如 0xC0 0x80 表示 U+0000）与超界码点 */
        if (cp > 0x10FFFF) return -1;
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000))
            return -1;
        /* UTF-16 保留区（代理对范围）在 UTF-8 里合法编码也不该出现 */
        if (cp >= 0xD800 && cp <= 0xDFFF) return -1;

        if (!utf16) {
            /* UTF-8 直拷：原字节就是合法序列，直接按长度拷 */
            for (int i = 0; i <= extra; i++) PUT((unsigned char)s[i]);
        } else {
            if (cp < 0x10000) {
                PUT(cp & 0xFF); PUT((cp >> 8) & 0xFF);          /* LE 低字节在前 */
            } else {
                cp -= 0x10000;
                uint32_t hi = 0xD800 + (cp >> 10);              /* 高代理 */
                uint32_t lo = 0xDC00 + (cp & 0x3FF);            /* 低代理 */
                PUT(hi & 0xFF); PUT((hi >> 8) & 0xFF);
                PUT(lo & 0xFF); PUT((lo >> 8) & 0xFF);
            }
        }
        s += 1 + extra;
    }
    if (append_nul) {
        PUT(0);
        if (utf16) PUT(0);             /* UTF-16 终止符是 2 字节 */
    }
#undef PUT
    return (int)n;
}

/* AoB 搜索: 在区域内滑动匹配 pat[plen] 字节序列, 命中记录起始地址。
 * 结果条目值类型记 VT_BYTE。 */
int64_t search_aob(gg_driver_t *d, pid_t pid, gg_results_t *r,
                   const uint8_t *pat, const uint8_t *mask,
                   size_t plen, int region_type) {
    if (!pat || plen == 0 || plen > 512) return -1;
    int has_wild = 0;
    if (mask) {
        for (size_t i = 0; i < plen; i++) if (mask[i] != 0xFF) { has_wild = 1; break; }
    }

    size_t nreg = 0;
    gg_map_region_t *regs = process_search_regions(pid, &nreg, region_type);
    if (!regs) return 0;

    size_t keep_cap = r->cap ? r->cap : GG_RESULT_CAP_DEFAULT;
    results_clear(r);
    results_init(r, keep_cap);

    uint64_t total = 0;
    if (nreg > 0) {
        size_t nt = nreg < GG_SEARCH_NTHREADS ? nreg : GG_SEARCH_NTHREADS;
        search_worker_t ws[GG_SEARCH_NTHREADS];
        pthread_t ths[GG_SEARCH_NTHREADS];
        for (size_t i = 0; i < nt; i++) {
            memset(&ws[i], 0, sizeof(ws[i]));
            ws[i].d = d; ws[i].pid = pid; ws[i].regs = regs;
            ws[i].vtype = VT_BYTE; ws[i].mode = 2;
            memcpy(ws[i].pat, pat, plen);
            if (mask) memcpy(ws[i].pat_mask, mask, plen);
            else memset(ws[i].pat_mask, 0xFF, plen);
            ws[i].pat_has_wild = has_wild;
            ws[i].pat_len = plen;
            ws[i].hit_cap = keep_cap;
        }
        search_split_regions(ws, nt, nreg);
        int err = 0;
        total = search_run_parallel(ws, ths, nt, r, &err);
    }
    free(regs);

    /* AoB 命中的字节各不相同（就是 pattern 本身，含通配符），不能像精确搜索
     * 那样拿单一值填快照——旧实现填全 0，之后 REFINE_UNCHANGED 会把"当前
     * 恰好为 0"误判成不变。exact_vt=VT_BYTE 只为 get_results 定读值宽度；
     * 基线由 refine 第一轮建立（exact_vals_valid=0）。 */
    r->exact_vt = VT_BYTE;
    r->exact_value = 0;
    r->exact_vals_valid = 0;
    return (int64_t)total;
}

/* ---------- 区间搜索 ---------- */

/* 区间搜索：min <= value <= max。
 * 精度：整数类型（BYTE/WORD/DWORD/QWORD）走 u64 整数比较——read_as_double
 * 对 >2^53 的 u64 会丢精度（大数值区间搜索出错误结果）。float/double 走 double。 */
int64_t search_range(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     int vtype, double vmin, double vmax, int region_type, int aligned) {
    int sz = vt_size(vtype);
    if (sz <= 0) return -1;

    /* double 输入本身受 2^53 限制；worker 内整数类型走 u64 比较不丢精度。
     *
     * 负数下界必须钳掉：worker 比的是 u64，(uint64_t)(-1.0) 会变成 0xFFFF...
     * 于是 [min,max] 静默退化成"只匹配 ≥2^64-1"，调用方看到 0 条还以为区间没问题。
     * vmax<0 则是空区间 —— 置成 ilo>ihi 走完整流程（照样清旧结果），不特判返回。 */
    int is_float = (vtype == VT_FLOAT || vtype == VT_DOUBLE);
    uint64_t ilo = 0, ihi = 0;
    if (!is_float) {
        if (vmax < 0) {
            ilo = 1; ihi = 0;                       /* 空区间：必然 0 命中 */
        } else {
            double lo = vmin < 0 ? 0 : vmin;
            double hi = vmax >= 18446744073709551616.0 ? 18446744073709551615.0 : vmax;
            ilo = (uint64_t)lo;
            ihi = (uint64_t)hi;
        }
    }

    size_t nreg = 0;
    gg_map_region_t *regs = process_search_regions(pid, &nreg, region_type);
    if (!regs) return 0;

    size_t keep_cap = r->cap ? r->cap : GG_RESULT_CAP_DEFAULT;
    results_clear(r);
    results_init(r, keep_cap);

    uint64_t total = 0;
    if (nreg > 0) {
        size_t nt = nreg < GG_SEARCH_NTHREADS ? nreg : GG_SEARCH_NTHREADS;
        search_worker_t ws[GG_SEARCH_NTHREADS];
        pthread_t ths[GG_SEARCH_NTHREADS];
        for (size_t i = 0; i < nt; i++) {
            memset(&ws[i], 0, sizeof(ws[i]));
            ws[i].d = d; ws[i].pid = pid; ws[i].regs = regs;
            ws[i].vtype = vtype; ws[i].mode = 1;
            ws[i].aligned = aligned;
            ws[i].ilo = ilo; ws[i].ihi = ihi;
            ws[i].dlo = vmin; ws[i].dhi = vmax;
            ws[i].hit_cap = keep_cap;
        }
        search_split_regions(ws, nt, nreg);
        int err = 0;
        total = search_run_parallel(ws, ths, nt, r, &err);
    }
    free(regs);

    /* 只定类型不定基线：区间搜索的候选值各不相同，无法像精确搜索那样拿单一
     * 值填快照。exact_vt 在此设置是为了 h_get_results 按 vtype 宽度读值
     * （旧实现不设 → 永远按 dword 读，qword 指针只显示低 32 位）。
     * 基线由 refine 第一轮建立（exact_vals_valid=0）。 */
    r->exact_vt = vtype;
    r->exact_value = 0;
    r->exact_vals_valid = 0;
    return (int64_t)total;
}

/* ---------- 模糊搜索 ---------- */

int64_t search_unknown_init(gg_driver_t *d, pid_t pid, gg_results_t *r,
                            int vtype, int region_type) {
    int sz = vt_size(vtype);
    if (sz <= 0) return -1;

    size_t nreg = 0;
    gg_map_region_t *regs = process_search_regions(pid, &nreg, region_type);
    if (!regs) return -1;
    int pmfd = pm_open(pid);   /* <0 = pagemap 不可用，退回逐页探测 */

    size_t keep_cap = r->cap ? r->cap : GG_RESULT_CAP_DEFAULT;
    results_clear(r);
    results_init(r, keep_cap);

    /* 收集区域直到快照上限。被截掉的部分记进 skipped_*（见 search.h 注释），
     * 由调用方透传给 UI/AI —— 静默截断 = 模糊搜索漏扫一大片还以为扫完了。
     * 放不下的大 region 跳过后继续尝试后面的（原实现直接 break，
     * 按地址序扫到一个巨大 region 就提前收工，哪怕后面还有能塞进的小 region）。 */
    uint64_t total = 0;
    uint64_t n_used = 0;
    uint64_t skipped_bytes = 0, skipped_from = 0;
    for (size_t k = 0; k < nreg; k++) {
        uint64_t l = regs[k].end - regs[k].start;
        if (total + l > GG_FUZZY_MAX_BYTES) {
            uint64_t take = GG_FUZZY_MAX_BYTES - total;
            if (take < (uint64_t)sz) {
                /* 整个 region 放不下：它一个字节都不纳入 */
                if (!skipped_from) skipped_from = regs[k].start;
                skipped_bytes += l;
                continue;
            }
            /* region 头部纳入、尾部截掉 */
            if (!skipped_from) skipped_from = regs[k].start + take;
            skipped_bytes += l - take;
            l = take;
        }
        total += l;
        n_used++;
        if (total >= GG_FUZZY_MAX_BYTES) {
            /* 之后的 region 全部跳过 */
            for (size_t j = k + 1; j < nreg; j++) {
                if (!skipped_from) skipped_from = regs[j].start;
                skipped_bytes += regs[j].end - regs[j].start;
            }
            break;
        }
    }
    r->fuzzy_skipped_bytes = skipped_bytes;
    r->fuzzy_skipped_from = skipped_from;
    if (skipped_bytes) {
        log_msg("unknown_init: 快照上限 %llu 字节，另有 %llu 字节（起 0x%llx）未纳入模糊搜索",
                (unsigned long long)GG_FUZZY_MAX_BYTES,
                (unsigned long long)skipped_bytes,
                (unsigned long long)skipped_from);
    }
    if (n_used == 0 || total == 0) { free(regs); if (pmfd >= 0) close(pmfd); return -1; }

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
        if (pmfd >= 0) close(pmfd);
        return -1;
    }

    /* 全置候选（清尾部多余位） */
    memset(r->fuzzy_bitmap, 0xff, (elem_total + 7) / 8);
    if (elem_total % 8) {
        uint8_t mask = (uint8_t)((1u << (elem_total % 8)) - 1);
        r->fuzzy_bitmap[elem_total / 8] &= mask;
    }

    uint64_t snap_off = 0, elem_idx = 0;
    uint64_t fail_pages = 0, fail_bytes = 0, short_reads = 0;
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
            size_t skip = pm_try_skip(pmfd, regs[k].start + off, rd, (size_t)(l - off));
            ssize_t got;
            if (skip) {
                /* absent 段：快照补 0 + 从候选里清掉（等价于原 got<=0 分支） */
                memset(r->fuzzy_snapshot + snap_off + off, 0, skip);
                bm_clear_span(r->fuzzy_bitmap, fr->elem_begin, off, elem_total, skip, sz);
                fail_pages++;
                fail_bytes += skip;
                off += skip;
                continue;
            }
            got = driver_read(d, pid, regs[k].start + off, r->fuzzy_snapshot + snap_off + off, rd);
            if (got <= 0) {
                /* 故障推进(见 search_exact: C2 逐页 + X4 大步)；跳过区快照填 0 */
                uint64_t abs = regs[k].start + off;
                uint64_t adv = gg_fault_advance(abs, l - off, rd, &fail_streak);
                memset(r->fuzzy_snapshot + snap_off + off, 0, adv);
                /* 读不到就不该当候选（否则永远是候选，见 bm_clear_span 注释） */
                bm_clear_span(r->fuzzy_bitmap, fr->elem_begin, off, elem_total, adv, sz);
                if (fail_pages < 3)
                    log_msg("fuzzy init: read fail at 0x%llx len=%zu off=%llu got=%zd",
                            (unsigned long long)abs, rd, (unsigned long long)off, got);
                fail_pages++;
                fail_bytes += adv;
                off += adv;
                continue;
            }
            fail_streak = 0;
            if ((size_t)got < rd) {
                /* 短读：只拿到前 got 字节（rwproc 遇到不可读页就停，实测请求
                 * 64KB 只回 8KB）。**不能清位图** —— 下一轮从 off+got 继续读，
                 * 没读到的部分还会被读到；清掉等于把可读元素误杀（曾经这么
                 * 写过，结果 unchanged 直接归零）。只把快照补 0，等下一轮覆盖。 */
                memset(r->fuzzy_snapshot + snap_off + off + (size_t)got, 0, rd - (size_t)got);
                short_reads++;
            }
            off += (size_t)got;
        }
        elem_idx += l / (uint64_t)sz;
        snap_off += l;
    }
    r->fuzzy_region_count = (size_t)n_used;
    /* 重新初始化 = 换了元素规模：旧的 undo 副本一律作废，否则 undo 会把
     * 上一轮搜索的位图盖到这一轮的 bitmap 上（长度可能还不一样）。 */
    for (int i = 0; i < GG_FUZZY_UNDO_DEPTH; i++) { free(r->fuzzy_undo[i]); r->fuzzy_undo[i] = NULL; }
    r->fuzzy_undo_bytes = 0;
    r->fuzzy_undo_head = -1;
    r->fuzzy_undo_count = 0;
    free(regs);
    if (pmfd >= 0) close(pmfd);
    /* 诊断：读不到的页会让候选被清掉，这里把规模记下来 —— 真机上
     * "refine 后 remaining 不动 / 立刻归零" 这两个相反症状都源于这块。 */
    if (fail_pages || short_reads)
        log_msg("fuzzy init: regions=%zu elems=%llu bytes=%llu fail_pages=%llu "
                "fail_bytes=%llu short_reads=%llu",
                n_used, (unsigned long long)elem_total, (unsigned long long)total,
                (unsigned long long)fail_pages, (unsigned long long)fail_bytes,
                (unsigned long long)short_reads);
    return (int64_t)elem_total;
}

/* 把当前位图拷一份进撤销栈（环形，深度 GG_FUZZY_UNDO_DEPTH）。
 * 分配失败只影响"这一步能否回退"，不影响 refine 本身。 */
static void fuzzy_undo_push(gg_results_t *r) {
    size_t bytes = (size_t)((r->fuzzy_elem_count + 7) / 8);
    if (!bytes || !r->fuzzy_bitmap) return;
    if (r->fuzzy_undo_bytes != bytes) {
        for (int i = 0; i < GG_FUZZY_UNDO_DEPTH; i++) { free(r->fuzzy_undo[i]); r->fuzzy_undo[i] = NULL; }
        r->fuzzy_undo_bytes = bytes;
        r->fuzzy_undo_head = -1;
        r->fuzzy_undo_count = 0;
    }
    int next = (r->fuzzy_undo_head + 1) % GG_FUZZY_UNDO_DEPTH;
    if (!r->fuzzy_undo[next]) {
        r->fuzzy_undo[next] = malloc(bytes);
        if (!r->fuzzy_undo[next]) return;
    }
    memcpy(r->fuzzy_undo[next], r->fuzzy_bitmap, bytes);
    r->fuzzy_undo_head = next;
    if (r->fuzzy_undo_count < GG_FUZZY_UNDO_DEPTH) r->fuzzy_undo_count++;
}

int64_t search_refine_undo(gg_results_t *r) {
    if (!r->fuzzy_active || !r->fuzzy_bitmap) return -1;
    if (r->fuzzy_undo_count <= 0 || r->fuzzy_undo_head < 0) return -1;
    uint8_t *slot = r->fuzzy_undo[r->fuzzy_undo_head];
    if (!slot) return -1;
    size_t bytes = (size_t)((r->fuzzy_elem_count + 7) / 8);
    memcpy(r->fuzzy_bitmap, slot, bytes);
    r->fuzzy_undo_head = (r->fuzzy_undo_head - 1 + GG_FUZZY_UNDO_DEPTH) % GG_FUZZY_UNDO_DEPTH;
    r->fuzzy_undo_count--;
    int64_t remain = (int64_t)bm_count(r->fuzzy_bitmap, r->fuzzy_elem_count);
    log_msg("fuzzy undo: restored %lld candidates (%d step(s) left)",
            (long long)remain, r->fuzzy_undo_count);
    return remain;
}

/* 增量比较（REFINE_*_BY）：|Δ| 与 by 的差落在相对容差内才保留。
 * float→double 转换 + double 减法对非二进小数有累积误差
 * （1.1f/0.1f 之类），精确等值会漏报；容差 1e-6 相对值覆盖 float
 * 精度又不会把相邻整数混进来（整数域 by 至少差 1，容差 << 1）。 */
static int refine_match_by(int op, double a, double b, double by) {
    double d = b - a;
    if (op == REFINE_DECREASED_BY) d = -d;
    double diff = d - by;
    double tol = 1e-6 * (fabs(by) + 1.0);
    return diff >= -tol && diff <= tol;
}

int64_t search_refine(gg_driver_t *d, pid_t pid, gg_results_t *r, int op) {
    return search_refine_by(d, pid, r, op, 0.0);
}

int64_t search_refine_by(gg_driver_t *d, pid_t pid, gg_results_t *r, int op, double by) {
    if (!r->fuzzy_active || !r->fuzzy_bitmap || !r->fuzzy_snapshot || !r->fuzzy_regions) return -1;
    int sz = vt_size(r->fuzzy_vt);
    if (sz <= 0) return -1;
    /* 读缓冲用堆分配而不是 static：static 缓冲的正确性完全依赖"所有 handler 都在
     * g_eng.mtx 内执行"这个隐式前提，护栏（check_rwproc_locking.py）只覆盖
     * driver.c 的 rwproc_buf，覆盖不到这里。一旦有人把某个 handler 挪出锁，
     * 表现不是报错而是"读到别的地址的数据"。每次 refine 一次 malloc 的代价可忽略。 */
    uint8_t *chunk = malloc(GG_SEARCH_CHUNK);
    if (!chunk) return -1;
    /* 先存走这一步之前的位图：refine 只减不增，搜错就没有回头路，
     * 只能重新 init（把整个可写区重读一遍）。 */
    fuzzy_undo_push(r);

    int pmfd = pm_open(pid);   /* <0 = pagemap 不可用，退回逐页探测 */
    uint64_t rf_fails = 0, cmp_n = 0, keep_n = 0;
    for (size_t k = 0; k < r->fuzzy_region_count; k++) {
        fz_region_t *fr = &r->fuzzy_regions[k];
        uint64_t off = 0;
        int fail_streak = 0;
        while (off < fr->len) {
            size_t rd = (fr->len - off) > GG_SEARCH_CHUNK ? GG_SEARCH_CHUNK : (size_t)(fr->len - off);
            size_t skip = pm_try_skip(pmfd, fr->start + off, rd, (size_t)(fr->len - off));
            ssize_t got;
            if (skip) {
                memset(r->fuzzy_snapshot + fr->snap_off + off, 0, skip);
                bm_clear_span(r->fuzzy_bitmap, fr->elem_begin, off,
                              r->fuzzy_elem_count, skip, sz);
                rf_fails++;
                off += skip;
                continue;
            }
            got = driver_read(d, pid, fr->start + off, chunk, rd);
            if (got <= 0) {
                /* 模糊 refine 读: 容错推进(见 search_exact 注释: C2 逐页 + X4 大步) */
                uint64_t abs = fr->start + off;
                uint64_t adv = gg_fault_advance(abs, fr->len - off, rd, &fail_streak);
                memset(r->fuzzy_snapshot + fr->snap_off + off, 0, adv);
                /* 读不到的段落必须从候选里清掉：否则这些元素每轮都失败、
                 * 每轮都保留，remaining 会卡住不动（真机实测卡在 5300 万）。 */
                bm_clear_span(r->fuzzy_bitmap, fr->elem_begin, off,
                              r->fuzzy_elem_count, adv, sz);
                if (rf_fails < 3)
                    log_msg("fuzzy refine: read fail at 0x%llx len=%zu off=%llu got=%zd",
                            (unsigned long long)abs, rd, (unsigned long long)off, got);
                rf_fails++;
                off += adv;
                continue;
            }
            fail_streak = 0;
            rd = (size_t)got;
            uint64_t elem_in_chunk = rd / (uint64_t)sz;
            for (uint64_t e = 0; e < elem_in_chunk; e++) {
                uint64_t gi = fr->elem_begin + off / (uint64_t)sz + e;
                if (!bm_get(r->fuzzy_bitmap, gi)) continue;
                cmp_n++;
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
                case REFINE_INCREASED_BY:
                case REFINE_DECREASED_BY: keep = refine_match_by(op, a, b, by); break;
                default: keep = 0; break;
                }
                if (keep) keep_n++;
                if (!keep) bm_clr(r->fuzzy_bitmap, gi);
            }
            /* 快照就地更新（读到的 chunk 即当前字节, 无需第二次读）:
             * 合并双读 → 每轮 refine 只读一次全量内存 */
            memcpy(r->fuzzy_snapshot + fr->snap_off + off, chunk, rd);
            off += rd;
        }
    }
    if (pmfd >= 0) close(pmfd);
    free(chunk);
    int64_t remain = (int64_t)bm_count(r->fuzzy_bitmap, r->fuzzy_elem_count);
    if (rf_fails || cmp_n)
        log_msg("fuzzy refine: op=%d fails=%llu compared=%llu kept=%llu remaining=%lld",
                op, (unsigned long long)rf_fails, (unsigned long long)cmp_n,
                (unsigned long long)keep_n, (long long)remain);
    return remain;
}

/* 精确搜索 refine：候选在搜索时都 == exact_value（已入快照），
 * 逐地址读当前值对比快照，命中 op 的保留并更新快照（支持多轮 refine）。 */
/* 精确 refine 前保存地址数组副本（环形栈）。分配失败只影响这次能否回退。 */
static void exact_undo_push(gg_results_t *r) {
    if (!r->addrs || r->count == 0) return;
    size_t bytes = r->count * sizeof(uint64_t);
    int next = (r->exact_undo_head + 1) % GG_FUZZY_UNDO_DEPTH;
    gg_exact_undo_t *u = &r->exact_undo[next];
    uint64_t *a = malloc(bytes);
    uint64_t *v = malloc(bytes);
    if (!a || !v) { free(a); free(v); return; }
    memcpy(a, r->addrs, bytes);
    if (r->exact_vals) memcpy(v, r->exact_vals, bytes);
    else memset(v, 0, bytes);
    free(u->addrs);
    free(u->vals);
    u->addrs = a;
    u->vals = v;
    u->count = r->count;
    r->exact_undo_head = next;
    if (r->exact_undo_depth < GG_FUZZY_UNDO_DEPTH) r->exact_undo_depth++;
}

int64_t search_refine_undo_exact(gg_results_t *r) {
    if (!r->addrs || r->exact_undo_depth <= 0 || r->exact_undo_head < 0) return -1;
    gg_exact_undo_t *u = &r->exact_undo[r->exact_undo_head];
    if (!u->addrs || u->count == 0) return -1;
    if (u->count > r->cap) return -1;   /* 容量不该变，防越界写 */
    size_t bytes = u->count * sizeof(uint64_t);
    if (!r->exact_vals) {
        r->exact_vals = malloc((size_t)r->cap * sizeof(uint64_t));
        if (!r->exact_vals) return -1;
    }
    memcpy(r->addrs, u->addrs, bytes);
    memcpy(r->exact_vals, u->vals, bytes);
    r->count = u->count;
    r->exact_undo_head = (r->exact_undo_head - 1 + GG_FUZZY_UNDO_DEPTH) % GG_FUZZY_UNDO_DEPTH;
    r->exact_undo_depth--;
    log_msg("exact undo: restored %zu candidates (%d step(s) left)",
            r->count, r->exact_undo_depth);
    return (int64_t)r->count;
}

int64_t search_refine_exact(gg_driver_t *d, pid_t pid, gg_results_t *r, int op) {
    return search_refine_exact_by(d, pid, r, op, 0.0);
}

int64_t search_refine_exact_by(gg_driver_t *d, pid_t pid, gg_results_t *r, int op, double by) {
    if (!r->addrs || r->count == 0) return 0;
    if (r->exact_vt <= 0 || r->exact_vt > VT_DOUBLE) return -1;
    int sz = vt_size(r->exact_vt);
    if (sz <= 0) return -1;

    /* 区间/AoB 搜索没有 t0 快照（候选值各不相同，单一值填充会全错）。
     * 本轮只建立基线：逐地址读当前值填 exact_vals，不按 op 筛选、
     * count 不变（读失败的候选被剔除——后续 refine 对它们同样读不到）。
     * 下一轮 refine 才有"变化"可比较。不 push undo（没发生筛选）。
     * 这意味着区间搜索后第一次 refine 是"建基线"，第二次起才收敛——
     * 与精确搜索（搜索即基线）不同，MCP 工具说明里已注明。 */
    if (!r->exact_vals_valid) {
        uint64_t *vals = malloc(r->count * sizeof(uint64_t));
        uint8_t *buf = malloc(GG_SEARCH_CHUNK);
        if (!vals || !buf) { free(vals); free(buf); return -1; }
        size_t n = 0;
        for (size_t i = 0; i < r->count; ) {
            size_t seg_n = 0;
            uint64_t cur = r->addrs[i];
            while (i + seg_n < r->count && r->addrs[i + seg_n] == cur &&
                   (seg_n + 1) * (size_t)sz <= GG_SEARCH_CHUNK) {
                seg_n++;
                cur += (uint64_t)sz;
            }
            ssize_t got = driver_read(d, pid, r->addrs[i], buf, seg_n * (size_t)sz);
            if (got == (ssize_t)(seg_n * (size_t)sz)) {
                for (size_t j = 0; j < seg_n; j++) {
                    r->addrs[n] = r->addrs[i + j];
                    vals[n] = read_bits(buf + j * (size_t)sz, r->exact_vt);
                    n++;
                }
            } else {
                for (size_t j = 0; j < seg_n; j++) {
                    uint8_t one[8] = {0};
                    if (driver_read(d, pid, r->addrs[i + j], one, (size_t)sz) != sz) continue;
                    r->addrs[n] = r->addrs[i + j];
                    memcpy(&vals[n], one, (size_t)sz);
                    n++;
                }
            }
            i += seg_n;
        }
        free(buf);
        free(r->exact_vals);
        r->exact_vals = vals;
        r->count = n;
        r->exact_vals_valid = 1;
        log_msg("refine: baseline established for %zu candidates (range/aob first refine)", n);
        return (int64_t)r->count;
    }

    if (!r->exact_vals) {
        r->exact_vals = malloc(r->count * sizeof(uint64_t));
        if (!r->exact_vals) return -1;
        for (size_t i = 0; i < r->count; i++) r->exact_vals[i] = r->exact_value;
    }
    /* 这一步会原地压缩 addrs（只减不增），先存副本供 undo */
    exact_undo_push(r);

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
                case REFINE_INCREASED_BY:
                case REFINE_DECREASED_BY: keep = refine_match_by(op, prev, curv, by); break;
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
                case REFINE_INCREASED_BY:
                case REFINE_DECREASED_BY: keep = refine_match_by(op, prev, curv, by); break;
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

/* 结果集内过滤：只保留**当前值**满足条件的条目（原地压缩，只减不增，可 undo）。
 *
 * 与 search_refine_exact 的分工：那个比「变化方向」，这个比「绝对值区间」。
 * 用途是搜出两万条之后把候选砍到几十条 —— 不做这一步，AI 只能分页看两百轮。
 *
 * 边界与取舍：
 *   - 整型走 u64 精确比较（入参是 double，>2^53 会失真，与 search_range 同）；
 *     float/double 走浮点 + isfinite。
 *   - exclude_nan 只对 float/double 有意义，整型忽略。NaN 参与任何比较都是 false，
 *     所以区间过滤本身已经排除它；这个开关是给「不限区间、只要非 NaN」用的。
 *   - 读不到的条目**直接剔除**：无法判定是否满足条件，留着会污染后续 set/freeze。
 *   - 压缩必须同步 exact_vals，否则 undo 恢复后 refine 会拿错快照。
 */
int64_t search_filter_exact(gg_driver_t *d, pid_t pid, gg_results_t *r,
                            int has_min, double vmin, int has_max, double vmax,
                            int exclude_nan, int nonzero) {
    if (!r->addrs || r->count == 0) return 0;
    /* 模糊结果集物化后 exact_vt 仍为 0，类型落在 fuzzy_vt 上 */
    int vt = r->exact_vt > 0 ? r->exact_vt : r->fuzzy_vt;
    if (vt <= 0 || vt > VT_DOUBLE) return -1;
    int sz = vt_size(vt);
    if (sz <= 0) return -1;
    int is_float = (vt == VT_FLOAT || vt == VT_DOUBLE);
    /* 整型无符号：负下界钳到 0，否则 (uint64_t)(-1) 会变成巨大的上界 */
    uint64_t ilo = (has_min && vmin > 0) ? (uint64_t)vmin : 0;
    uint64_t ihi = (has_max && vmax > 0) ? (uint64_t)vmax : 0;

    if (!r->exact_vals) {
        r->exact_vals = malloc(r->count * sizeof(uint64_t));
        if (!r->exact_vals) return -1;
        for (size_t i = 0; i < r->count; i++) r->exact_vals[i] = r->exact_value;
    }
    exact_undo_push(r);

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
        size_t have_n = got > 0 ? (size_t)got / (size_t)sz : 0;
        for (size_t j = 0; j < seg_n; j++) {
            const uint8_t *pb = bigbuf + j * (size_t)sz;
            uint8_t one[8];
            if (j >= have_n) {
                /* 段读覆盖不到（短读/空洞）：单地址重试一次，仍失败就剔除 */
                if (driver_read(d, pid, r->addrs[i + j], one, (size_t)sz) != (ssize_t)sz)
                    continue;
                pb = one;
            }
            if (is_float) {
                double v = read_as_double(pb, vt);
                if (exclude_nan && !isfinite(v)) continue;
                if (has_min && !(v >= vmin)) continue;
                if (has_max && !(v <= vmax)) continue;
                if (nonzero && v == 0.0) continue;
            } else {
                uint64_t v = read_bits(pb, vt);
                if (has_min && v < ilo) continue;
                if (has_max && v > ihi) continue;
                if (nonzero && v == 0) continue;
            }
            dst[n] = r->addrs[i + j];
            dstv[n] = read_bits(pb, vt);
            n++;
        }
        i += seg_n;
    }
    free(bigbuf);
    r->count = n;
    /* filter 之后 exact_vals 已是各候选的真实当前值 —— 基线就此建立，
     * 后续 refine（变化方向比较）可直接用，无需再跑建基线轮。 */
    r->exact_vals_valid = 1;
    log_msg("filter: kept %zu candidates", n);
    return (int64_t)n;
}

/* 值分布摘要（top-N）。按命中数降序，聚合键是值的**位模式**（float 不会因 double 精度被合并）。
 *
 * 为什么不返回完整直方图：两万条候选的完整分布可能有上万个 bin，返回给 AI 又大又没用；
 * top-N 刚好回答"是不是绝大多数都是同一个值"这个问题。
 *
 * 溢出语义：桶容量是 4×max_bins，满了就替换当前命中数最小的桶。此时**唯一值总数不再准确**
 * （*unique_exact=0），但 top-N 通常仍正确 —— 只有真实高频值在替换时被踢掉才会失真。
 * 调用方必须把 unique_exact=0 当成"至少这么多"，不能当精确值用。
 */
static int hist_cmp_desc(const void *a, const void *b) {
    const gg_hist_bin_t *x = a, *y = b;
    if (x->hits == y->hits) return 0;
    return x->hits < y->hits ? 1 : -1;
}

size_t results_value_histogram(gg_driver_t *d, pid_t pid, gg_results_t *r,
                               gg_hist_bin_t *out, size_t max_bins, int *unique_exact) {
    if (unique_exact) *unique_exact = 1;
    if (!r->addrs || r->count == 0 || !out || max_bins == 0) return 0;
    int vt = r->exact_vt > 0 ? r->exact_vt : r->fuzzy_vt;
    if (vt <= 0 || vt > VT_DOUBLE) return (size_t)-1;
    int sz = vt_size(vt);
    if (sz <= 0 || sz > 8) return (size_t)-1;

    size_t bucket_cap = max_bins * 4;
    gg_hist_bin_t *bins = calloc(bucket_cap, sizeof(*bins));
    if (!bins) return (size_t)-1;
    size_t used = 0;
    uint8_t *bigbuf = malloc(GG_SEARCH_CHUNK);
    if (!bigbuf) { free(bins); return (size_t)-1; }

    for (size_t i = 0; i < r->count; ) {
        size_t seg_n = 0;
        uint64_t cur = r->addrs[i];
        while (i + seg_n < r->count && r->addrs[i + seg_n] == cur &&
               (seg_n + 1) * (size_t)sz <= GG_SEARCH_CHUNK) {
            seg_n++;
            cur += (uint64_t)sz;
        }
        ssize_t got = driver_read(d, pid, r->addrs[i], bigbuf, seg_n * (size_t)sz);
        size_t have_n = got > 0 ? (size_t)got / (size_t)sz : 0;
        for (size_t j = 0; j < seg_n; j++) {
            const uint8_t *pb = bigbuf + j * (size_t)sz;
            uint8_t one[8];
            if (j >= have_n) {
                /* 读不到的值不计入分布：用 0 顶替会让"0 出现了两万次"这种假结论进 AI 上下文 */
                if (driver_read(d, pid, r->addrs[i + j], one, (size_t)sz) != (ssize_t)sz)
                    continue;
                pb = one;
            }
            uint64_t raw = read_bits(pb, vt);
            size_t k = 0;
            while (k < used && bins[k].raw != raw) k++;
            if (k < used) { bins[k].hits++; continue; }
            if (used < bucket_cap) {
                bins[used].raw = raw;
                bins[used].hits = 1;
                used++;
                continue;
            }
            size_t victim = 0;
            for (size_t m = 1; m < used; m++) if (bins[m].hits < bins[victim].hits) victim = m;
            bins[victim].raw = raw;
            bins[victim].hits = 1;
            if (unique_exact) *unique_exact = 0;
        }
        i += seg_n;
    }
    free(bigbuf);

    qsort(bins, used, sizeof(*bins), hist_cmp_desc);
    size_t nb = used < max_bins ? used : max_bins;
    memcpy(out, bins, nb * sizeof(*out));
    free(bins);
    return nb;
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

/* ---------- 结果集持久化（save/load）----------
 *
 * 动机：结果集活在 agentd 进程内存里，agentd 重启/崩溃/断线重连后全丢 ——
 * 几百候选收敛到一半的长任务直接报废。落盘成 JSON，跨会话恢复。
 *
 * 保存语义：地址 + 「最近一轮的值」（exact_vals 就是这个；fuzzy 先物化）。
 * 恢复语义：加载成 exact 结果集（exact_vt + exact_vals=保存值 + valid=1），
 * refine（含 increased_by 等）从保存值继续比较，链路完整。
 *
 * 序列化/反序列化是纯函数（search.c 内不碰文件），文件 IO 在 cmd_search.c
 * 的薄壳里 —— 这样 round-trip 能在 PC 上单测（tests/unit/test_search.c）。 */

/* 手工拼 JSON 而不是 cJSON：10 万条结果 cJSON 逐节点构造又慢又费内存，
 * 地址数组直接 snprintf 拼接（hex 定长，无转义问题）。 */
char *results_serialize(const gg_results_t *r, int vtype) {
    if (!r || r->count == 0 || !r->addrs) return NULL;
    if (r->fuzzy_active) return NULL;          /* 调用方须先物化 */
    size_t cap = 64 + r->count * 24;           /* "0x%llx",  足够 */
    char *s = malloc(cap);
    if (!s) return NULL;
    int off = snprintf(s, cap, "{\"vtype\":%d,\"count\":%zu,\"addrs\":[", vtype, r->count);
    for (size_t i = 0; i < r->count && off > 0 && (size_t)off < cap - 24; i++) {
        off += snprintf(s + off, cap - (size_t)off, "%s\"0x%llx\"",
                        i ? "," : "", (unsigned long long)r->addrs[i]);
    }
    off += snprintf(s + off, cap - (size_t)off, "],\"vals\":[");
    for (size_t i = 0; i < r->count && off > 0 && (size_t)off < cap - 24; i++) {
        uint64_t v = r->exact_vals ? r->exact_vals[i] : 0;
        off += snprintf(s + off, cap - (size_t)off, "%s\"0x%llx\"",
                        i ? "," : "", (unsigned long long)v);
    }
    snprintf(s + off, cap - (size_t)off, "]}");
    return s;
}

/* 返回成功加载的条数；<0 = 格式错误。成功时 r 被整体重建（原内容清掉）。 */
int64_t results_deserialize(gg_results_t *r, const char *json_text) {
    if (!r || !json_text) return -1;
    cJSON *j = cJSON_Parse(json_text);
    if (!j) return -1;
    cJSON *jvt = cJSON_GetObjectItem(j, "vtype");
    cJSON *ja  = cJSON_GetObjectItem(j, "addrs");
    cJSON *jv  = cJSON_GetObjectItem(j, "vals");
    if (!cJSON_IsNumber(jvt) || !cJSON_IsArray(ja) || !cJSON_IsArray(jv)) {
        cJSON_Delete(j); return -1;
    }
    int vt = (int)jvt->valuedouble;
    if (vt < VT_BYTE || vt > VT_DOUBLE) { cJSON_Delete(j); return -1; }
    size_t n = cJSON_GetArraySize(ja);
    size_t nv = cJSON_GetArraySize(jv);
    if (n == 0 || nv != n) { cJSON_Delete(j); return -1; }

    results_clear(r);
    results_init(r, n < GG_RESULT_CAP_DEFAULT ? n : GG_RESULT_CAP_DEFAULT);
    if (r->cap < n) r->cap = n;    /* init 只给默认 cap，这里补到 n（push 会用满） */
    r->addrs = malloc(n * sizeof(uint64_t));
    r->exact_vals = malloc(n * sizeof(uint64_t));
    if (!r->addrs || !r->exact_vals) { results_clear(r); cJSON_Delete(j); return -1; }
    size_t k = 0;
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, ja) {
        if (!cJSON_IsString(a)) continue;
        char *end = NULL;
        uint64_t addr = strtoull(a->valuestring, &end, 16);
        if (!end || *end) continue;
        r->addrs[k++] = addr;
    }
    if (k != n) { results_clear(r); cJSON_Delete(j); return -1; }
    k = 0;
    cJSON *v = NULL;
    cJSON_ArrayForEach(v, jv) {
        if (!cJSON_IsString(v)) { results_clear(r); cJSON_Delete(j); return -1; }
        char *end = NULL;
        uint64_t val = strtoull(v->valuestring, &end, 16);
        if (!end || *end) { results_clear(r); cJSON_Delete(j); return -1; }
        r->exact_vals[k++] = val;
    }
    r->count = n;
    r->exact_vt = vt;
    r->exact_value = 0;
    r->exact_vals_valid = 1;
    cJSON_Delete(j);
    return (int64_t)n;
}

/* save/load 的文件名校验：[A-Za-z0-9._-]{1,64}，拒路径穿越与控制字符。
 * 返回 1 = 合法。名字直接拼进文件路径，任何 "../" 或 '/' 都不许出现。 */
int gg_safe_result_name(const char *name) {
    if (!name) return 0;
    size_t n = strlen(name);
    if (n == 0 || n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
        /* 连续 '.' 的第一段是 ".."：开头检查（防 "../x"），中段也防（多余但廉价） */
        if (c == '.' && i + 1 < n && name[i + 1] == '.') return 0;
    }
    return 1;
}

/* 批量读连续地址段：addrs[0..n) 中从 idx 起收集连续段（步长 sz，段长 ≤ GG_SEARCH_CHUNK），
 * 一次 driver_read 拉整段，逐项写入 vals[]。返回段内元素数；段读失败返回 0（调用方回退逐项）。
 *
 * ⚠ max_n 是调用方缓冲区还能容纳的元素数，必须参与循环条件。
 *   历史缺陷（2026-10-21 修复）：原来的条件只有「结果集剩余」和「段长 ≤ GG_SEARCH_CHUNK」，
 *   完全不看调用方的 limit —— 而搜索结果天然大量连续地址，于是 bulk 段会按 256KB 上限
 *   一口气写 seg_n（vt=DWORD 时最多 65536 个 u64 = 512KB）进只有 limit*8 字节的缓冲区，
 *   单次 gg_get_results(limit=100) 即可造成堆溢出。agentd 是 root 进程，影响面极大。 */
static size_t bulk_read_segment(gg_driver_t *d, pid_t pid, const uint64_t *addrs,
                                size_t n, size_t idx, int vtype,
                                uint8_t *bigbuf, uint64_t *vals, size_t max_n) {
    int sz = vt_size(vtype);
    size_t seg_n = 0;
    uint64_t cur = addrs[idx];
    while (idx + seg_n < n && seg_n < max_n && addrs[idx + seg_n] == cur &&
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
        /* 本次最多还能写多少个元素：必须传给 bulk_read_segment，
         * 否则连续段的批量写会越过 out_addrs/out_values 的 limit 容量。 */
        size_t seg = bulk_read_segment(d, pid, r->addrs, r->count, i, vtype,
                                       bigbuf, out_values + n, limit - n);
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

/* 在已排序的目标数组里找"与 v 相差不超过 lookback"的目标。
 * 命中时返回 1 并把最接近的目标写到 *out_t（用于回报 target 字段）。
 * 二分找第一个 >= v 的位置，只需检查它和它的前一个。 */
static int find_near_target(const uint64_t *sorted, size_t uniq, uint64_t v,
                            uint64_t lookback, uint64_t *out_t) {
    size_t lo = 0, hi = uniq;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (sorted[mid] < v) lo = mid + 1; else hi = mid;
    }
    /* sorted[lo] >= v（若存在），差值不会下溢 */
    if (lo < uniq && sorted[lo] - v <= lookback) { *out_t = sorted[lo]; return 1; }
    /* sorted[lo-1] < v（若存在），差值同样不下溢 */
    if (lo > 0 && v - sorted[lo - 1] <= lookback) { *out_t = sorted[lo - 1]; return 1; }
    return 0;
}

size_t pointer_scan(gg_driver_t *d, pid_t pid, const uint64_t *targets, size_t ntargets,
                    int vtype, uint64_t lookback, int aligned,
                    gg_pointer_hit_t *out, size_t cap) {
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
    /* 同 search_refine：不用 static（隐式依赖全局锁），见那里的说明。 */
    uint8_t *chunk = malloc(GG_SEARCH_CHUNK);
    if (!chunk) { free(sorted); free(regs); return 0; }

    /* pagemap 跳过 absent 页。可写区里绝大多数是 malloc 保留但从未触碰的页，
     * 逐块 driver_read 会一路读失败、再靠 gg_fault_advance 一页页推进 ——
     * 2026-10-23 靶场实测：微信（585MB）扫 2852 个目标耗 11.7s，其中 90% 是
     * 在读不存在的页。搜索路径（X2）早就接了这套跳过，指针扫描当时漏了。
     * pmfd<0（pagemap 打不开）时 pm_try_skip 恒返回 0，退回原逐页探测。 */
    int pmfd = pm_open(pid);

    for (size_t k = 0; k < nreg && hits < cap; k++) {
        uint64_t addr = regs[k].start;
        uint64_t len = regs[k].end - regs[k].start;
        int fail_streak = 0;
        while (len > 0 && hits < cap) {
            size_t rd = len > GG_SEARCH_CHUNK ? GG_SEARCH_CHUNK : (size_t)len;
            size_t skip = pm_try_skip(pmfd, addr, rd, (size_t)len);
            if (skip) { addr += skip; len -= skip; continue; }
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
            for (size_t i = 0; i <= max_i; i += (size_t)psz) {
                /* 对齐过滤：ARM64 的指针/整数天然按 psz 对齐，非对齐窗口命中
                 * 基本都是二进制数据里的巧合（真机实测过：900/39 相邻的两条
                 * 命中里，非对齐那条周边全是资源数据）。psz 是 2 的幂，用位与。 */
                if (aligned && ((addr + i) & (uint64_t)(psz - 1))) continue;
                uint64_t v = 0;
                memcpy(&v, chunk + i, (size_t)psz);
                if (v == 0) continue;
                uint64_t t = 0;
                int hit;
                if (lookback == 0) {
                    hit = bsearch(&v, sorted, uniq, sizeof(uint64_t), cmp_u64) != NULL;
                    t = v;
                } else {
                    hit = find_near_target(sorted, uniq, v, lookback, &t);
                }
                if (hit) {
                    out[hits].container = addr + i;
                    out[hits].target = t;
                    out[hits].value = v;
                    out[hits].ptype = vtype;
                    hits++;
                    if (hits >= cap) break;
                }
            }
            len -= rd;
            addr += rd;
        }
    }
    if (pmfd >= 0) close(pmfd);
    free(chunk);
    free(sorted);
    free(regs);
    return hits;
}

/* ---------- 多级指针链 ---------- */

/* 每级候选上限。pointer_scan 的 cap 太大会让"每级都爆满"，后续级的
 * 目标集合失真（几万个地址一起当 target，lookback 会把整片堆吸进来）；
 * 太小又会截断真链。2000 条 × 6 级 ≈ 400KB，可接受。 */
#define GG_CHAIN_LEVEL_CAP 2000

typedef struct {
    gg_pointer_hit_t *hits;      /* 已按 container 升序（bsearch 反查用） */
    uint64_t         *containers;/* 去重后的 container 列表（下一级的目标集） */
    size_t            n;         /* hits 条数（bsearch 的长度，不能用去重后的数） */
    size_t            n_cont;
} chain_level_t;

static int cmp_hit_container(const void *a, const void *b) {
    uint64_t x = ((const gg_pointer_hit_t *)a)->container;
    uint64_t y = ((const gg_pointer_hit_t *)b)->container;
    return (x > y) - (x < y);
}

static int in_anchors(uint64_t a, const gg_map_region_t *an, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a >= an[i].start && a < an[i].end) return 1;
    return 0;
}

size_t pointer_chain_scan(gg_driver_t *d, pid_t pid, uint64_t target,
                          int vtype, int max_depth, uint64_t lookback,
                          const gg_map_region_t *anchors, size_t nanchors,
                          gg_pointer_chain_t *out, size_t cap) {
    if (vtype != VT_DWORD && vtype != VT_QWORD) return 0;
    if (target == 0 || cap == 0) return 0;
    if (max_depth < 1) max_depth = 1;
    if (max_depth > GG_CHAIN_MAX_DEPTH) max_depth = GG_CHAIN_MAX_DEPTH;

    chain_level_t lv[GG_CHAIN_MAX_DEPTH];
    memset(lv, 0, sizeof(lv));
    size_t nchain = 0;
    uint64_t solo = target;      /* 第 1 级只有单目标 */

    for (int L = 0; L < max_depth && nchain < cap; L++) {
        gg_pointer_hit_t *hits = malloc(GG_CHAIN_LEVEL_CAP * sizeof(*hits));
        if (!hits) break;
        const uint64_t *tg = (L == 0) ? &solo : lv[L - 1].containers;
        size_t nt = (L == 0) ? 1 : lv[L - 1].n_cont;
        size_t n = pointer_scan(d, pid, tg, nt, vtype, lookback, 1,
                                hits, GG_CHAIN_LEVEL_CAP);
        if (n == 0) { free(hits); break; }
        lv[L].hits = hits;
        lv[L].n = n;
        qsort(hits, n, sizeof(*hits), cmp_hit_container);

        /* 本级命中的 container 即下一级要回溯的目标；同时按升序去重
         * （frame 里同一 container 只会有一条命中，去重是防御性的） */
        lv[L].containers = malloc(n * sizeof(uint64_t));
        if (!lv[L].containers) break;
        size_t cn = 0;
        for (size_t i = 0; i < n; i++) {
            if (cn == 0 || lv[L].containers[cn - 1] != hits[i].container)
                lv[L].containers[cn++] = hits[i].container;
        }
        lv[L].n_cont = cn;

        /* 落在锚点段内的命中 = 一条完整链的浅端。沿 target 反查回原始目标 */
        for (size_t i = 0; i < n && nchain < cap; i++) {
            if (nanchors > 0 && !in_anchors(hits[i].container, anchors, nanchors))
                continue;
            gg_pointer_chain_t ch;
            memset(&ch, 0, sizeof(ch));
            ch.depth = (size_t)L + 1;
            ch.nodes[0].addr  = hits[i].container;
            ch.nodes[0].value = hits[i].value;

            /* 从浅往深走：本级 hit 的 target 指向上一级的 container */
            uint64_t want = hits[i].target;
            int ok = 1;
            for (int k = L - 1; k >= 0; k--) {
                gg_pointer_hit_t key;
                memset(&key, 0, sizeof(key));
                key.container = want;
                gg_pointer_hit_t *f = bsearch(&key, lv[k].hits, lv[k].n,
                                              sizeof(*f), cmp_hit_container);
                if (!f) { ok = 0; break; }       /* 断链：该级被 cap 截断了 */
                size_t pos = (size_t)(L - k);
                ch.nodes[pos].addr  = f->container;
                ch.nodes[pos].value = f->value;
                ch.nodes[pos - 1].offset =
                    (int64_t)(ch.nodes[pos].addr - ch.nodes[pos - 1].value);
                want = f->target;
            }
            if (!ok) continue;
            /* 最深层：它读到的值（或区间命中的 target）离目标多远 */
            ch.nodes[ch.depth - 1].offset =
                (int64_t)(target - ch.nodes[ch.depth - 1].value);
            out[nchain++] = ch;
        }
    }

    for (int i = 0; i < GG_CHAIN_MAX_DEPTH; i++) {
        free(lv[i].hits);
        free(lv[i].containers);
    }
    return nchain;
}
