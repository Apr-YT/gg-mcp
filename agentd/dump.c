/* dump.c — region dump/restore 纯函数实现（handler 见 cmd_mem.c） */
#include "dump.h"

#include <string.h>

void gg_dump_header_pack(uint8_t *out, uint64_t orig_addr, uint64_t payload_len,
                         uint32_t checksum) {
    memset(out, 0, GG_DUMP_HEADER_LEN);
    uint32_t magic = GG_DUMP_MAGIC, ver = GG_DUMP_VERSION;
    memcpy(out + 0, &magic, 4);
    memcpy(out + 4, &ver, 4);
    memcpy(out + 8, &orig_addr, 8);
    memcpy(out + 16, &payload_len, 8);
    memcpy(out + 24, &checksum, 4);
    /* 偏移 28 reserved 已由 memset 清零 */
}

int gg_dump_header_unpack(const uint8_t *buf, size_t bufsz,
                          uint64_t *orig_addr, uint64_t *payload_len,
                          uint32_t *checksum) {
    if (bufsz < GG_DUMP_HEADER_LEN) return -1;
    uint32_t magic, ver;
    memcpy(&magic, buf + 0, 4);
    memcpy(&ver, buf + 4, 4);
    if (magic != GG_DUMP_MAGIC || ver != GG_DUMP_VERSION) return -1;
    memcpy(orig_addr, buf + 8, 8);
    memcpy(payload_len, buf + 16, 8);
    memcpy(checksum, buf + 24, 4);
    if (*payload_len == 0 || *payload_len > GG_DUMP_MAX_BYTES) return -1;
    return 0;
}

uint32_t gg_fnv1a32(const uint8_t *data, size_t len, uint32_t seed) {
    /* FNV-1a: hash = (hash ^ byte) * prime，prime=16777619（32 位标准值） */
    uint32_t h = seed;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}
