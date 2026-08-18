/* version.c — GG 版本提取与版本族匹配
 *
 * 核心需求："只要是同类型的如 6.12.xx，后面 xx 不同都可以加载"。
 * 匹配规则：pattern 为 "X.Y.*"（或 "X.Y"）时，major.minor 相同即同族，
 * 补丁号任意 → 允许加载；pattern 为 "X.Y.Z" 时要求全等。
 */
#include "version.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int version_parse(const char *s, gg_ver_t *out) {
    memset(out, 0, sizeof(*out));
    out->valid = 0;
    if (!s || !*s) return -1;

    /* 跳过前导字母（如 "v"、"ver "） */
    const char *p = s;
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) return -1;

    char tmp[64];
    size_t n = 0;
    while (*p && n < sizeof(tmp) - 1) {
        char c = *p;
        if (isdigit((unsigned char)c) || c == '.') tmp[n++] = c;
        else break;
        p++;
    }
    tmp[n] = 0;

    int a = 0, b = 0, c = 0;
    int nf = sscanf(tmp, "%d.%d.%d", &a, &b, &c);
    if (nf < 2) return -1;
    out->major = a;
    out->minor = b;
    if (nf == 3) {
        out->patch = c;
        out->has_patch = 1;
    }
    out->valid = 1;
    return 0;
}

int version_match_family(const char *version, const char *pattern) {
    gg_ver_t v;
    if (version_parse(version, &v) < 0 || !v.valid) return -1;

    /* 模式按 '.' 切 token；"*" 为通配（该段不比较）。
     * 支持形态:
     *   "101.*"   → 仅比 major（major=101 任意 minor/patch）
     *   "101.1.*" → 比 major+minor（patch 任意）  ← 严格同族
     *   "6.12"    → 同 "6.12.*"（比 major+minor）
     *   "6.12.9"  → 三段全比（精确）
     */
    char pt[64];
    snprintf(pt, sizeof(pt), "%s", pattern ? pattern : "");
    int tok[3] = {0, 0, 0};
    int wild[3] = {0, 0, 0};
    int nt = 0;
    char *save = NULL;
    char *t = strtok_r(pt, ".", &save);
    while (t && nt < 3) {
        if (strcmp(t, "*") == 0) wild[nt] = 1;
        else tok[nt] = atoi(t);
        nt++;
        t = strtok_r(NULL, ".", &save);
    }
    if (nt == 0) return -1;

    int comps[3] = {v.major, v.minor, v.patch};
    if (!wild[0] && tok[0] != comps[0]) return 0;
    if (nt >= 2 && !wild[1] && tok[1] != comps[1]) return 0;
    if (nt >= 3 && !wild[2] && tok[2] != comps[2]) return 0;
    return 1;
}

int version_from_package(const char *pkg, char **out) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "dumpsys package %s 2>/dev/null | grep -m1 'versionName='", pkg);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char line[256];
    *out = NULL;
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strstr(line, "versionName=");
        if (!eq) continue;
        eq += strlen("versionName=");
        char *end = eq;
        while (*end && *end != '\n' && *end != ' ' && *end != '\r') end++;
        size_t len = (size_t)(end - eq);
        if (len == 0) continue;
        char *res = malloc(len + 1);
        memcpy(res, eq, len);
        res[len] = 0;
        *out = res;
        break;
    }
    int rc = pclose(fp);
    if (*out == NULL) return -1;
    (void)rc;
    return 0;
}

int version_syntactic(const char *s) {
    gg_ver_t v;
    return (version_parse(s, &v) == 0 && v.valid) ? 1 : 0;
}

int version_from_ko_name(const char *name, char *out, size_t outsz) {
    const char *p = name;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            size_t n = 0;
            const char *q = p;
            int has_dot = 0;
            while (*q && n + 1 < outsz) {
                char c = *q;
                if (isdigit((unsigned char)c)) {
                    out[n++] = c;
                    q++;
                } else if (c == '.') {
                    out[n++] = c;
                    has_dot = 1;
                    q++;
                } else {
                    break;
                }
            }
            out[n] = 0;
            /* 去掉尾随点（"6.12.9.ko" 会提取出 "6.12.9."） */
            while (n > 0 && out[n - 1] == '.') out[--n] = 0;
            if (has_dot && n > 0) return 0;      /* "6.12.9" 等: 含点 → 视为版本 */
            p = q;                                 /* 名字里的数字段（如 QX114）无点 → 继续找 */
            continue;
        }
        p++;
    }
    out[0] = 0;
    return -1;
}
