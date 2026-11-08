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

/* ---- 差异回调收集（gg_diff_feed 在热循环里回调，这里只做拷贝） ---- */
#define MAXREC 64
typedef struct {
    uint64_t addr;
    uint64_t span;
    size_t   pre;
    uint8_t  a[GG_DIFF_PREFIX];
    uint8_t  b[GG_DIFF_PREFIX];
} rec_t;
static rec_t g_recs[MAXREC];
static int g_nrecs;

static void rec_cb(void *ud, uint64_t addr, uint64_t span,
                   const uint8_t *a, const uint8_t *b, size_t pre) {
    (void)ud;
    if (g_nrecs >= MAXREC) return;
    rec_t *r = &g_recs[g_nrecs++];
    r->addr = addr;
    r->span = span;
    r->pre = pre;
    memcpy(r->a, a, pre);
    memcpy(r->b, b, pre);
}

/* 跑一趟完整扫描（begin+feed+finish），返回区间数 */
static size_t diff_run(const uint8_t *a, const uint8_t *b, size_t len,
                       uint64_t base, size_t gap, size_t max_regions,
                       gg_diff_state_t *st_out) {
    gg_diff_state_t st;
    gg_diff_begin(&st, base, gap, max_regions);
    g_nrecs = 0;
    gg_diff_feed(&st, a, b, len, rec_cb, NULL);
    gg_diff_finish(&st, rec_cb, NULL);
    if (st_out) *st_out = st;
    return st.regions;
}

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

    /* ---- 区域差异状态机：区间划分 / gap 合并 / 跨块 / 截断 ---- */
    {
        uint8_t a[256], b[256];
        gg_diff_state_t st;
        memset(a, 0x11, sizeof(a));
        memset(b, 0x11, sizeof(b));

        /* 全同 */
        CHECK(diff_run(a, b, sizeof(a), 0x1000, 0, 0, &st) == 0,
              "identical buffers -> 0 regions");
        CHECK(st.diff_bytes == 0, "identical -> diff_bytes 0");

        /* 单字节差异：地址 = base + 偏移，span=1，前缀带新旧值 */
        b[100] = 0x22;
        CHECK(diff_run(a, b, sizeof(a), 0x1000, 0, 0, &st) == 1,
              "single byte diff -> 1 region");
        CHECK(g_recs[0].addr == 0x1000 + 100 && g_recs[0].span == 1,
              "region addr/span at base+100");
        CHECK(st.diff_bytes == 1, "single byte -> diff_bytes 1");
        /* span=1 时前缀截到 1 字节（pre 取 min(pre_len, span)） */
        CHECK(g_recs[0].pre == 1 && g_recs[0].a[0] == 0x11 && g_recs[0].b[0] == 0x22,
              "prefix carries old/new byte");

        /* 两段差异相隔 3 个相同字节：gap=0 分开，gap=4 合并 */
        memset(b, 0x11, sizeof(b));
        b[10] = 0xAA;
        b[14] = 0xBB;
        CHECK(diff_run(a, b, sizeof(a), 0x2000, 0, 0, &st) == 2,
              "gap=0 -> two separate regions");
        CHECK(st.diff_bytes == 2, "gap=0 -> diff_bytes 2");
        CHECK(diff_run(a, b, sizeof(a), 0x2000, 4, 0, &st) == 1,
              "gap=4 merges gaps of 3 bytes");
        CHECK(g_recs[0].span == 5, "merged span covers from first to last diff");
        CHECK(st.diff_bytes == 2,
              "merged region still reports exact diff_bytes (2)");
        /* gap 恰好等于间隔时合并，小于时分开（边界） */
        CHECK(diff_run(a, b, sizeof(a), 0x2000, 3, 0, NULL) == 1,
              "gap==3 merges exactly");
        CHECK(diff_run(a, b, sizeof(a), 0x2000, 2, 0, NULL) == 2,
              "gap==2 splits");

        /* 跨块：32 字节连续差异分三次 feed，必须仍是 1 个区间 */
        memset(b, 0x11, sizeof(b));
        for (int i = 8; i < 40; i++) b[i] = (uint8_t)(0x50 + i);
        gg_diff_begin(&st, 0x3000, 0, 0);
        g_nrecs = 0;
        gg_diff_feed(&st, a, b, 16, rec_cb, NULL);
        gg_diff_feed(&st, a + 16, b + 16, 16, rec_cb, NULL);
        gg_diff_feed(&st, a + 32, b + 32, 224, rec_cb, NULL);
        gg_diff_finish(&st, rec_cb, NULL);
        CHECK(g_nrecs == 1, "cross-chunk run stays a single region");
        CHECK(g_recs[0].addr == 0x3000 + 8 && g_recs[0].span == 32,
              "cross-chunk region addr/span correct");
        CHECK(st.diff_bytes == 32, "cross-chunk diff_bytes 32");
        /* 前缀只在段起点块内抓：起点在块 1 的 i=8，块内剩余 8 字节 */
        CHECK(g_recs[0].pre == 8, "cross-chunk prefix limited to start chunk remainder");
        CHECK(g_recs[0].a[0] == 0x11 && g_recs[0].b[0] == (uint8_t)(0x50 + 8),
              "cross-chunk prefix starts at run start");

        /* max_regions 截断 */
        memset(b, 0x11, sizeof(b));
        for (int i = 0; i < 100; i += 2) b[i] = 0x99;   /* 50 个单字节区间 */
        CHECK(diff_run(a, b, sizeof(a), 0x4000, 0, 3, &st) == 3,
              "max_regions stops at 3");
        CHECK(st.truncated == 1, "max_regions sets truncated");
        CHECK(g_nrecs == 3, "only 3 callbacks emitted");

        /* 尾字节差异必须在 finish 时吐出（in_run 未关闭） */
        memset(b, 0x11, sizeof(b));
        b[255] = 0x77;
        gg_diff_begin(&st, 0x6000, 0, 0);
        g_nrecs = 0;
        gg_diff_feed(&st, a, b, 256, rec_cb, NULL);
        CHECK(g_nrecs == 0, "trailing diff not emitted before finish");
        gg_diff_finish(&st, rec_cb, NULL);
        CHECK(g_nrecs == 1 && g_recs[0].addr == 0x6000 + 255,
              "finish flushes the trailing region");

        /* 空块 feed 是 no-op（分块循环的边界，块大小正好整除时会出现） */
        gg_diff_begin(&st, 0x5000, 0, 0);
        g_nrecs = 0;
        gg_diff_feed(&st, a, b, 0, rec_cb, NULL);
        gg_diff_finish(&st, rec_cb, NULL);
        CHECK(st.base == 0x5000 && st.regions == 0, "empty feed is a no-op");

        /* 长区间前缀上限 16 */
        memset(b, 0x11, sizeof(b));
        memset(b + 32, 0x77, 64);   /* 64 字节连续差异 */
        CHECK(diff_run(a, b, sizeof(a), 0x7000, 0, 0, &st) == 1, "long run -> 1 region");
        CHECK(g_recs[0].span == 64, "long run span == 64");
        CHECK(g_recs[0].pre == GG_DIFF_PREFIX, "long run prefix capped at 16");
    }

    if (fails) { printf("test_dump: %d failure(s)\n", fails); return 1; }
    printf("test_dump: all passed\n");
    return 0;
}
