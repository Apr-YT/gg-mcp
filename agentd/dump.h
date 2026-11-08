/* dump.h — 内存区域快照（region dump/restore）的纯函数层
 *
 * 文件格式（/data/adb/GG-Ghost/dumps/<name>.bin）：
 *   偏移 0   u32 magic   = GG_DUMP_MAGIC（"GGRD" 小端）
 *   偏移 4   u32 version = 1
 *   偏移 8   u64 orig_addr   dump 时的起始地址（restore 缺省 addr 时写回这里）
 *   偏移 16  u64 payload_len 数据区字节数
 *   偏移 24  u32 checksum    数据区 FNV-1a（流式，seed=0）
 *   偏移 28  u32 reserved = 0
 *
 * 头部定长 32 字节、小端：ARM64 与 x86_64 同为小端，不做字节序转换；
 * 头部自身不参与校验和（restore 先验 magic/len 再流式校验数据）。
 */
#ifndef GG_DUMP_H
#define GG_DUMP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* pid_t */

#include "driver.h"

#define GG_DUMP_MAGIC       0x47475244u   /* "GGRD" */
#define GG_DUMP_VERSION     1u
#define GG_DUMP_HEADER_LEN  32u
/* 单次 dump/restore 数据上限。典型用途是备份一个模块/堆区（几 MB~几十 MB），
 * 64MB 既覆盖场景又防 wire 请求里的 len 异常值把 /data 分区写爆。 */
#define GG_DUMP_MAX_BYTES   (64u * 1024u * 1024u)

/* 头部打包（out 需 ≥ GG_DUMP_HEADER_LEN）。 */
void gg_dump_header_pack(uint8_t *out, uint64_t orig_addr, uint64_t payload_len,
                         uint32_t checksum);

/* 头部解析。返回 0 成功；-1 = magic/version/payload_len 任一非法。
 * payload_len 超过 GG_DUMP_MAX_BYTES 也判非法：restore 侧不依赖调用方先做上限检查。 */
int gg_dump_header_unpack(const uint8_t *buf, size_t bufsz,
                          uint64_t *orig_addr, uint64_t *payload_len,
                          uint32_t *checksum);

/* FNV-1a 流式校验和：首次调用 seed 传 GG_FNV_OFFSET_BASIS，跨块续算传上一块返回值。
 * 与标准 FNV-1a 全兼容 —— dump 文件可被 PC 侧任何现成 FNV 实现独立校验。
 * 选 FNV 而非 CRC32：检测落盘偶发位翻转足够（防"文件坏了还写回内存"，
 * 不是密码学场景），且无需查表、零依赖。 */
#define GG_FNV_OFFSET_BASIS 0x811c9dc5u
uint32_t gg_fnv1a32(const uint8_t *data, size_t len, uint32_t seed);

/* ---------------- 核心循环（依赖 driver，但无 cJSON/引擎单例，itest 可直测） ----------------
 *
 * 放在这层而非 cmd_mem.c handler 的理由：handler 只该做参数解析和发帧，
 * 分块读写 + 文件 IO 的语义（残缺即失败、FNV 先验后写）要在设备端集成测试里
 * 直接驱动 —— 塞在 handler 里就永远测不到，只能等线上翻车。
 *
 * 两函数共用约定：成功返回 0；失败返回 -1 并把人类可读原因写进 err
 * （err 可为 NULL）。文件中途失败一律 remove(path)，不留半成品。
 */

/* 读 [addr, addr+len) 分块落盘 path（含 32B 头部）。len 上限 GG_DUMP_MAX_BYTES。
 * 短读（页被回收/is_force=0 读不了）即失败删文件 —— 不补 0：补零备份恢复时
 * 会把假数据写回目标进程。force 恒为 0（is_force_read 在这台内核 panic）。 */
int gg_region_dump_to_file(gg_driver_t *d, pid_t pid, uint64_t addr, uint64_t len,
                           const char *path, char *err, size_t errsz);

/* 校验 path（头部 + 全量 FNV）后分块写回。
 * addr_override != 0 时写到该地址，否则写回头部记录的原址（注意：原址恰为 0
 * 的 dump 无法区分两种语义 —— 地址 0 本身不可写，无实际影响）。
 * 成功时 out_addr/out_len 回填实际写回位置（均可为 NULL）。 */
int gg_region_restore_from_file(gg_driver_t *d, pid_t pid, const char *path,
                                uint64_t addr_override,
                                uint64_t *out_addr, uint64_t *out_len,
                                char *err, size_t errsz);

/* dumps 目录条目（gg_dump_list 用）：name 去掉 .bin 后缀。 */
typedef struct gg_dump_entry {
    char     name[65];
    uint64_t orig_addr;
    uint64_t payload_len;
    uint32_t checksum;
} gg_dump_entry_t;

