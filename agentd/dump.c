/* dump.c — region dump/restore 纯函数实现 + 核心循环（handler 薄壳见 cmd_mem.c） */
#include "dump.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 分块大小 = 驱动单次读写上限 RWPROC_MAX_IO（driver.c 定义，256KB）。
 * 两块各自定义是因为这个上限属于驱动契约，改任何一边都要同步 ——
 * 若有一天不一致，症状是每块最后一次调用短读/短写，dump 立即报残缺失败。 */
#define GG_DUMP_CHUNK (256u << 10)

static void set_err(char *err, size_t errsz, const char *fmt, ...) {
    if (!err || !errsz) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

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

/* ------------------------------------------------------------------ */

int gg_region_dump_to_file(gg_driver_t *d, pid_t pid, uint64_t addr, uint64_t len,
                           const char *path, char *err, size_t errsz) {
    if (len < 1 || len > GG_DUMP_MAX_BYTES) {
        set_err(err, errsz, "len must be 1..%u bytes", GG_DUMP_MAX_BYTES);
        return -1;
    }
    /* "w+b"：头部先写占位（checksum 未知），流完数据后 fseek 回填。
     * 不先在内存拼完整镜像 —— 64MB 上限下那要多吃一份内存。 */
    FILE *f = fopen(path, "w+b");
    if (!f) { set_err(err, errsz, "cannot write file"); return -1; }
    uint8_t *chunk = malloc(GG_DUMP_CHUNK);
    if (!chunk) {
        fclose(f); remove(path);
        set_err(err, errsz, "out of memory");
        return -1;
    }

    int io_failed = 0;
    uint8_t hdr[GG_DUMP_HEADER_LEN];
    gg_dump_header_pack(hdr, addr, len, 0);
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) io_failed = 1;

    uint32_t sum = GG_FNV_OFFSET_BASIS;
    uint64_t done = 0;
    while (!io_failed && done < len) {
        size_t want = (len - done) > GG_DUMP_CHUNK ? GG_DUMP_CHUNK : (size_t)(len - done);
        ssize_t got = driver_read_ex(d, pid, addr + done, chunk, want, 0);
        if (got != (ssize_t)want) {
            set_err(err, errsz,
                    "short read at 0x%llx (%lld of %zu bytes): page not resident? "
                    "dump aborted, incomplete backup removed; "
                    "try a smaller range or an active mapping",
                    (unsigned long long)(addr + done), (long long)got, want);
            free(chunk); fclose(f); remove(path);
            return -1;
        }
        if (fwrite(chunk, 1, want, f) != want) { io_failed = 1; break; }
        sum = gg_fnv1a32(chunk, want, sum);
        done += want;
    }

    if (!io_failed) {
        gg_dump_header_pack(hdr, addr, len, sum);
        if (fseek(f, 0, SEEK_SET) != 0 || fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
            io_failed = 1;
    }
    if (fclose(f) != 0) io_failed = 1;
    free(chunk);
    if (io_failed) {
        remove(path);
        set_err(err, errsz, "file write failed (disk full?)");
        return -1;
    }
    return 0;
}

int gg_region_restore_from_file(gg_driver_t *d, pid_t pid, const char *path,
                                uint64_t addr_override,
                                uint64_t *out_addr, uint64_t *out_len,
                                char *err, size_t errsz) {
    FILE *f = fopen(path, "rb");
    if (!f) { set_err(err, errsz, "no such dump"); return -1; }

    /* 第一遍：读头部 + 全量 FNV 校验。文件在设备上可能躺了几天，
     * 写回前不校验就是把潜在的坏数据直接灌进目标进程。 */
    uint8_t hdr[GG_DUMP_HEADER_LEN];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f); set_err(err, errsz, "corrupt dump (short header)"); return -1;
    }
    uint64_t orig_addr, len;
    uint32_t want_sum;
    if (gg_dump_header_unpack(hdr, sizeof(hdr), &orig_addr, &len, &want_sum) != 0) {
        fclose(f); set_err(err, errsz, "corrupt dump (bad header)"); return -1;
    }
    uint8_t *chunk = malloc(GG_DUMP_CHUNK);
    if (!chunk) { fclose(f); set_err(err, errsz, "out of memory"); return -1; }
    uint32_t sum = GG_FNV_OFFSET_BASIS;
    uint64_t checked = 0;
    while (checked < len) {
        size_t want = (len - checked) > GG_DUMP_CHUNK ? GG_DUMP_CHUNK : (size_t)(len - checked);
        if (fread(chunk, 1, want, f) != want) {
            free(chunk); fclose(f);
            set_err(err, errsz, "corrupt dump (truncated data)");
            return -1;
        }
        sum = gg_fnv1a32(chunk, want, sum);
        checked += want;
    }
    if (sum != want_sum) {
        free(chunk); fclose(f);
        set_err(err, errsz, "checksum mismatch: dump file corrupted, refusing to write back");
        return -1;
    }

    uint64_t addr = addr_override ? addr_override : orig_addr;

    /* 第二遍：rewind 越过头部，分块写回。短写必须报错并带上进度 ——
     * 此时内存已写了一半，调用方需要知道断点才能决定重试还是放弃。 */
    if (fseek(f, GG_DUMP_HEADER_LEN, SEEK_SET) != 0) {
        free(chunk); fclose(f);
        set_err(err, errsz, "seek failed");
        return -1;
    }
    uint64_t done = 0;
    while (done < len) {
        size_t want = (len - done) > GG_DUMP_CHUNK ? GG_DUMP_CHUNK : (size_t)(len - done);
        if (fread(chunk, 1, want, f) != want) {
            free(chunk); fclose(f);
            set_err(err, errsz, "file changed during restore?");
            return -1;
        }
        ssize_t w = driver_write(d, pid, addr + done, chunk, want);
        if (w != (ssize_t)want) {
            set_err(err, errsz,
                    "short write at 0x%llx after %llu bytes: memory state is now PARTIAL",
                    (unsigned long long)(addr + done), (unsigned long long)done);
            free(chunk); fclose(f);
            return -1;
        }
        done += want;
    }
    free(chunk);
    fclose(f);
    if (out_addr) *out_addr = addr;
    if (out_len)  *out_len = len;
    return 0;
}

