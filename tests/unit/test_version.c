/* test_version.c — 版本族匹配单测（核心需求: 6.12.xx 同族可加载） */
#include "../../agentd/version.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else { printf("ok  : %s\n", msg); } \
} while (0)

int main(void) {
    /* 同族: 补丁号任意均可加载 */
    CHECK(version_match_family("6.12.1", "6.12.*") == 1, "6.12.1 in 6.12.*");
    CHECK(version_match_family("6.12.9", "6.12.*") == 1, "6.12.9 in 6.12.*");
    CHECK(version_match_family("6.12.123", "6.12.*") == 1, "6.12.123 in 6.12.*");
    CHECK(version_match_family("6.12", "6.12.*") == 1, "6.12 (no patch) in 6.12.*");
    CHECK(version_match_family("v6.12.9-beta", "6.12.*") == 1, "v6.12.9-beta in 6.12.*");

    /* 族外: 拒绝 */
    CHECK(version_match_family("6.13.0", "6.12.*") == 0, "6.13.0 NOT in 6.12.*");
    CHECK(version_match_family("7.0.1", "6.12.*") == 0, "7.0.1 NOT in 6.12.*");
    CHECK(version_match_family("5.99.0", "6.12.*") == 0, "5.99.0 NOT in 6.12.*");

    /* 精确模式 "X.Y.Z" */
    CHECK(version_match_family("6.12.9", "6.12.9") == 1, "exact 6.12.9 == 6.12.9");
    CHECK(version_match_family("6.12.8", "6.12.9") == 0, "6.12.8 != 6.12.9 (exact)");

    /* 解析错误 */
    CHECK(version_match_family("garbage", "6.12.*") == -1, "garbage -> parse error");
    CHECK(version_match_family("", "6.12.*") == -1, "empty -> parse error");

    /* 通配形态（修复: "101.*" 等 major 通配模式） */
    CHECK(version_match_family("101.1", "101.*") == 1, "101.1 in 101.* (major only)");
    CHECK(version_match_family("101.9", "101.*") == 1, "101.9 in 101.* (major only)");
    CHECK(version_match_family("102.0", "101.*") == 0, "102.0 NOT in 101.*");
    CHECK(version_match_family("101.1", "101.1.*") == 1, "101.1 in 101.1.* (major.minor)");
    CHECK(version_match_family("101.2", "101.1.*") == 0, "101.2 NOT in 101.1.*");
    CHECK(version_match_family("101.1.3", "101.1.*") == 1, "101.1.3 in 101.1.*");
    CHECK(version_match_family("101", "101.*") == -1, "single-component version -> parse error");
    CHECK(version_match_family("6.12", "6.12.*") == 1, "6.12 pattern still works");

    /* version_parse */
    gg_ver_t v;
    CHECK(version_parse("6.12.9", &v) == 0 && v.valid && v.major == 6 &&
          v.minor == 12 && v.patch == 9 && v.has_patch, "parse 6.12.9");
    CHECK(version_parse("6.12", &v) == 0 && v.valid && v.major == 6 &&
          v.minor == 12 && !v.has_patch, "parse 6.12");
    CHECK(version_parse("7.0.1", &v) == 0 && v.major == 7 && v.minor == 0 &&
          v.patch == 1, "parse 7.0.1");

    /* KO 文件名版本提取（KO 方案: 文件名版本段参与族匹配） */
    {
        char buf[64];
        CHECK(version_from_ko_name("driverQX114_6.12.9.ko", buf, sizeof(buf)) == 0 &&
              strcmp(buf, "6.12.9") == 0, "ko name -> 6.12.9 (skips QX114)");
        CHECK(version_from_ko_name("driverGT_6.13.0.ko", buf, sizeof(buf)) == 0 &&
              strcmp(buf, "6.13.0") == 0, "ko name -> 6.13.0");
        CHECK(version_from_ko_name("plain_driver.ko", buf, sizeof(buf)) == -1,
              "ko without version -> -1");
        CHECK(version_from_ko_name("6.12.9.ko", buf, sizeof(buf)) == 0 &&
              strcmp(buf, "6.12.9") == 0, "ko name starting with version");
        CHECK(version_from_ko_name("driverQX114_6.12.9.ko", buf, sizeof(buf)) == 0 &&
              version_match_family(buf, "6.12.*") == 1,
              "ko 6.12.9 in 6.12.* family");
        CHECK(version_from_ko_name("driverGT_6.13.0.ko", buf, sizeof(buf)) == 0 &&
              version_match_family(buf, "6.12.*") == 0,
              "ko 6.13.0 NOT in 6.12.* family");
    }

    printf(fails ? "\n== FAILED: %d ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
