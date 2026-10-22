/* test_last_resp.c — 响应存档单测（GG-U6）
 *
 * 覆盖：短负载、边界（正好 CAP-1）、超长截断、UTF-8 字符边界回退、
 * 空串/NULL、reset。重点是**截断点不能切在多字节字符中间** —— 半个字符
 * 会让下游 JSON 解析失败，而这在真机上很难碰到、也很难发现。
 */
#include "../../agentd/last_resp.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("ok  : %s\n", msg); } \
    else { printf("[FAIL]: %s\n", msg); failures++; } \
} while (0)

/* 严格校验 UTF-8：不接受孤立 continuation byte、不接受被截断的首字节序列 */
static int utf8_valid(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return 0;                      /* 非法首字节（含孤立 continuation） */
        if (i + need >= n) return 0;        /* 序列被截断（末字节下标 i+need 越界） */
        for (size_t k = 1; k <= need; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
        i += need + 1;
    }
    return 1;
}

int main(void) {
    gg_last_resp_t r;
    char big[GG_LAST_RESP_CAP * 2];

    /* 1) 短负载：完整保存、不截断 */
    gg_last_resp_reset(&r);
    gg_last_resp_snapshot(&r, "{\"ok\":1}", 1);
    CHECK(r.set == 1 && r.ok == 1 && r.truncated == 0,
          "短负载：set=1 ok=1 truncated=0");
    CHECK(r.full_len == 8, "短负载：full_len = 原始长度");
    CHECK(strcmp(r.buf, "{\"ok\":1}") == 0, "短负载：内容一致");

    /* 2) 错误响应 ok 标记 */
    gg_last_resp_snapshot(&r, "{\"e\":{\"code\":3}}", 0);
    CHECK(r.ok == 0, "错误响应：ok=0");

    /* 3) 边界：正好 CAP-1 不截断 */
    memset(big, 'a', sizeof(big));
    big[GG_LAST_RESP_CAP - 1] = 0;
    gg_last_resp_reset(&r);
    gg_last_resp_snapshot(&r, big, 1);
    CHECK(r.truncated == 0 && r.full_len == GG_LAST_RESP_CAP - 1,
          "边界（CAP-1）：不截断");

    /* 4) 超一位即截断，且只保留 PREVIEW 字节 */
    big[GG_LAST_RESP_CAP - 1] = 'a';
    big[GG_LAST_RESP_CAP] = 0;
    gg_last_resp_reset(&r);
    gg_last_resp_snapshot(&r, big, 1);
    CHECK(r.truncated == 1, "超长：truncated=1");
    CHECK(r.full_len == GG_LAST_RESP_CAP, "超长：full_len 记原始长度");
    CHECK(strlen(r.buf) == GG_LAST_RESP_PREVIEW, "超长：预览长度 = PREVIEW");

    /* 5) UTF-8 边界：让 s[PREVIEW] 落在中文字符中间，必须回退 */
    /* "中" = E4 B8 AD。从下标 254 开始：s[254]=E4 s[255]=B8 s[256]=AD，
     * s[256] 是 continuation byte → 应回退到 254。
     * 注意总长必须超 CAP-1 才会走到截断分支（9000 > 8191）。 */
    const char *zh = "\xE4\xB8\xAD";   /* 中 */
    memset(big, 'a', sizeof(big));
    memcpy(big + 254, zh, 3);
    big[9000] = 0;
    gg_last_resp_reset(&r);
    gg_last_resp_snapshot(&r, big, 1);
    CHECK(r.truncated == 1, "UTF-8 边界：截断");
    CHECK(strlen(r.buf) == 254, "UTF-8 边界：回退到字符首字节（254）");
    CHECK(utf8_valid(r.buf, strlen(r.buf)), "UTF-8 边界：前缀是合法 UTF-8");

    /* 6) 首字节正好落在 256 上（不需要回退） */
    memset(big, 'a', sizeof(big));
    memcpy(big + 256, zh, 3);
    big[9000] = 0;
    gg_last_resp_reset(&r);
    gg_last_resp_snapshot(&r, big, 1);
    CHECK(strlen(r.buf) == 256, "首字节对齐：不回退（256）");
    CHECK(utf8_valid(r.buf, strlen(r.buf)), "首字节对齐：合法 UTF-8");

    /* 7) 全中文超长：任意截断点都要合法 */
    {
        int ok_all = 1;
        for (size_t off = 0; off < 3; off++) {
            memset(big, 'a', off);
            for (size_t i = off; i + 3 < sizeof(big); i += 3) memcpy(big + i, zh, 3);
            big[sizeof(big) - 1] = 0;
            gg_last_resp_reset(&r);
            gg_last_resp_snapshot(&r, big, 1);
            if (!utf8_valid(r.buf, strlen(r.buf))) ok_all = 0;
        }
        CHECK(ok_all, "全中文（三种相位）：截断结果均为合法 UTF-8");
    }

    /* 8) 空串 / NULL */
    gg_last_resp_reset(&r);
    gg_last_resp_snapshot(&r, "", 1);
    CHECK(r.set == 1 && r.full_len == 0 && r.buf[0] == 0, "空串：长度 0、内容空");
    gg_last_resp_snapshot(&r, NULL, 1);
    CHECK(r.set == 1 && r.full_len == 0 && r.buf[0] == 0, "NULL：按空串处理");

    /* 9) reset 清状态 */
    gg_last_resp_reset(&r);
    CHECK(r.set == 0 && r.truncated == 0 && r.full_len == 0 && r.buf[0] == 0,
          "reset：清空全部状态");

    if (failures) { printf("\n%d 项失败\n", failures); return 1; }
    printf("\n全部通过\n");
    return 0;
}
