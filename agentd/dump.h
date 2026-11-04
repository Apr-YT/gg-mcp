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

#endif /* GG_DUMP_H */
