/* search.h — 内存搜索引擎：精确/区间/未知初值模糊搜索 + 结果集管理 */
#ifndef GG_AGENTD_SEARCH_H
#define GG_AGENTD_SEARCH_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "driver.h"
#include "process.h"          /* gg_map_region_t（指针链的锚点段） */
#include "../shared/wire_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GG_RESULT_CAP_DEFAULT    100000u   /* 结果上限（可配） */
#define GG_FUZZY_MAX_BYTES       (256ull << 20)  /* 模糊快照内存上限 256MB */
#define GG_GROUP_MAX             8               /* 组合搜索最多 8 个值 */
#define GG_GROUP_SPAN_MAX        4096            /* 组合搜索窗口上限（字节） */
#define GG_SEARCH_CHUNK          (256u << 10)    /* 搜索分块 256KB */
/* 模糊 refine 可回退步数。每级占「元素数/8」字节（256MB 区 = 8.4MB/级），
 * 所以不设太大；3 步足够覆盖"搜错了退回去换 op 再试"。
 * 按需分配：没做过 refine 就不占内存。 */
#define GG_FUZZY_UNDO_DEPTH      3

/* 模糊扫描区域表项：addr = start + (全局元素i - elem_begin)*步长 */
typedef struct fz_region {
    uint64_t start;
    uint64_t len;
    uint64_t elem_begin;   /* 全局元素起始索引 */
    uint64_t snap_off;     /* 快照内字节偏移 */
} fz_region_t;

/* 精确搜索 refine 的撤销副本（地址数组 + 各自的上一轮值） */
typedef struct gg_exact_undo {
    uint64_t *addrs;
    uint64_t *vals;
    size_t    count;
} gg_exact_undo_t;

typedef struct gg_results {
    /* 物化结果（精确/区间搜索直接写入；模糊搜索在小到阈值后物化） */
    uint64_t *addrs;
    size_t    count;
    size_t    cap;

    /* 精确搜索 refine 状态（GG 标准流程: 搜已知值 → 游戏内值变化 → refine）
     * exact_vals[i] 为 addrs[i] 最近一轮的值快照（位模式）
     *
     * exact_vals_valid: exact_vals 是否已建立"所有候选的真实基线"。
     * 1 = 精确搜索（快照 == 搜索值）或 refine 已跑过至少一轮，可直接比较；
     * 0 = 区间/AoB 搜索刚结束——候选值各不相同，快照尚未建立，
     *     search_refine_exact 的第一轮会先读基线（不筛选），之后才有效。 */
    int       exact_vt;
    int       exact_vals_valid;
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

    /* 快照上限截断报告（P1-10）：扫描区总大小超过 GG_FUZZY_MAX_BYTES 时，
     * 超出部分不纳入快照 —— 静默截断会让"模糊搜索搜不到"被误当成
     * "值不存在"。这两个字段把被丢掉的部分亮出来给调用方看。
     * 0 = 没有截断（全部纳入）。skipped_from 是第一个未纳入的字节地址。 */
    uint64_t  fuzzy_skipped_bytes; /* 被丢掉的字节数（含末尾 region 截短部分） */
    uint64_t  fuzzy_skipped_from;  /* 第一个未纳入的地址（0 = 无截断） */

    /* 模糊 refine 的撤销栈：每次 refine 前把位图拷一份。
     *
     * 为什么需要：refine 是**破坏性**的（位图只减不增），搜错一次就只能重新
     * init —— 而 init 要把整个可写区（最多 256MB）读一遍。真机靶场里这个
     * 成本直接劝退"多试一次"。
     *
     * 只回退位图、不回退数据快照（快照是 256MB，每轮拷一份太贵）。
     * 代价是 undo 后紧接着的 refine 是拿"更早一轮的值"做基准，语义上相当于
     * 跳过一次比较 —— 对"我搜错了，退回去换个 op 再试"这个用法没有影响。 */
    uint8_t  *fuzzy_undo[GG_FUZZY_UNDO_DEPTH];
    size_t    fuzzy_undo_bytes;    /* 单级位图字节数（0 = 尚未分配） */
    int       fuzzy_undo_head;     /* 最近一次保存的下标，-1 = 空 */
    int       fuzzy_undo_count;    /* 当前可回退步数 */

    /* 精确搜索 refine 的撤销栈（同一深度）。精确 refine 是原地压缩 addrs，
     * 同样只减不增 —— 不存副本就没法回退。每级最多 cap × 16 字节
     * （默认 cap=100000 → 1.6MB），比模糊的位图副本便宜得多。 */
    gg_exact_undo_t exact_undo[GG_FUZZY_UNDO_DEPTH];
    int       exact_undo_head;
    int       exact_undo_depth;
} gg_results_t;