/* 枚举 dir 下所有 *.bin 并解析头部，按目录顺序填 out（上限 cap）。
 * 返回条目数（≥0）；目录打不开返回 -1。
 * 头部非法的文件**跳过**（混入的坏文件不该让整个列表失败，restore 时会被
 * FNV 挡住）。 */
int gg_dump_list(const char *dir, gg_dump_entry_t *out, size_t cap);

/* ---------------- 区域差异比较（gg_region_diff） ----------------
 *
 * CE "compare memory" 对应物。三个典型用途：
 *   1. dump vs 当前内存 —— 备份到现在这块区域被改成什么样了（restore 前
 *      看清改动范围，restore 后确认真的写回去了）
 *   2. 两个 dump 对比 —— 同区域两次快照间哪些字节变了
 *   3. 结构体变化定位 —— 只改一个字段，看差异区间落在哪个偏移
 *
 * 按"区间"报告而非逐字节：相邻差异间隔 ≤ gap_merge 个相同字节时合并成
 * 一个区间（多字节数值一次写 4/8 字节，逐字节报告会把信息淹掉）。
 * 区间 span 含中间被合并进来的未变字节，diff_bytes 才是精确差异计数。
 */

#define GG_DIFF_PREFIX 16

typedef struct gg_diff_state {
    uint64_t base;        /* 下一块首字节的绝对地址 */
    uint64_t run_start;   /* 当前区间起点（绝对地址） */
    uint64_t run_last;    /* 当前区间内最后一个差异字节（绝对地址） */
    uint64_t diff_bytes;  /* 精确差异字节数（不含 gap 内未变字节） */
    size_t   gap_merge;   /* 合并阈值：间隔 ≤ 此值的两段差异算一段 */
    size_t   gap_seen;    /* 当前连续相同字节计数 */
    size_t   regions;     /* 已产出区间数 */
    size_t   max_regions; /* 0 = 不限；达到后停止扫描并置 truncated */
    size_t   pre_len;     /* 区间前缀有效字节数 0..GG_DIFF_PREFIX */
    int      in_run;
    int      truncated;
    uint8_t  a_pre[GG_DIFF_PREFIX];
    uint8_t  b_pre[GG_DIFF_PREFIX];
} gg_diff_state_t;

/* 区间回调。addr/span 是该区间的绝对范围；a_pre/b_pre 为区间起点起
 * 连续 pre_len 字节的内容副本（跨块的长区间前缀只覆盖起始块内剩余部分，
 * 可能短于 GG_DIFF_PREFIX —— 前缀是为了"一眼看出改了什么"，
 * 要完整内容用 gg_hex_dump）。
 * 回调内不要调用 driver 或做重活：它在扫描热循环里被调用。 */
typedef void (*gg_diff_cb)(void *ud, uint64_t addr, uint64_t span,
                           const uint8_t *a_pre, const uint8_t *b_pre, size_t pre_len);

/* 初始化。base_addr = 第一块数据对应的绝对地址（两块内存区域绝对地址不同时
 * 传 0 即可，报告里就是偏移而非地址）。 */
void gg_diff_begin(gg_diff_state_t *st, uint64_t base_addr, size_t gap_merge,
                   size_t max_regions);

/* 喂一块数据（a/b 长度均为 len，内容为该块两边的字节）。可多次调用，
 * 状态跨块保持（差异段跨块不会断成两段）。返回当前区间数。 */
size_t gg_diff_feed(gg_diff_state_t *st, const uint8_t *a, const uint8_t *b,
                    size_t len, gg_diff_cb cb, void *ud);

/* 收尾：把最后一个未关闭的区间吐出来。返回区间总数。 */
size_t gg_diff_finish(gg_diff_state_t *st, gg_diff_cb cb, void *ud);

/* 两个 dump 文件对比。要求原址与长度一致 —— 不同区域/不同长度的逐字节
 * 比较没有意义，直接在头部阶段拒绝。两文件的 FNV 在扫描过程中顺便验证
 * （坏文件拒绝对比，而不是吐满屏假差异）。
 * max_regions 达到后停止扫描，*out_truncated=1。
 * err 可传 NULL；out_* 均可传 NULL。成功 0，失败 -1。 */
int gg_region_diff_files(const char *path_a, const char *path_b,
                         size_t gap_merge, size_t max_regions,
                         gg_diff_cb cb, void *ud,
                         uint64_t *out_regions, uint64_t *out_diff_bytes,
                         int *out_truncated, char *err, size_t errsz);

/* dump 与目标进程当前内存对比（范围取自 dump 头部）。
 * 遇短读即失败：读不到的部分不能当作"相同"，那会误导成"没变"。 */
int gg_region_diff_live(gg_driver_t *d, pid_t pid, const char *path_a,
                        size_t gap_merge, size_t max_regions,
                        gg_diff_cb cb, void *ud,
                        uint64_t *out_regions, uint64_t *out_diff_bytes,
                        int *out_truncated, char *err, size_t errsz);

#endif /* GG_DUMP_H */
