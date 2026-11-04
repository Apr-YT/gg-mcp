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

#endif /* GG_DUMP_H */