void results_init(gg_results_t *r, size_t cap);
void results_clear(gg_results_t *r);

/* 按 module 子串过滤 region：保留 path 含 module 的段（文件映射）。
 * 原地压缩（只挪 struct，不 realloc），返回过滤后条数。
 * module 为 NULL/空串时原样返回 n。path 为空的匿名段一律剔除。 */
size_t regions_filter_module(gg_map_region_t *regs, size_t n, const char *module);

/* module 参数语义（所有 search_* 共用）：NULL/空 = 不过滤（全 region_type 范围）；
 * 非空 = 只搜 maps 里 path 含该子串的段（如 "libil2cpp.so"）。
 * 典型用途：特征码/静态数据搜索限定在目标 so 内 —— 整片堆一起扫噪声巨大。 */

/* 精确搜索：value 为按类型解释的数值（float/double 用位模式）。
 * 返回找到数量；-1 出错。region_type ∈ gg_region。 */
int64_t search_exact(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     int vtype, uint64_t value, int region_type,
                     const char *module);
/* 组合搜索 "v1;v2;v3::span"：n 个值顺序无关地出现在 span 字节窗口内，
 * 结果集存锚点 v1 的地址。见 search.c 函数头注释。 */
int64_t search_group(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     const uint64_t *vals, size_t nvals, int vtype,
                     uint64_t span, int region_type, const char *module);

/* 区间搜索：min <= value <= max（float/double 按 double 解释）。 */
/* aligned: 1=只扫自然对齐位置（默认，避免 8 倍冗余命中），0=逐字节。 */
int64_t search_range(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     int vtype, double vmin, double vmax, int region_type, int aligned,
                     const char *module);

/* AoB 特征码搜索：pat[plen] 字节序列滑动匹配，命中起始地址。 */
/* AoB 特征码解析：支持三种写法
 *   8B            → 整字节精确（pat=0x8B, mask=0xFF）
 *   8?  / ?B      → 半字节通配（社区签名最常见，如 frida's "?8 ?? ?? ?? 08 01"）
 *   ??            → 整字节通配
 * mask 是逐位掩码：匹配条件是 (mem & mask[i]) == pat[i]。
 * mask 可为 NULL（调用方只想要字节值、不关心通配），此时通配位写 0。
 * 返回字节数；出错返回 -1。 */
int parse_hex_pattern_masked(const char *s, uint8_t *out, uint8_t *mask, size_t max);
/* 字符串 → AoB 字节序列（utf16=1 转 UTF-16LE 含代理对）。见 search.c 注释。 */
int gg_str_to_pattern(const char *s, int utf16, int append_nul,
                      uint8_t *out, size_t max);

/* 旧的纯字节接口（不含通配），保留给只接受精确序列的调用方 */
int parse_hex_pattern(const char *s, uint8_t *out, size_t max);

/* AoB 搜索。mask 为 NULL 表示整串精确（走 memcmp 快速路径）；
 * 非 NULL 则逐字节按位比较，支持通配。 */
int64_t search_aob(gg_driver_t *d, pid_t pid, gg_results_t *r,
                   const uint8_t *pat, const uint8_t *mask,
                   size_t plen, int region_type, const char *module);

/* 模糊初始化：全置候选 + 快照。返回扫描元素数；-1 出错。 */
int64_t search_unknown_init(gg_driver_t *d, pid_t pid, gg_results_t *r,
                            int vtype, int region_type, const char *module);

