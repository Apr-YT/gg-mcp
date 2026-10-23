/* wire_io.c — 出站帧发送层（自 main.c 拆出，2026-10-23，Phase 1·①）
 *
 * 为什么单独一个文件：这是"把 cJSON 响应变成线上字节"的唯一出口，所有
 * handler 都经过 send_ok/send_err。它和业务无关，只是协议编码 + 发送锁。
 *
 * 四条不能改的约束（每条都对应过一次事故）：
 *
 *   1. **g_send_mtx 是"出站帧全局串行"，不是注册表锁。** 事件广播线程和各
 *      连接线程会同时往同一个 fd 写（广播写事件、handler 写响应）；没有这把锁
 *      时两边的 header/payload 会交错，接收端按长度读必然错位。
 *      g_clients_mtx 只保护注册表，替代不了它。
 *
 *   2. **负载混淆必须作用在副本上。** broadcast_event 会把同一份 payload
 *      依次发给多个订阅者，而 XOR 自反 —— 原地改会让第 2 个订阅者收到明文
 *      却仍带 GGWF_ENC 标志，解码后即乱码。
 *
 *   3. **分片必须逐片独立 XOR。** 接收端按帧解密，整体加密后再切会让第二片
 *      起的解密偏移全错。整组分片要在同一次 g_send_mtx 持有期间发完，
 *      否则别的响应插进来，接收端会按 seq 拼错。
 *
 *   4. **超限响应要回明确的错误帧，不能静默不发。** 超限帧 send_frame 会拒发，
 *      客户端只能等连接层 300s 超时，从外部完全看不出是"结果太大"。
 *
 * send_frame_fragmented 必须和 send_frame 待在同一个 .c 里：它们共用
 * g_send_mtx，分开文件容易在改动时破坏"同一锁持有期间发完一组分片"。
 */
#include "engine_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 出站帧全局串行：多线程（多连接 + 分片组内同 fd）写同 fd 会交错 —— 锁内统一封装。
 * 事件广播已删除（AI 主力走 pull 而非 push），不再有广播线程竞争。 */
pthread_mutex_t g_send_mtx = PTHREAD_MUTEX_INITIALIZER;

/* 一次系统调用发出「header + payload」。
 *
 * 为什么要合并：原实现是两次 send_full（先 20B header、再 payload）。响应用户
 * 请求时 payload 往往只有几十字节，于是第二个包落在「前一个包未确认」的状态下，
 * 触发 Nagle；对端 TCP 的 delayed ACK 定时器是 40ms —— 实测每个单值请求
 * （READ/WRITE）稳定 40.0ms，而 64KB 读只要 19ms（满 MSS 段不受 Nagle 限制）。
 * 合并成单次 send 后没有"未确认小包"这个前提，40ms 空等消失。
 *
 * 拷贝代价：plen 最大 1MiB，多一次 memcpy 是 µs 级，远小于省下的 40ms。
 * 栈缓冲覆盖常见小响应，避免每次请求 malloc/free。 */
static int send_hdr_payload(int fd, const gg_wire_header_t *h,
                            const void *payload, size_t plen) {
    size_t total = sizeof(*h) + plen;
    unsigned char stackbuf[2048];
    unsigned char *buf = stackbuf;
    if (total > sizeof(stackbuf)) {
        buf = malloc(total);
        if (!buf) {
            log_msg("send_hdr_payload: oom allocating %zu bytes", total);
            return -1;
        }
    }
    memcpy(buf, h, sizeof(*h));
    if (plen) memcpy(buf + sizeof(*h), payload, plen);
    int rc = send_full(fd, buf, total);
    if (buf != stackbuf) free(buf);
    return rc;
}

