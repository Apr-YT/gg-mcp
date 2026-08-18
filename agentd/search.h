/* search.h — 内存搜索引擎：精确/区间/未知初值模糊搜索 + 结果集管理 */
#ifndef GG_AGENTD_SEARCH_H
#define GG_AGENTD_SEARCH_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "driver.h"
#include "../shared/wire_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GG_RESULT_CAP_DEFAULT    100000u   /* 结果上限（可配） */
#define GG_FUZZY_MAX_BYTES       (256ull << 20)  /* 模糊快照内存上限 256MB */
#define GG_SEARCH_CHUNK          (256u << 10)    /* 搜索分块 256KB */

/* 模糊扫描区域表项：addr = start + (全局元素i - elem_begin)*步长 */
typedef struct fz_region {
    uint64_t start;
    uint64_t len;
    uint64_t elem_begin;   /* 全局元素起始索引 */
    uint64_t snap_off;     /* 快照内字节偏移 */
} fz_region_t;

typedef struct gg_results {
    /* 物化结果（精确/区间搜索直接写入；模糊搜索在小到阈值后物化） */
    uint64_t *addrs;
    size_t    count;
    size_t    cap;

    /* 精确搜索 refine 状态（GG 标准流程: 搜已知值 → 游戏内值变化 → refine）
     * exact_vals[i] 为 addrs[i] 最近一轮的值快照（位模式） */
    int       exact_vt;
    uint64_t  exact_value;          /* 精确搜索目标值（位模式） */
    uint64_t *exact_vals;           /* 并行数组: 每候选最近值快照 */

    /* 模糊搜索状态（元素粒度 = 值类型大小） */
    int       fuzzy_active;
    int       fuzzy_vt;            /* 值类型（决定元素步长） */
    uint64_t  fuzzy_elem_count;    /* 元素总数 */
    uint8_t  *fuzzy_bitmap;        /* 候选位图: 1 bit/元素 */
    uint8_t  *fuzzy_snapshot;      /* 上一轮快照（原始字节） */
    size_t    fuzzy_snapshot_len;
    fz_region_t *fuzzy_regions;
    size_t    fuzzy_region_count;
} gg_results_t;

void results_init(gg_results_t *r, size_t cap);
void results_clear(gg_results_t *r);

/* 精确搜索：value 为按类型解释的数值（float/double 用位模式）。
 * 返回找到数量；-1 出错。region_type ∈ gg_region。 */
int64_t search_exact(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     int vtype, uint64_t value, int region_type);

/* 区间搜索：min <= value <= max（float/double 按 double 解释）。 */
int64_t search_range(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     int vtype, double vmin, double vmax, int region_type);

/* AoB 特征码搜索：pat[plen] 字节序列滑动匹配，命中起始地址。 */
int parse_hex_pattern(const char *s, uint8_t *out, size_t max);
int64_t search_aob(gg_driver_t *d, pid_t pid, gg_results_t *r,
                   const uint8_t *pat, size_t plen, int region_type);

/* 模糊初始化：全置候选 + 快照。返回扫描元素数；-1 出错。 */
int64_t search_unknown_init(gg_driver_t *d, pid_t pid, gg_results_t *r,
                            int vtype, int region_type);

/* 模糊细化：按 op 收缩候选。返回剩余候选数；-1 出错。 */
int64_t search_refine(gg_driver_t *d, pid_t pid, gg_results_t *r, int op);

/* 精确搜索 refine：对 search_exact 的候选按 op 比较（当前值 vs 各自快照）。
 * 语义同 GG：increased=值变大 / decreased=变小 / unchanged=不变 / changed=变化。
 * 返回剩余数量；-1 无可 refine 的精确结果。 */
int64_t search_refine_exact(gg_driver_t *d, pid_t pid, gg_results_t *r, int op);

/* 物化模糊候选 → addrs（<= cap 时）。返回物化数量。 */
size_t results_materialize(gg_results_t *r);

/* 分页取结果（addr + 当前值位模式）。out_count 为实际返回数。 */
int results_get_page(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     size_t offset, size_t limit,
                     uint64_t *out_addrs, uint64_t *out_values, int vtype, size_t *out_count);

/* 批量修改：addrs[i] ← value（vtype 解释）。返回成功数。 */
size_t results_set_values(gg_driver_t *d, pid_t pid, gg_results_t *r,
                          const uint64_t *addrs, size_t n, int vtype, uint64_t value);

/* 指针扫描：对每个目标地址 A，在可写区找值为 A 的指针。 */
typedef struct gg_pointer_hit {
    uint64_t container;   /* 指针所在地址 */
    uint64_t target;      /* 指针值（= 目标 A） */
    int      ptype;       /* VT_DWORD / VT_QWORD */
} gg_pointer_hit_t;
size_t pointer_scan(gg_driver_t *d, pid_t pid, const uint64_t *targets, size_t ntargets,
                    int vtype, gg_pointer_hit_t *out, size_t cap);

/* 工具 */
int vt_size(int vtype);

/* 把内存字节按类型读出为 u64（float/double 为位模式） */
uint64_t read_bits(const uint8_t *buf, int vtype);

/* 把 u64 数值按类型编码为内存字节（LE）；float/double 按位模式 */
void encode_value_local(int vtype, uint64_t value, uint8_t out[8]);

/* 模糊候选剩余数（未物化时） */
uint64_t results_fuzzy_count(const gg_results_t *r);

#ifdef __cplusplus
}
#endif

#endif /* GG_AGENTD_SEARCH_H */