/* 模糊细化：按 op 收缩候选。返回剩余候选数；-1 出错。 */
int64_t search_refine(gg_driver_t *d, pid_t pid, gg_results_t *r, int op);
/* by 版本：REFINE_INCREASED_BY/DECREASED_BY 时 by 为增量（必须 > 0） */
int64_t search_refine_by(gg_driver_t *d, pid_t pid, gg_results_t *r, int op, double by);

/* 撤销最近一次模糊 refine（恢复候选位图）。返回恢复后的候选数；
 * -1 = 没有可回退的步骤（或当前不是模糊搜索）。 */
int64_t search_refine_undo(gg_results_t *r);

/* 撤销最近一次精确/区间搜索的 refine（恢复地址数组）。返回恢复后的条目数；
 * -1 = 没有可回退的步骤。 */
int64_t search_refine_undo_exact(gg_results_t *r);

/* 精确搜索 refine：对 search_exact 的候选按 op 比较（当前值 vs 各自快照）。
 * 语义同 GG：increased=值变大 / decreased=变小 / unchanged=不变 / changed=变化。
 * 返回剩余数量；-1 无可 refine 的精确结果。 */
int64_t search_refine_exact(gg_driver_t *d, pid_t pid, gg_results_t *r, int op);
/* by 版本：REFINE_INCREASED_BY/DECREASED_BY 时 by 为增量（必须 > 0） */
int64_t search_refine_exact_by(gg_driver_t *d, pid_t pid, gg_results_t *r, int op, double by);

/* 结果集内过滤：只保留当前值落在 [vmin, vmax] 的条目（原地压缩，可用 undo 回退）。
 * has_min/has_max 为 0 表示该端不限；exclude_nan 只对 float/double 生效；
 * nonzero 排掉值为 0 的条目。读不到的条目会被剔除。
 * 返回剩余数量；-1 类型未知或内存不足。 */
int64_t search_filter_exact(gg_driver_t *d, pid_t pid, gg_results_t *r,
                            int has_min, double vmin, int has_max, double vmax,
                            int exclude_nan, int nonzero);

/* 物化模糊候选 → addrs（<= cap 时）。返回物化数量。 */
size_t results_materialize(gg_results_t *r);
/* 持久化（save/load）：序列化/反序列化是纯函数（PC 可单测），
 * 文件 IO 在 cmd_search.c。fuzzy 须先物化。 */
char *results_serialize(const gg_results_t *r, int vtype);
int64_t results_deserialize(gg_results_t *r, const char *json_text);
int gg_safe_result_name(const char *name);

/* 值分布摘要：把结果集的**当前值**聚合成 top-N（按出现次数降序）。
 * 用途：AI 拿到上万条候选时先看"是不是绝大多数都是同一个值"，一眼定位异常值。
 * out 由调用方提供（元素数 >= max_bins），实现内部用 4×max_bins 的桶，满后替换最小项。
 * *unique_exact 置 1 表示唯一值总数准确（没发生溢出替换），0 表示实际唯一值更多。
 * 返回写入 out 的 bin 数（<= max_bins）；-1 = 类型未知/内存不足。 */
typedef struct {
    uint64_t raw;    /* 值位模式：聚合与比较都用它，float 不会因 double 精度被误合并 */
    size_t   hits;
} gg_hist_bin_t;

size_t results_value_histogram(gg_driver_t *d, pid_t pid, gg_results_t *r,
                               gg_hist_bin_t *out, size_t max_bins, int *unique_exact);

/* 分页取结果（addr + 当前值位模式）。out_count 为实际返回数。 */
int results_get_page(gg_driver_t *d, pid_t pid, gg_results_t *r,
                     size_t offset, size_t limit,
                     uint64_t *out_addrs, uint64_t *out_values, int vtype, size_t *out_count);

/* 批量修改：addrs[i] ← value（vtype 解释）。返回成功数。 */
size_t results_set_values(gg_driver_t *d, pid_t pid, gg_results_t *r,
                          const uint64_t *addrs, size_t n, int vtype, uint64_t value);

