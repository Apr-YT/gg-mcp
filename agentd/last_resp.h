/* last_resp.h — 响应存档（cmd_end 事件里 result 摘要的来源）
 *
 * 单独成头是为了可测：截断点必须回退到 UTF-8 字符边界，否则下游 JSON
 * 解析会撞上半截多字节字符。这类字节级细节靠真机试不出来。
 *
 * 为什么需要截断标记：响应体没有上限（gg_get_results limit=10000 单帧
 * 610–740KB），而存档缓冲有上限。原实现直接 snprintf 截断后仍按完整 JSON
 * 解析 —— 必然失败，cmd_end 里 result 字段静默消失，订阅端分不清「没有结果」
 * 和「结果被截断」。所以这里把「原始长度」和「是否截断」单独记下来。
 */
#ifndef GG_AGENTD_LAST_RESP_H
#define GG_AGENTD_LAST_RESP_H

#include <stddef.h>
#include <string.h>

#define GG_LAST_RESP_CAP      8192   /* 未截断时最多存这么多 */
#define GG_LAST_RESP_PREVIEW  256    /* 截断时只留这么多作预览 */

typedef struct gg_last_resp {
    char   buf[GG_LAST_RESP_CAP];
    size_t full_len;    /* 原始负载长度（截断前） */
    int    truncated;   /* 1 = buf 只是前缀，不能当完整 JSON 解析 */
    int    ok;          /* 1 = 成功响应，0 = 错误响应 */
    int    set;         /* 0 = 本次请求没产生响应（handler 直接返回） */
} gg_last_resp_t;

static inline void gg_last_resp_reset(gg_last_resp_t *r) {
    r->set = 0;
    r->truncated = 0;
    r->full_len = 0;
    r->buf[0] = 0;
}

/* 存档一次响应负载。s 可为 NULL（等同空串）。 */
static inline void gg_last_resp_snapshot(gg_last_resp_t *r, const char *s, int ok) {
    r->ok = ok;
    r->set = 1;
    size_t n = s ? strlen(s) : 0;
    r->full_len = n;

    size_t cp = n;
    if (cp > GG_LAST_RESP_CAP - 1) {
        cp = GG_LAST_RESP_PREVIEW;
        /* s[cp] 落在某个多字节字符中间（continuation byte, 10xxxxxx）时回退到
         * 该字符的首字节 —— 半个字符会让下游 UTF-8 解码失败。 */
        while (cp > 0 && ((unsigned char)s[cp] & 0xC0) == 0x80) cp--;
        r->truncated = 1;
    } else {
        r->truncated = 0;
    }
    if (s && cp) memcpy(r->buf, s, cp);
    r->buf[cp] = 0;
}

#endif /* GG_AGENTD_LAST_RESP_H */
