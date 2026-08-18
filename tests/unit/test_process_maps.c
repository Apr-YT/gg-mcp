/* test_process_maps.c — /proc/pid/maps 行解析单测（T2）
 * 覆盖 parse_maps_line：正常文件映射 / 匿名映射(path 置空) / [anon][heap][stack] /
 * 带空格路径 / 畸形行 / perms 解析。平台无关（纯字符串解析），MinGW 可编译。
 */
#include "../../agentd/process.h"
#include "../../shared/wire_protocol.h"   /* REGION_* */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

/* MinGW(msvcrt) 无 readlink 符号/声明：process.c 全编时链接需要，测试环境提供 shim。
 * 测试只测 parse_maps_line，不触碰任何真实 /proc 读取路径。 */
ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    (void)path; (void)buf; (void)bufsiz;
    errno = ENOSYS;
    return -1;
}

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("ok  : %s\n", msg); } \
    else { printf("[FAIL]: %s\n", msg); failures++; } \
} while (0)

static void check_region(const char *tag, gg_map_region_t *r,
                         uint64_t start, uint64_t end,
                         const char *perms, const char *path) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s: start=0x%llx", tag, (unsigned long long)r->start);
    CHECK(r->start == start, buf);
    snprintf(buf, sizeof(buf), "%s: end=0x%llx", tag, (unsigned long long)r->end);
    CHECK(r->end == end, buf);
    snprintf(buf, sizeof(buf), "%s: perms=%s", tag, r->perms);
    CHECK(strcmp(r->perms, perms) == 0, buf);
    snprintf(buf, sizeof(buf), "%s: path=[%s]", tag, r->path);
    CHECK(strcmp(r->path, path) == 0, buf);
}