int gg_dump_list(const char *dir, gg_dump_entry_t *out, size_t cap) {
    DIR *dp = opendir(dir);
    if (!dp) return -1;
    size_t n = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL && n < cap) {
        const char *fn = de->d_name;
        size_t fl = strlen(fn);
        if (fl <= 4 || fl - 4 >= sizeof(out->name)) continue;
        if (strcmp(fn + fl - 4, ".bin") != 0) continue;
        char path[256];
        int pl = snprintf(path, sizeof(path), "%s/%s", dir, fn);
        if (pl < 0 || (size_t)pl >= sizeof(path)) continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint8_t hdr[GG_DUMP_HEADER_LEN];
        size_t got = fread(hdr, 1, sizeof(hdr), f);
        fclose(f);
        gg_dump_entry_t *e = &out[n];
        /* 头部非法的文件跳过（见 dump.h 注释：坏文件留给 restore 的 FNV 挡） */
        if (gg_dump_header_unpack(hdr, got, &e->orig_addr, &e->payload_len,
                                  &e->checksum) != 0)
            continue;
        memcpy(e->name, fn, fl - 4);
        e->name[fl - 4] = '\0';
        n++;
    }
    closedir(dp);
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* 区域差异比较 */

void gg_diff_begin(gg_diff_state_t *st, uint64_t base_addr, size_t gap_merge,
                   size_t max_regions) {
    memset(st, 0, sizeof(*st));
    st->base = base_addr;
    st->gap_merge = gap_merge;
    st->max_regions = max_regions;
}

/* 关闭当前区间并回调。span = 最后一个差异字节 - 起点 + 1，
 * 因此 span 里可能含 ≤ gap_merge 个未变字节（合并的代价，diff_bytes 才是
 * 精确差异数）。 */
static void emit_run(gg_diff_state_t *st, gg_diff_cb cb, void *ud) {
    if (!st->in_run) return;
    uint64_t span = st->run_last + 1 - st->run_start;
    size_t pre = st->pre_len < span ? st->pre_len : (size_t)span;
    if (cb) cb(ud, st->run_start, span, st->a_pre, st->b_pre, pre);
    st->regions++;
    st->in_run = 0;
    st->gap_seen = 0;
    if (st->max_regions && st->regions >= st->max_regions) st->truncated = 1;
}