int send_frame(int fd, uint16_t cmd, uint32_t seq, uint32_t flags,
                      char *payload, size_t plen) {
    /* 与接收侧同一个上限：超限帧发出去只会被 PC 端判为非法帧并断开连接，
     * 不如在这里明确失败（也避免 plen 被截断成 uint32）。 */
    if (plen > GGWM_MAX_PAYLOAD) {
        log_msg("send_frame: payload %zu exceeds GGWM_MAX_PAYLOAD %u",
                plen, (unsigned)GGWM_MAX_PAYLOAD);
        return -1;
    }
    gg_wire_header_t h;
    h.magic = GGWM_MAGIC;
    h.ver = GGWM_VERSION;
    h.cmd = cmd;
    h.seq = seq;
    /* ⚠️ 负载混淆必须作用在**副本**上，不能原地改写调用者缓冲：
     * broadcast_event 会把同一份 payload 依次发给多个订阅者，XOR 又是自反的，
     * 原地改会让第 2 个订阅者收到明文、却仍带 GGWF_ENC 标志（解码后即乱码）。 */
    char *enc = NULL;
    if (plen && payload) {
        enc = malloc(plen);
        if (!enc) {
            log_msg("send_frame: oom allocating %zu-byte payload copy", plen);
            return -1;
        }
        memcpy(enc, payload, plen);
        gg_xor_payload((unsigned char *)enc, plen);
        flags |= GGWF_ENC;
    }
    h.flags = flags;
    h.payload_len = (uint32_t)plen;
    pthread_mutex_lock(&g_send_mtx);
    int rc = send_hdr_payload(fd, &h, enc, plen);
    pthread_mutex_unlock(&g_send_mtx);
    free(enc);
    return rc;
}

/* 分片发送：把 plen 切成 ≤ GGWM_MAX_PAYLOAD 的块，除最后一块外都带 GGWF_MORE。
 *
 * XOR 必须**逐片独立**做：接收端是按帧解密的，整体加密后再切会让第二片
 * 起的解密偏移全错。整组分片在一次 g_send_mtx 持有期间发完 —— 否则中途
 * 被别的响应插进来，接收端会按 seq 拼错。
 */
int send_frame_fragmented(int fd, uint16_t cmd, uint32_t seq, uint32_t flags,
                                 const char *payload, size_t plen) {
    int rc = 0;
    pthread_mutex_lock(&g_send_mtx);
    size_t off = 0;
    while (off < plen) {
        size_t chunk = plen - off;
        if (chunk > (size_t)GGWM_MAX_PAYLOAD) chunk = (size_t)GGWM_MAX_PAYLOAD;
        int more = (off + chunk < plen);
        char *enc = malloc(chunk);
        if (!enc) { rc = -1; break; }
        memcpy(enc, payload + off, chunk);
        gg_xor_payload((unsigned char *)enc, chunk);

        gg_wire_header_t h;
        h.magic = GGWM_MAGIC;
        h.ver = GGWM_VERSION;
        h.cmd = cmd;
        h.seq = seq;
        h.flags = flags | GGWF_ENC | (more ? GGWF_MORE : 0u);
        h.payload_len = (uint32_t)chunk;
        if (send_hdr_payload(fd, &h, enc, chunk) < 0) {
            log_msg("fragmented send failed at off=%zu/%zu", off, plen);
            free(enc);
            rc = -1;
            break;
        }
        free(enc);
        off += chunk;
    }
    pthread_mutex_unlock(&g_send_mtx);
    return rc;
}

