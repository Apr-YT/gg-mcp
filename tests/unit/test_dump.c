/* test_dump.c — region dump/restore 纯函数层护栏（dump.h / dump.c）
 *
 * 覆盖：头部 pack/unpack 往返、坏 magic/version 拒绝、payload_len 上下界、
 * 短缓冲拒绝、FNV-1a 流式累积与已知向量、翻位检测。
 * handler 层（cmd_mem.c 的文件 IO + 分块读写）走 itest，真机验证。
 */
#include <stdio.h>
#include <string.h>

#include "dump.h"

/* dump.c 的核心循环依赖 driver（itest 在设备端真链路验证）；
 * 本测试只碰纯函数（header/FNV），这两个符号不会被调用，
 * stub 仅为让链接通过 —— 若哪个断言误触它们，返回 -1 会立刻失败。 */
#include "driver.h"
ssize_t driver_read_ex(gg_driver_t *d, pid_t pid, uint64_t addr, void *buf,
                       size_t len, int force) {
    (void)d; (void)pid; (void)addr; (void)buf; (void)len; (void)force;
    return -1;
}
ssize_t driver_write(gg_driver_t *d, pid_t pid, uint64_t addr, const void *buf, size_t len) {
    (void)d; (void)pid; (void)addr; (void)buf; (void)len;
    return -1;
}

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

int main(void) {
    /* ---- header pack/unpack 往返 ---- */
    {
        uint8_t hdr[GG_DUMP_HEADER_LEN];
        gg_dump_header_pack(hdr, 0x7f00abcd1234ull, 0x10000ull, 0xdeadbeefu);
        uint64_t a = 0, l = 0;
        uint32_t sum = 0;
        CHECK(gg_dump_header_unpack(hdr, sizeof(hdr), &a, &l, &sum) == 0, "unpack valid header");
        CHECK(a == 0x7f00abcd1234ull, "orig_addr roundtrip");
        CHECK(l == 0x10000ull, "payload_len roundtrip");
        CHECK(sum == 0xdeadbeefu, "checksum roundtrip");
        /* reserved 28..31 必须清零：restore 端将来启用该字段时老文件不能带垃圾 */
        CHECK(hdr[28] == 0 && hdr[29] == 0 && hdr[30] == 0 && hdr[31] == 0,
              "reserved field zeroed");
        /* magic 字节序："GGRD" 小端 → 44 52 47 47 */
        CHECK(hdr[0] == 'D' && hdr[1] == 'R' && hdr[2] == 'G' && hdr[3] == 'G',
              "magic bytes little-endian GGRD");
    }

    /* ---- 坏头部必须拒绝 ---- */
    {
        uint8_t hdr[GG_DUMP_HEADER_LEN];
        gg_dump_header_pack(hdr, 0x1000, 0x100, 0);
        uint64_t a, l; uint32_t s;
        hdr[0] ^= 0xff;   /* 坏 magic */
        CHECK(gg_dump_header_unpack(hdr, sizeof(hdr), &a, &l, &s) == -1, "bad magic rejected");
        gg_dump_header_pack(hdr, 0x1000, 0x100, 0);
        hdr[4] = 9;       /* 坏版本 */
        CHECK(gg_dump_header_unpack(hdr, sizeof(hdr), &a, &l, &s) == -1, "bad version rejected");
        gg_dump_header_pack(hdr, 0x1000, 0, 0);      /* len=0 */
        CHECK(gg_dump_header_unpack(hdr, sizeof(hdr), &a, &l, &s) == -1, "len=0 rejected");
        uint64_t huge = GG_DUMP_MAX_BYTES;
        gg_dump_header_pack(hdr, 0x1000, huge + 1, 0); /* len 超上限 */
        CHECK(gg_dump_header_unpack(hdr, sizeof(hdr), &a, &l, &s) == -1, "len over cap rejected");
        gg_dump_header_pack(hdr, 0x1000, huge, 0);     /* 恰好等于上限：合法边界 */
        CHECK(gg_dump_header_unpack(hdr, sizeof(hdr), &a, &l, &s) == 0, "len==cap accepted");
        /* 缓冲区短于头部 */
        gg_dump_header_pack(hdr, 0x1000, 0x100, 0);
        CHECK(gg_dump_header_unpack(hdr, GG_DUMP_HEADER_LEN - 1, &a, &l, &s) == -1,
              "short buffer rejected");
    }

    /* ---- FNV-1a：流式累积 == 一次算完；已知向量；翻位检测 ---- */
    {
        uint8_t data[300];
        for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 37 + 5);
        uint32_t whole = gg_fnv1a32(data, sizeof(data), GG_FNV_OFFSET_BASIS);
        uint32_t split = gg_fnv1a32(data, 100, GG_FNV_OFFSET_BASIS);
        split = gg_fnv1a32(data + 100, 150, split);
        split = gg_fnv1a32(data + 250, 50, split);
        CHECK(whole == split, "streamed fnv equals one-shot");
        /* 空块不改状态（seed 透传语义；dump len>0 下的防御性确认） */
        CHECK(gg_fnv1a32(data, 0, 0x1234) == 0x1234, "empty block is identity");
        /* 标准 FNV-1a 向量：与 PC 侧/外部现成实现可互校验 dump 文件的前提 */
        CHECK(gg_fnv1a32(NULL, 0, GG_FNV_OFFSET_BASIS) == 0x811c9dc5u, "fnv empty = basis");
        uint8_t one = 'a';
        CHECK(gg_fnv1a32(&one, 1, GG_FNV_OFFSET_BASIS) == 0xe40c292cu, "fnv 'a' vector");
        /* 翻一个位，校验和必须变（restore 拒坏文件的根据） */
        data[42] ^= 0x01;
        CHECK(gg_fnv1a32(data, sizeof(data), GG_FNV_OFFSET_BASIS) != whole,
              "single bit flip changes sum");
    }

    if (fails) { printf("test_dump: %d failure(s)\n", fails); return 1; }
    printf("test_dump: all passed\n");
    return 0;
}