size_t gg_diff_feed(gg_diff_state_t *st, const uint8_t *a, const uint8_t *b,
                    size_t len, gg_diff_cb cb, void *ud) {
    for (size_t i = 0; i < len; i++) {
        if (a[i] == b[i]) {
            if (!st->in_run) continue;
            /* 连续相同字节超阈值才断段 —— 这就是 gap_merge 的全部语义 */
            if (++st->gap_seen > st->gap_merge) emit_run(st, cb, ud);
            if (st->truncated) break;
            continue;
        }
        if (!st->in_run) {
            st->in_run = 1;
            st->run_start = st->base + i;
            st->gap_seen = 0;
            /* 前缀只在段起点抓一次。跨块的长段前缀就是起点块内剩余部分
             * （见头文件说明）—— 段跨块后指针已失效，不能延后补。 */
            size_t avail = len - i;
            st->pre_len = avail < GG_DIFF_PREFIX ? avail : GG_DIFF_PREFIX;
            memcpy(st->a_pre, a + i, st->pre_len);
            memcpy(st->b_pre, b + i, st->pre_len);
        }
        st->run_last = st->base + i;
        st->diff_bytes++;
    }
    st->base += len;
    return st->regions;
}

size_t gg_diff_finish(gg_diff_state_t *st, gg_diff_cb cb, void *ud) {
    emit_run(st, cb, ud);
    return st->regions;
}

/* 读一个 dump 的头部并校验。成功 0。 */
static int read_dump_header(FILE *f, const char *path, uint64_t *addr, uint64_t *len,
                            uint32_t *sum, char *err, size_t errsz) {
    uint8_t hdr[GG_DUMP_HEADER_LEN];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        set_err(err, errsz, "%s: corrupt dump (short header)", path);
        return -1;
    }
    if (gg_dump_header_unpack(hdr, sizeof(hdr), addr, len, sum) != 0) {
        set_err(err, errsz, "%s: corrupt dump (bad header)", path);
        return -1;
    }
    return 0;
}

/* 两个 dump 文件的共同收尾：校验 FNV + 关闭最后一段。
 * 分开写是因为 files/live 两条路径的 FNV 来源不同（前者两文件，后者只有文件侧）。
 * 返回 0 成功；FNV 不符返回 -1。truncated 时跳过校验（已提前结束扫描，
 * 校验结果没有意义）。 */
static int finish_checks(gg_diff_state_t *st, uint32_t sum_a, uint32_t want_a,
                         uint32_t sum_b, uint32_t want_b, int two_files,
                         gg_diff_cb cb, void *ud, char *err, size_t errsz) {
    size_t regions = gg_diff_finish(st, cb, ud);
    if (st->truncated) return 0;   /* 提前结束，FNV 未算完 */
    if (sum_a != want_a) {
        set_err(err, errsz, "dump A checksum mismatch: file corrupted");
        return -1;
    }
    if (two_files && sum_b != want_b) {
        set_err(err, errsz, "dump B checksum mismatch: file corrupted");
        return -1;
    }
    (void)regions;
    return 0;
}