void send_ok(int fd, uint16_t cmd, uint32_t seq, cJSON *res) {
    char *s = cJSON_PrintUnformatted(res);
    if (!s) {
        /* 序列化失败（OOM）：原实现继续 strlen(NULL) → 在错误处理里再崩一次。
         * 交给 send_err 收尾，存档也是错误负载（不要留一个假的 "{}"）。 */
        send_err(fd, cmd, seq, GGERR_INTERNAL, "response serialization failed");
        return;
    }
    size_t len = strlen(s);
    if (!gg_payload_fits(len) && client_supports_frag(fd)) {
        /* 客户端声明过能重组（AUTH 带 "frag":1）→ 分片发完整结果。
         * 旧客户端没声明，下面仍走 GGERR_CAP —— 不能让它们收到看不懂的帧。 */
        log_msg("send_ok: %zu bytes > %u, sending fragmented", len,
                (unsigned)GGWM_MAX_PAYLOAD);
        send_frame_fragmented(fd, cmd, seq, GGWF_RESP, s, len);
        free(s);
        return;
    }
    if (!gg_payload_fits(len)) {
        /* 超限帧 send_frame 会直接拒发，客户端什么也收不到，只能等到连接层
         * 超时（PC 端 300s），从外部完全看不出是"结果太大"。改成明确的错误帧：
         * 把实际大小、上限和该怎么调一起告诉调用方。
         * 目前所有 handler 的输出都远低于上限（get_results 10000 条约 610–740KB，
         * hex_dump 单次 256KB → 512KB），这里是给「将来加大 limit / 字段变长」
         * 留的兜底，不是当前常见路径。 */
        log_msg("send_ok: response %zu bytes exceeds GGWM_MAX_PAYLOAD %u (cmd=%u)",
                len, (unsigned)GGWM_MAX_PAYLOAD, (unsigned)cmd);
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "response too large: %zu bytes exceeds the %u-byte frame limit; "
                 "reduce limit/len and page the results",
                 len, (unsigned)GGWM_MAX_PAYLOAD);
        free(s);
        send_err(fd, cmd, seq, GGERR_CAP, msg);
        return;
    }
    send_frame(fd, cmd, seq, GGWF_RESP, s, len);
    free(s);
}

void send_err(int fd, uint16_t cmd, uint32_t seq, int code, const char *msg) {
    cJSON *root = cJSON_CreateObject();
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg ? msg : "error");
    cJSON_AddItemToObject(root, GG_J_ERR, e);
    char *s = cJSON_PrintUnformatted(root);
    if (!s) {
        /* 连错误帧都序列化不出来：发一个静态最小负载。send_frame 不改写调用者
         * 缓冲，所以这里可以安全传字符串字面量。 */
        static const char kFallback[] = "{\"e\":{\"code\":12,\"message\":\"serialization failed\"}}";
        send_frame(fd, cmd, seq, GGWF_RESP | GGWF_ERR, (char *)kFallback, sizeof(kFallback) - 1);
        cJSON_Delete(root);
        return;
    }
    send_frame(fd, cmd, seq, GGWF_RESP | GGWF_ERR, s, strlen(s));
    free(s);
    cJSON_Delete(root);
}

/* =================================================================
 * 分片注册表（从 events.c 搬来，AI 主力下仅保留 frag_ok 标记）
 * ================================================================= */

typedef struct gg_client {
    int fd;
    int frag_ok;                 /* AUTH 时声明能重组分片响应（"frag":1） */
    struct gg_client *next;
} gg_client_t;

static gg_client_t *g_clients = NULL;
static pthread_mutex_t g_clients_mtx = PTHREAD_MUTEX_INITIALIZER;

void clients_register(int fd) {
    pthread_mutex_lock(&g_clients_mtx);
    gg_client_t *c = malloc(sizeof(*c));
    if (c) { c->fd = fd; c->frag_ok = 0; c->next = g_clients; g_clients = c; }
    pthread_mutex_unlock(&g_clients_mtx);
}

void clients_unregister(int fd) {
    pthread_mutex_lock(&g_clients_mtx);
    gg_client_t **pp = &g_clients;
    while (*pp) {
        gg_client_t *c = *pp;
        if (c->fd == fd) { *pp = c->next; free(c); break; }
        pp = &c->next;
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

void clients_set_frag(int fd, int on) {
    pthread_mutex_lock(&g_clients_mtx);
    for (gg_client_t *c = g_clients; c; c = c->next)
        if (c->fd == fd) { c->frag_ok = on ? 1 : 0; break; }
    pthread_mutex_unlock(&g_clients_mtx);
}

int client_supports_frag(int fd) {
    int ok = 0;
    pthread_mutex_lock(&g_clients_mtx);
    for (gg_client_t *c = g_clients; c; c = c->next)
        if (c->fd == fd) { ok = c->frag_ok; break; }
    pthread_mutex_unlock(&g_clients_mtx);
    return ok;
}
