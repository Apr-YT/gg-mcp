/* cmd_mem.c — 内存读写 / 冻结 / 暂停恢复（自 main.c 拆出，2026-10-22）
 *
 * 域内命令（wire 层）：
 *   GGCMD_READ / GGCMD_READ_BYTES / GGCMD_HEX_DUMP / GGCMD_WRITE /
 *   GGCMD_FREEZE / GGCMD_UNFREEZE / GGCMD_PAUSE / GGCMD_RESUME
 *
 * 三个容易改错的地方（都在下面的注释里标了）：
 *   - h_read_bytes 的 cmd 由调用方传入：READ_BYTES 与 HEX_DUMP 共用它，
 *     响应帧必须回填请求里的那个 cmd（旧实现硬编码成 READ_BYTES）。
 *   - 写类命令（write / freeze）逐条校验 vt_size，0 字节会被计成"成功"。
 *   - safe_mode 是只读闸门：write / freeze 必须在入口拒绝。
 *
 * 拆分纪律：只消费 engine_internal.h（g_eng / send_ok / send_err / 参数解析）
 * 与 freeze.h / search.h（vt_size / encode_value_local），引擎耦合仅 g_eng.drv。
 */
#include "engine_internal.h"
#include "freeze.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


void h_read(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_READ, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    int type = parse_vt(p, VT_DWORD);
    if (type < 0) { send_err(fd, GGCMD_READ, seq, GGERR_BAD_REQUEST, "invalid " VT_HINT); return; }
    if (!as) { send_err(fd, GGCMD_READ, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    int sz = vt_size(type);
    /* VT_BYTES 的 vt_size 是 0：拿它做单值读会以 0 长度调驱动，再把零初始化的 buf 当结果 */
    if (sz <= 0) { send_err(fd, GGCMD_READ, seq, GGERR_BAD_REQUEST,
                            "type bytes not supported for single-value read (use gg_read_bytes)"); return; }
    uint64_t addr = strtoull(as, NULL, 0);
    uint8_t buf[8] = {0};
    ssize_t got = driver_read_ex(&g_eng.drv, g_eng.session_pid, addr, buf, (size_t)sz,
                                 parse_force(p));
    if (got < 0) { send_err(fd, GGCMD_READ, seq, GGERR_PERM, g_eng.drv.last_error); return; }
    /* 短读不能当成功：buf 是零初始化的，1/4 字节会把"页读不到"报成"值是 0" */
    if (got != (ssize_t)sz) {
        char msg[128];
        snprintf(msg, sizeof(msg), "short read: %lld of %d bytes (page not resident?)",
                 (long long)got, sz);
        send_err(fd, GGCMD_READ, seq, GGERR_INTERNAL, msg);
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "value", (double)read_bits(buf, type));
    cJSON_AddNumberToObject(r, "bytes", (double)got);
    send_ok(fd, GGCMD_READ, seq, r);
    cJSON_Delete(r);
}

/* cmd 由调用方传入：HEX_DUMP 与 READ_BYTES 共用本函数，响应帧的 cmd 必须
 * 回填请求里的那个值。旧实现硬编码 GGCMD_READ_BYTES，导致 gg_hex_dump 的
 * 响应 cmd=16 而请求是 17 —— 违反「响应 cmd 回填请求 cmd」的协议约定。 */
void h_read_bytes(int fd, uint16_t cmd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, cmd, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    double len_d = cJSON_GetObjectItem(p, "len") ? cJSON_GetObjectItem(p, "len")->valuedouble : 64;
    if (!as) { send_err(fd, cmd, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    /* len 走 double 再转 size_t：负数/0 会变成巨大值或 0 长度。必须先判范围再转。 */
    if (!(len_d >= 1)) { send_err(fd, cmd, seq, GGERR_BAD_REQUEST, "len must be >= 1"); return; }
    size_t len = (size_t)len_d;
    if (len > GG_SEARCH_CHUNK) len = GG_SEARCH_CHUNK;   /* 单次读上限 256KB（= RWPROC_MAX_IO） */
    uint64_t addr = strtoull(as, NULL, 0);
    uint8_t *buf = malloc(len);
    if (!buf) { send_err(fd, cmd, seq, GGERR_INTERNAL, "out of memory"); return; }
    ssize_t got = driver_read_ex(&g_eng.drv, g_eng.session_pid, addr, buf, len, parse_force(p));
    cJSON *r = cJSON_CreateObject();
    if (got < 0) {
        cJSON_AddStringToObject(r, "hex", "");
        cJSON_AddNumberToObject(r, "bytes", 0);
        cJSON_AddStringToObject(r, "error", g_eng.drv.last_error);
    } else {
        char *hex = malloc((size_t)got * 2 + 1);
        if (!hex) { free(buf); send_err(fd, cmd, seq, GGERR_INTERNAL, "out of memory"); return; }
        for (ssize_t i = 0; i < got; i++) snprintf(hex + i * 2, 3, "%02x", buf[i]);
        cJSON_AddStringToObject(r, "hex", hex);
        cJSON_AddNumberToObject(r, "bytes", (double)got);
        free(hex);
        /* got==0 不是"读到全零"，而是内核判定该页读不了。rwproc 走 is_force=0
         * （不翻转 PTE，避免与目标进程活跃页竞争导致内核崩溃），所以**已被回收的
         * 文件页读不到**。不解释清楚会被当成工具坏了。 */
        if (got == 0)
            cJSON_AddStringToObject(r, "note",
                "read 0 bytes: page not resident (is_force=0 does not flip PTE). "
                "Try an address in an executing/active mapping, or gg_search_range within a mapped region");
    }
    free(buf);
    send_ok(fd, cmd, seq, r);
    cJSON_Delete(r);
}

void h_write(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.safe_mode) { send_err(fd, GGCMD_WRITE, seq, GGERR_SAFE_MODE, "safe mode: write denied"); return; }
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_WRITE, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    int type = parse_vt(p, VT_DWORD);
    char vbuf[64];
    const char *vs = j_scalar_str(cJSON_GetObjectItem(p, "value"), vbuf, sizeof(vbuf));
    if (!as || !vs) { send_err(fd, GGCMD_WRITE, seq, GGERR_BAD_REQUEST, "addr+value required"); return; }
    int ok = 0;
    uint64_t value = parse_value_str(type, vs, &ok);
    if (!ok) { send_err(fd, GGCMD_WRITE, seq, GGERR_BAD_REQUEST, "bad value"); return; }
    int sz = vt_size(type);
    if (sz <= 0) { send_err(fd, GGCMD_WRITE, seq, GGERR_BAD_REQUEST,
                            "type bytes not supported for single-value write"); return; }
    uint64_t addr = strtoull(as, NULL, 0);
    uint8_t buf[8];
    encode_value_local(type, value, buf);
    ssize_t w = driver_write(&g_eng.drv, g_eng.session_pid, addr, buf, (size_t)sz);
    if (w < 0) { send_err(fd, GGCMD_WRITE, seq, GGERR_PERM, g_eng.drv.last_error); return; }
    /* 短写必须报错：只判 <0 会把"只落进 2/4 字节"回报成成功，调用方以为改完了 */
    if (w != (ssize_t)sz) {
        char msg[128];
        snprintf(msg, sizeof(msg), "short write: %lld of %d bytes", (long long)w, sz);
        send_err(fd, GGCMD_WRITE, seq, GGERR_INTERNAL, msg);
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "bytes", (double)w);
    send_ok(fd, GGCMD_WRITE, seq, r);
    cJSON_Delete(r);
}

void h_freeze(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.safe_mode) { send_err(fd, GGCMD_FREEZE, seq, GGERR_SAFE_MODE, "safe mode: freeze denied"); return; }
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_FREEZE, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    int type = parse_vt(p, VT_DWORD);
    char vbuf[64];
    const char *vs = j_scalar_str(cJSON_GetObjectItem(p, "value"), vbuf, sizeof(vbuf));
    if (!as || !vs) { send_err(fd, GGCMD_FREEZE, seq, GGERR_BAD_REQUEST, "addr+value required"); return; }
    int ok = 0;
    uint64_t value = parse_value_str(type, vs, &ok);
    if (!ok) { send_err(fd, GGCMD_FREEZE, seq, GGERR_BAD_REQUEST, "bad value"); return; }
    int sz = vt_size(type);
    /* vt_size(VT_BYTES)=0：冻结一个 0 长度的值只会让冻结线程空转，且表项永不清除 */
    if (sz <= 0) { send_err(fd, GGCMD_FREEZE, seq, GGERR_BAD_REQUEST,
                            "type bytes not supported for freeze"); return; }
    uint8_t buf[8];
    encode_value_local(type, value, buf);
    if (freeze_add(&g_eng.freeze, strtoull(as, NULL, 0), buf, (size_t)sz) < 0) {
        send_err(fd, GGCMD_FREEZE, seq, GGERR_INTERNAL, "freeze table full");
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "frozen", (double)freeze_count(&g_eng.freeze));
    send_ok(fd, GGCMD_FREEZE, seq, r);
    cJSON_Delete(r);
}

void h_unfreeze(int fd, uint32_t seq, cJSON *p) {
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (as) freeze_remove(&g_eng.freeze, strtoull(as, NULL, 0));
    else freeze_clear(&g_eng.freeze);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "frozen", (double)freeze_count(&g_eng.freeze));
    send_ok(fd, GGCMD_UNFREEZE, seq, r);
    cJSON_Delete(r);
}

void h_pause_resume(int fd, uint32_t seq, cJSON *p, int pause) {
    if (g_eng.session_pid <= 0) {
        send_err(fd, pause ? GGCMD_PAUSE : GGCMD_RESUME, seq, GGERR_NO_SESSION, "no session");
        return;
    }
    int sig = pause ? SIGSTOP : SIGCONT;
    int rc = kill(g_eng.session_pid, sig);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "signal", sig);
    cJSON_AddNumberToObject(r, "rc", rc);
    send_ok(fd, pause ? GGCMD_PAUSE : GGCMD_RESUME, seq, r);
    cJSON_Delete(r);
}