int gg_region_diff_files(const char *path_a, const char *path_b,
                         size_t gap_merge, size_t max_regions,
                         gg_diff_cb cb, void *ud,
                         uint64_t *out_regions, uint64_t *out_diff_bytes,
                         int *out_truncated, char *err, size_t errsz) {
    FILE *fa = fopen(path_a, "rb");
    if (!fa) { set_err(err, errsz, "%s: cannot open", path_a); return -1; }
    FILE *fb = fopen(path_b, "rb");
    if (!fb) { fclose(fa); set_err(err, errsz, "%s: cannot open", path_b); return -1; }

    uint64_t addr_a = 0, len_a = 0, addr_b = 0, len_b = 0;
    uint32_t want_a = 0, want_b = 0;
    if (read_dump_header(fa, path_a, &addr_a, &len_a, &want_a, err, errsz) != 0 ||
        read_dump_header(fb, path_b, &addr_b, &len_b, &want_b, err, errsz) != 0) {
        fclose(fa); fclose(fb);
        return -1;
    }
    /* 原址与长度都必须一致：不同长度说明两次快照覆盖的范围都变了，
     * 逐字节对齐比较会得出"整段都变了"的假结论。 */
    if (addr_a != addr_b || len_a != len_b) {
        set_err(err, errsz,
                "not the same region: A=0x%llx+%llu B=0x%llx+%llu "
                "(compare two dumps of the same address range)",
                (unsigned long long)addr_a, (unsigned long long)len_a,
                (unsigned long long)addr_b, (unsigned long long)len_b);
        fclose(fa); fclose(fb);
        return -1;
    }

    uint8_t *ba = malloc(GG_DUMP_CHUNK);
    uint8_t *bb = malloc(GG_DUMP_CHUNK);
    if (!ba || !bb) {
        free(ba); free(bb); fclose(fa); fclose(fb);
        set_err(err, errsz, "out of memory");
        return -1;
    }

    gg_diff_state_t st;
    gg_diff_begin(&st, addr_a, gap_merge, max_regions);
    uint32_t sum_a = GG_FNV_OFFSET_BASIS, sum_b = GG_FNV_OFFSET_BASIS;
    uint64_t done = 0;
    int rc = 0;
    while (done < len_a) {
        size_t want = (len_a - done) > GG_DUMP_CHUNK ? GG_DUMP_CHUNK
                                                     : (size_t)(len_a - done);
        if (fread(ba, 1, want, fa) != want || fread(bb, 1, want, fb) != want) {
            set_err(err, errsz, "truncated dump data at 0x%llx",
                    (unsigned long long)(addr_a + done));
            rc = -1;
            break;
        }
        sum_a = gg_fnv1a32(ba, want, sum_a);
        sum_b = gg_fnv1a32(bb, want, sum_b);
        gg_diff_feed(&st, ba, bb, want, cb, ud);
        if (st.truncated) break;
        done += want;
    }
    if (rc == 0)
        rc = finish_checks(&st, sum_a, want_a, sum_b, want_b, 1, cb, ud, err, errsz);

    free(ba); free(bb); fclose(fa); fclose(fb);
    if (rc != 0) return rc;
    if (out_regions)    *out_regions = st.regions;
    if (out_diff_bytes) *out_diff_bytes = st.diff_bytes;
    if (out_truncated)  *out_truncated = st.truncated;
    return 0;
}

int gg_region_diff_live(gg_driver_t *d, pid_t pid, const char *path_a,
                        size_t gap_merge, size_t max_regions,
                        gg_diff_cb cb, void *ud,
                        uint64_t *out_regions, uint64_t *out_diff_bytes,
                        int *out_truncated, char *err, size_t errsz) {
    FILE *fa = fopen(path_a, "rb");
    if (!fa) { set_err(err, errsz, "%s: cannot open", path_a); return -1; }
    uint64_t addr = 0, len = 0;
    uint32_t want_sum = 0;
    if (read_dump_header(fa, path_a, &addr, &len, &want_sum, err, errsz) != 0) {
        fclose(fa);
        return -1;
    }

    uint8_t *ba = malloc(GG_DUMP_CHUNK);
    uint8_t *bb = malloc(GG_DUMP_CHUNK);
    if (!ba || !bb) {
        free(ba); free(bb); fclose(fa);
        set_err(err, errsz, "out of memory");
        return -1;
    }

    gg_diff_state_t st;
    gg_diff_begin(&st, addr, gap_merge, max_regions);
    uint32_t sum_a = GG_FNV_OFFSET_BASIS;
    uint64_t done = 0;
    int rc = 0;
    while (done < len) {
        size_t want = (len - done) > GG_DUMP_CHUNK ? GG_DUMP_CHUNK
                                                   : (size_t)(len - done);
        if (fread(ba, 1, want, fa) != want) {
            set_err(err, errsz, "truncated dump data at 0x%llx",
                    (unsigned long long)(addr + done));
            rc = -1;
            break;
        }
        ssize_t got = driver_read_ex(d, pid, addr + done, bb, want, 0);
        if (got != (ssize_t)want) {
            /* 读不到不能当作"相同"：那会把真实的差异报成"没变" */
            set_err(err, errsz,
                    "short read at 0x%llx (%lld of %zu bytes): page not resident? "
                    "diff aborted",
                    (unsigned long long)(addr + done), (long long)got, want);
            rc = -1;
            break;
        }
        sum_a = gg_fnv1a32(ba, want, sum_a);
        gg_diff_feed(&st, ba, bb, want, cb, ud);
        if (st.truncated) break;
        done += want;
    }
    if (rc == 0)
        rc = finish_checks(&st, sum_a, want_sum, 0, 0, 0, cb, ud, err, errsz);

    free(ba); free(bb); fclose(fa);
    if (rc != 0) return rc;
    if (out_regions)    *out_regions = st.regions;
    if (out_diff_bytes) *out_diff_bytes = st.diff_bytes;
    if (out_truncated)  *out_truncated = st.truncated;
    return 0;
}