/* 指针扫描：对每个目标地址 A，在可写区找值为 A 的指针。
 *
 * lookback: 0 = 精确匹配（v == A）；>0 = 区间匹配，命中条件为
 *   A - lookback <= v <= A + lookback。用于"指针指向对象首址、目标字段在
 *   对象内部"的情况（C++ 对象普遍如此）。参数语义参考 CE 的 structsize /
 *   ptrscan 的 -p lookback，但这里是双向的，同时覆盖"指向成员"和"指向父结构"。
 *
 * aligned: 1 = 只扫 8 字节对齐位置。ARM64 指针天然对齐，非对齐命中基本是
 *   二进制数据里的巧合（真机实测：900/39 相邻的两条命中里，非对齐那条周边
 *   全是资源数据）。默认 1。 */
typedef struct gg_pointer_hit {
    uint64_t container;   /* 指针所在地址 */
    uint64_t target;      /* 指针值命中时对应的目标地址（区间模式下是最接近的 A） */
    uint64_t value;       /* container 处读到的原始指针值（区间模式下 target != value） */
    int      ptype;       /* VT_DWORD / VT_QWORD */
} gg_pointer_hit_t;

/* ---------- 多级指针链 ----------
 *
 * 单级 pointer_scan 只回答"谁指向这个地址"。游戏里的真需求是多级链：
 *
 *     模块基址 + 0x10 → 对象地址 → +0x24 → 目标字段
 *
 * 堆地址每次进程重启都变，只有从模块出来的那条链（基址稳定 + 固定偏移）
 * 才可复用。CE 的 pointer scan / GG 的"指针链"都是这个。
 *
 * 编址约定：nodes[0] 最浅（离模块最近），nodes[depth-1] 最贴近目标。
 *   nodes[0].addr    = 模块内的那个指针位置（锚点）
 *   nodes[0].value   = 下一级对象地址
 *   nodes[0].offset  = nodes[1].addr - nodes[0].value
 *   ……
 *   nodes[depth-1].offset = target_addr - nodes[depth-1].value
 * 按 CE 写法读出来是：[[[nodes[0].value + nodes[0].offset] + nodes[1].offset] ...]
 *
 * 实现是逐级回溯（每级一次全内存扫描），不是 ptrmap 一次性建图：
 * 内存换时间。6 级 = 6 次扫描，真机微信量级约 6×12s，慢但正确。
 * 见 docs/engineering/POINTER_CHAIN_20261104.md 的"已知限制"。
 */
#define GG_CHAIN_MAX_DEPTH 6

typedef struct gg_chain_node {
    uint64_t addr;     /* 指针所在地址（本级指针位置） */
    uint64_t value;    /* 该处读到的指针值 */
    int64_t  offset;   /* 下一级地址 - value（可负：lookback 命中偏移） */
} gg_chain_node_t;

typedef struct gg_pointer_chain {
    size_t          depth;                        /* 链长度（级数） */
    gg_chain_node_t nodes[GG_CHAIN_MAX_DEPTH];
} gg_pointer_chain_t;

/* target: 目标地址（通常是结果集里收敛下来的字段地址）
 * max_depth: 回溯级数上限 1..GG_CHAIN_MAX_DEPTH
 * lookback: 每级容差（同 pointer_scan 语义），0 = 精确
 * anchors/nanchors: 允许作为链终点的地址段（通常是"路径非空"的模块段）。
 *   nanchors==0 时不过滤，输出所有链。
 * 返回写入 out 的链条数（可能被 cap 截断）。 */
size_t pointer_chain_scan(gg_driver_t *d, pid_t pid, uint64_t target,
                          int vtype, int max_depth, uint64_t lookback,
                          const gg_map_region_t *anchors, size_t nanchors,
                          gg_pointer_chain_t *out, size_t cap);
size_t pointer_scan(gg_driver_t *d, pid_t pid, const uint64_t *targets, size_t ntargets,
                    int vtype, uint64_t lookback, int aligned,
                    gg_pointer_hit_t *out, size_t cap);

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