int main(void) {
    gg_map_region_t r;

    /* 1) 正常文件映射 */
    memset(&r, 0, sizeof(r));
    CHECK(parse_maps_line(
        "713c81b000-713c93b000 r-xp 00000000 08:07 123 /system/lib64/libc.so\n", &r) == 1,
        "1) 文件映射解析返回 1");
    check_region("1)", &r, 0x713c81b000ull, 0x713c93b000ull, "r-xp",
                 "/system/lib64/libc.so");

    /* 2) 匿名映射（无路径字段）→ path 置空 */
    memset(&r, 0, sizeof(r));
    CHECK(parse_maps_line(
        "7fd5e00000-7fd5f00000 rw-p 00000000 00:00 0 \n", &r) == 1,
        "2) 匿名映射解析返回 1");
    check_region("2)", &r, 0x7fd5e00000ull, 0x7fd5f00000ull, "rw-p", "");

    /* 3) [anon] 显式标记 → path 置空（region_matches 依赖空 path 判 anonymous） */
    memset(&r, 0, sizeof(r));
    CHECK(parse_maps_line(
        "5fa7512000-5fa7532000 rw-p 00000000 00:00 0 [anon:libc_malloc]\n", &r) == 1,
        "3) [anon] 解析返回 1");
    check_region("3)", &r, 0x5fa7512000ull, 0x5fa7532000ull, "rw-p", "");

    /* 4) [heap]/[stack] 特殊段 → path 置空 */
    memset(&r, 0, sizeof(r));
    CHECK(parse_maps_line(
        "5fd5e00000-5fd5f00000 rw-p 00000000 00:00 0 [heap]\n", &r) == 1,
        "4) [heap] 解析返回 1");
    check_region("4)", &r, 0x5fd5e00000ull, 0x5fd5f00000ull, "rw-p", "");
    memset(&r, 0, sizeof(r));
    CHECK(parse_maps_line(
        "7fd5d00000-7fd5e00000 rw-p 00000000 00:00 0 [stack]\n", &r) == 1,
        "4b) [stack] 解析返回 1");
    check_region("4b)", &r, 0x7fd5d00000ull, 0x7fd5e00000ull, "rw-p", "");

    /* 5) 带空格路径（sscanf %s 只取首段，strstr " /" 补全） */
    memset(&r, 0, sizeof(r));
    CHECK(parse_maps_line(
        "716d100000-716d200000 r--p 00000000 08:07 456 /data/app/~~xx/base.apk\n", &r) == 1,
        "5) 路径含 ~ 解析返回 1");
    check_region("5)", &r, 0x716d100000ull, 0x716d200000ull, "r--p",
                 "/data/app/~~xx/base.apk");

    /* 6) 畸形行 → 返回 0 */
    memset(&r, 0, sizeof(r));
    CHECK(parse_maps_line("garbage line without hex ranges\n", &r) == 0,
          "6) 畸形行返回 0");
    memset(&r, 0, sizeof(r));
    CHECK(parse_maps_line("713c81b000-713c93b000 r-xp\n", &r) == 0,
          "6b) 缺 dev/inode 字段返回 0");
    memset(&r, 0, sizeof(r));
    CHECK(parse_maps_line("", &r) == 0, "6c) 空行返回 0");

    /* 7) 大地址（64 位范围） */
    memset(&r, 0, sizeof(r));
    CHECK(parse_maps_line(
        "fffffffff0000000-fffffffff1000000 r-xp 00000000 00:00 0 [vsyscall]\n", &r) == 1,
        "7) 64 位大地址解析返回 1");
    check_region("7)", &r, 0xfffffffff0000000ull, 0xfffffffff1000000ull, "r-xp", "");

    /* 8) perms 五字符（x 附加字符如 'p'/'s'）不截断 */
    memset(&r, 0, sizeof(r));
    CHECK(parse_maps_line(
        "7120000000-7130000000 rwxs 00000000 00:00 0 [anon]\n", &r) == 1,
        "8) perms=rwxs 解析返回 1");
    check_region("8)", &r, 0x7120000000ull, 0x7130000000ull, "rwxs", "");

    /* 9) region_matches: REGION_ALL 排除 ---p(PROT_NONE), 保留可读 */
    {
        gg_map_region_t pn;
        memset(&pn, 0, sizeof(pn));
        CHECK(parse_maps_line(
            "7e28000000-7eaa000000 ---p 00000000 00:00 0 \n", &pn) == 1,
            "9) ---p 段解析返回 1");
        CHECK(region_matches(&pn, REGION_ALL) == 0,
              "9) REGION_ALL 排除 ---p (PROT_NONE 不可读)");

        gg_map_region_t rx;
        memset(&rx, 0, sizeof(rx));
        parse_maps_line("713c81b000-713c93b000 r-xp 00000000 00:00 0 /x/lib.so\n", &rx);
        CHECK(region_matches(&rx, REGION_ALL) == 1, "9) REGION_ALL 保留 r-xp 可读");

        gg_map_region_t rw;
        memset(&rw, 0, sizeof(rw));
        parse_maps_line("7fd5e00000-7fd5f00000 rw-p 00000000 00:00 0 \n", &rw);
        CHECK(region_matches(&rw, REGION_ALL) == 1, "9) REGION_ALL 保留 rw-p 可读");

        gg_map_region_t xs;
        memset(&xs, 0, sizeof(xs));
        parse_maps_line("7120000000-7130000000 --xp 00000000 00:00 0 [anon]\n", &xs);
        CHECK(region_matches(&xs, REGION_ALL) == 0, "9) REGION_ALL 排除 --xp 无读");

        /* REGION_ANONYMOUS / WRITABLE 语义不受影响 */
        CHECK(region_matches(&rw, REGION_WRITABLE) == 1, "9) rw-p REGION_WRITABLE=1");
        CHECK(region_matches(&rx, REGION_WRITABLE) == 0, "9) r-xp REGION_WRITABLE=0");
        CHECK(region_matches(&rw, REGION_ANONYMOUS) == 1, "9) 匿名 rw-p REGION_ANONYMOUS=1");
        CHECK(region_matches(&rx, REGION_ANONYMOUS) == 0, "9) 文件 r-xp REGION_ANONYMOUS=0");
    }

    if (failures == 0) {
        printf("\nALL PROCESS MAPS TESTS PASSED\n");
        return 0;
    }
    printf("\n%d FAILURES\n", failures);
    return 1;
}
