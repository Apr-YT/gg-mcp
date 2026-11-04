/* cmd_mem.c — 内存读写 / 冻结 / 暂停恢复（自 main.c 拆出，2026-10-22）
 *
 * 域内命令（wire 层）：
 *   GGCMD_READ / GGCMD_READ_BYTES / GGCMD_HEX_DUMP / GGCMD_WRITE /
 *   GGCMD_FREEZE / GGCMD_UNFREEZE / GGCMD_FREEZE_RESULTS /
 *   GGCMD_PAUSE / GGCMD_RESUME
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
#include "dump.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* 批量冻结当前结果集（GGCMD_FREEZE_RESULTS）。
 *
 * 语义由是否带 value 分岔：
 *   - 带 value：结果集全部冻结成该值；
 *   - 不带 value：逐条读**当前值**再冻结（"锁住现状"），AI 把候选收敛到几十个后最常用。
 *
 * 三条不能随手改的约束：
 *   1. 冻结表只有 GG_FREEZE_MAX(256) 槽。超出的条目**不冻结**并在响应里回报 truncated ——
 *      静默丢弃会让调用方以为全冻上了，后续看到值还在变也查不出原因。
 *   2. 不要为了"能冻更多"去调大 GG_FREEZE_MAX：freeze_loop 是 100ms 全量重写，
 *      256 条已经是 2560 次驱动写/秒；表再大，单轮耗时就会超过周期，反过来拖慢目标进程。
 *   3. 短读的条目 skip 掉。冻结一个读不到的值等于凭空造值 —— 下一轮循环会把这个假值
 *      写进目标内存（比不冻结危险得多）。
 */
void h_freeze_results(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.safe_mode) { send_err(fd, GGCMD_FREEZE_RESULTS, seq, GGERR_SAFE_MODE, "safe mode: freeze denied"); return; }
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_FREEZE_RESULTS, seq, GGERR_NO_SESSION, "no session"); return; }
    /* 模糊搜索结果在 addrs 里是物化后的候选地址；不物化就冻结一批过期的候选 */
    if (g_eng.results.fuzzy_active) results_materialize(&g_eng.results);
    if (!g_eng.results.addrs || g_eng.results.count == 0) {
        send_err(fd, GGCMD_FREEZE_RESULTS, seq, GGERR_BAD_REQUEST, "no results (search first)");
        return;
    }
    /* def=0 用来区分"调用方没给 type"（→ 取结果集自身类型）与"给了非法值"（→ -1 报错）。
     * 不能默认成 VT_DWORD：qword 结果集会被写成半个值。 */
    int type = parse_vt(p, 0);
    if (type == 0) {
        type = g_eng.results.exact_vt > 0 ? g_eng.results.exact_vt : g_eng.results.fuzzy_vt;
    }
    if (type < 0) { send_err(fd, GGCMD_FREEZE_RESULTS, seq, GGERR_BAD_REQUEST, "invalid " VT_HINT); return; }
    int sz = vt_size(type);
    if (sz <= 0) { send_err(fd, GGCMD_FREEZE_RESULTS, seq, GGERR_BAD_REQUEST,
                            "type bytes not supported for freeze"); return; }

    uint8_t buf[8];
    int fixed = 0;
    const cJSON *jv = cJSON_GetObjectItem(p, "value");
    if (jv) {
        char vbuf[64];
        int ok = 0;
        uint64_t value = parse_value_str(type, j_scalar_str((cJSON *)jv, vbuf, sizeof(vbuf)), &ok);
        if (!ok) { send_err(fd, GGCMD_FREEZE_RESULTS, seq, GGERR_BAD_REQUEST, "bad value"); return; }
        encode_value_local(type, value, buf);
        fixed = 1;
    }

    size_t want = g_eng.results.count;
    const cJSON *jl = cJSON_GetObjectItem(p, "limit");
    if (jl && jl->valuedouble >= 1 && (size_t)jl->valuedouble < want) want = (size_t)jl->valuedouble;

    size_t room = freeze_count(&g_eng.freeze) < GG_FREEZE_MAX
                      ? GG_FREEZE_MAX - freeze_count(&g_eng.freeze) : 0;
    size_t done = 0, skipped = 0, truncated = 0;
    for (size_t i = 0; i < want; i++) {
        if (done >= room) { truncated = want - i; break; }
        uint64_t addr = g_eng.results.addrs[i];
        if (!fixed) {
            ssize_t got = driver_read(&g_eng.drv, g_eng.session_pid, addr, buf, (size_t)sz);
            if (got != (ssize_t)sz) { skipped++; continue; }
        }
        if (freeze_add(&g_eng.freeze, addr, buf, (size_t)sz) < 0) { skipped++; continue; }
        done++;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "frozen_new", (double)done);
    cJSON_AddNumberToObject(r, "frozen_total", (double)freeze_count(&g_eng.freeze));
    cJSON_AddNumberToObject(r, "skipped", (double)skipped);
    cJSON_AddNumberToObject(r, "truncated", (double)truncated);
    cJSON_AddNumberToObject(r, "capacity", (double)GG_FREEZE_MAX);
    send_ok(fd, GGCMD_FREEZE_RESULTS, seq, r);
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

/* ---------------- 冻结表持久化（GGCMD_FREEZE_SAVE / GGCMD_FREEZE_LOAD） ----------------
 * 挂机锁血场景 agentd 重启即冻结全断。落盘同 results save/load：
 * /data/adb/GG-Ghost/freezes/<name>.json，纯函数在 freeze.c。
 * load 填表后循环线程（attach 时已起）下一轮就写回：safe_mode 必须拒。 */
void h_freeze_save(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_FREEZE_SAVE, seq, GGERR_NO_SESSION, "no session"); return; }
    cJSON *jn = cJSON_GetObjectItem(p, "name");
    if (!cJSON_IsString(jn) || !gg_safe_result_name(jn->valuestring)) {
        send_err(fd, GGCMD_FREEZE_SAVE, seq, GGERR_BAD_REQUEST,
                 "name required: [A-Za-z0-9._-]{1,64}");
        return;
    }
    char *json = freeze_serialize(&g_eng.freeze);
    if (!json) { send_err(fd, GGCMD_FREEZE_SAVE, seq, GGERR_BAD_REQUEST, "freeze list empty"); return; }

    mkdir("/data/adb/GG-Ghost/freezes", 0700);
    char path[128];
    snprintf(path, sizeof(path), "/data/adb/GG-Ghost/freezes/%s.json", jn->valuestring);
    FILE *f = fopen(path, "w");
    if (!f) { free(json); send_err(fd, GGCMD_FREEZE_SAVE, seq, GGERR_PERM, "cannot write file"); return; }
    size_t len = strlen(json);
    int ok = fwrite(json, 1, len, f) == len;
    fclose(f);
    free(json);
    if (!ok) { send_err(fd, GGCMD_FREEZE_SAVE, seq, GGERR_INTERNAL, "short write"); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "saved", (double)freeze_count(&g_eng.freeze));
    cJSON_AddStringToObject(r, "path", path);
    send_ok(fd, GGCMD_FREEZE_SAVE, seq, r);
    cJSON_Delete(r);
}

void h_freeze_load(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.safe_mode) { send_err(fd, GGCMD_FREEZE_LOAD, seq, GGERR_SAFE_MODE, "safe mode: freeze denied"); return; }
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_FREEZE_LOAD, seq, GGERR_NO_SESSION, "no session"); return; }
    cJSON *jn = cJSON_GetObjectItem(p, "name");
    if (!cJSON_IsString(jn) || !gg_safe_result_name(jn->valuestring)) {
        send_err(fd, GGCMD_FREEZE_LOAD, seq, GGERR_BAD_REQUEST,
                 "name required: [A-Za-z0-9._-]{1,64}");
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/data/adb/GG-Ghost/freezes/%s.json", jn->valuestring);
    FILE *f = fopen(path, "r");
    if (!f) { send_err(fd, GGCMD_FREEZE_LOAD, seq, GGERR_BAD_REQUEST, "no such saved freeze list"); return; }
    /* 上限：256 条 × ~96B < 25KB，给 256KB 防异常文件 */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) { fclose(f); send_err(fd, GGCMD_FREEZE_LOAD, seq, GGERR_BAD_REQUEST, "bad file size"); return; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); send_err(fd, GGCMD_FREEZE_LOAD, seq, GGERR_INTERNAL, "oom"); return; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    int64_t n = freeze_deserialize(&g_eng.freeze, buf);
    free(buf);
    if (n < 0) { send_err(fd, GGCMD_FREEZE_LOAD, seq, GGERR_BAD_REQUEST, "corrupt saved freeze list (table untouched)"); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "count", (double)n);
    send_ok(fd, GGCMD_FREEZE_LOAD, seq, r);
    cJSON_Delete(r);
}

/* ---------------- 内存区域快照（GGCMD_REGION_DUMP / GGCMD_REGION_RESTORE） ----------------
 *
 * 与结果集 save/load（cmd_search.c）同走设备端落盘：大区域走 wire 回 PC 按 2.8MB/s
 * 要几十秒，本地 /data 直接写。文件格式与 FNV 见 dump.h。
 *
 * 残缺即失败是本对命令的核心语义：
 *   - dump 遇短读（页被回收 / is_force=0 读不了）直接报错删文件，不补 0 ——
 *     备份不完整没有恢复价值，而"补 0 的完整长度"会让 restore 把假零写回目标进程。
 *     注意 force 读在这台内核上会 panic（AGENTS 27），所以不做 force 逃生口；
 *   - restore 前先完整校验 FNV，文件落盘后损坏（重启/写穿/手工编辑）就拒绝写回，
 *     把坏数据写进目标内存比不恢复危险得多。
 */

static void region_dump_path(char *out, size_t outsz, const char *name) {
    snprintf(out, outsz, "/data/adb/GG-Ghost/dumps/%s.bin", name);
}

void h_region_dump(int fd, uint32_t seq, cJSON *p) {
    uint16_t cmd = GGCMD_REGION_DUMP;
    if (g_eng.session_pid <= 0) { send_err(fd, cmd, seq, GGERR_NO_SESSION, "no session"); return; }
    cJSON *jn = cJSON_GetObjectItem(p, "name");
    if (!cJSON_IsString(jn) || !gg_safe_result_name(jn->valuestring)) {
        send_err(fd, cmd, seq, GGERR_BAD_REQUEST, "name required: [A-Za-z0-9._-]{1,64}");
        return;
    }
    const cJSON *ja = cJSON_GetObjectItem(p, "addr");
    uint64_t addr = 0;
    if (!ja || !j_addr_u64((cJSON *)ja, &addr)) {
        send_err(fd, cmd, seq, GGERR_BAD_REQUEST, "addr required (hex string or number)");
        return;
    }
    const cJSON *jl = cJSON_GetObjectItem(p, "len");
    if (!jl || !cJSON_IsNumber(jl) || jl->valuedouble < 1 ||
        jl->valuedouble > (double)GG_DUMP_MAX_BYTES) {
        char msg[96];
        snprintf(msg, sizeof(msg), "len must be 1..%u bytes", GG_DUMP_MAX_BYTES);
        send_err(fd, cmd, seq, GGERR_BAD_REQUEST, msg);
        return;
    }
    uint64_t len = (uint64_t)jl->valuedouble;

    mkdir("/data/adb/GG-Ghost/dumps", 0700);
    char path[128];
    region_dump_path(path, sizeof(path), jn->valuestring);
    char err[192];
    if (gg_region_dump_to_file(&g_eng.drv, g_eng.session_pid, addr, len,
                               path, err, sizeof(err)) != 0) {
        send_err(fd, cmd, seq, GGERR_INTERNAL, err);
        return;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "bytes", (double)len);
    char abuf[32];
    snprintf(abuf, sizeof(abuf), "0x%llx", (unsigned long long)addr);
    cJSON_AddStringToObject(r, "addr", abuf);
    cJSON_AddStringToObject(r, "path", path);
    send_ok(fd, cmd, seq, r);
    cJSON_Delete(r);
}

void h_region_restore(int fd, uint32_t seq, cJSON *p) {
    uint16_t cmd = GGCMD_REGION_RESTORE;
    if (g_eng.safe_mode) { send_err(fd, cmd, seq, GGERR_SAFE_MODE, "safe mode: restore denied"); return; }
    if (g_eng.session_pid <= 0) { send_err(fd, cmd, seq, GGERR_NO_SESSION, "no session"); return; }
    cJSON *jn = cJSON_GetObjectItem(p, "name");
    if (!cJSON_IsString(jn) || !gg_safe_result_name(jn->valuestring)) {
        send_err(fd, cmd, seq, GGERR_BAD_REQUEST, "name required: [A-Za-z0-9._-]{1,64}");
        return;
    }
    /* addr 可选：缺省写回 dump 原址；显式给可把备份写到别处（对照分析用）。
     * override 语义用 0/非 0 区分：地址 0 本身不可写，无歧义。 */
    uint64_t addr = 0;
    const cJSON *ja = cJSON_GetObjectItem(p, "addr");
    if (ja && !j_addr_u64((cJSON *)ja, &addr)) {
        send_err(fd, cmd, seq, GGERR_BAD_REQUEST, "invalid addr");
        return;
    }
    char path[128];
    region_dump_path(path, sizeof(path), jn->valuestring);
    uint64_t out_addr = 0, out_len = 0;
    char err[192];
    if (gg_region_restore_from_file(&g_eng.drv, g_eng.session_pid, path, addr,
                                    &out_addr, &out_len, err, sizeof(err)) != 0) {
        send_err(fd, cmd, seq, GGERR_BAD_REQUEST, err);
        return;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "bytes", (double)out_len);
    char abuf[32];
    snprintf(abuf, sizeof(abuf), "0x%llx", (unsigned long long)out_addr);
    cJSON_AddStringToObject(r, "addr", abuf);
    cJSON_AddStringToObject(r, "source", path);
    send_ok(fd, cmd, seq, r);
    cJSON_Delete(r);
}

void h_dump_list(int fd, uint32_t seq, cJSON *p) {
    (void)p;
    uint16_t cmd = GGCMD_DUMP_LIST;
    gg_dump_entry_t ents[128];   /* 备份是低频手工操作，128 个槽足够；超出静默截断 */
    int n = gg_dump_list("/data/adb/GG-Ghost/dumps", ents, 128);
    if (n < 0) {
        /* 目录还没建过（从没 dump 过）：返回空列表而不是错误 */
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "dumps", cJSON_CreateArray());
        send_ok(fd, cmd, seq, r);
        cJSON_Delete(r);
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", ents[i].name);
        char abuf[32];
        snprintf(abuf, sizeof(abuf), "0x%llx", (unsigned long long)ents[i].orig_addr);
        cJSON_AddStringToObject(e, "orig_addr", abuf);
        cJSON_AddNumberToObject(e, "bytes", (double)ents[i].payload_len);
        cJSON_AddNumberToObject(e, "checksum", (double)ents[i].checksum);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(r, "dumps", arr);
    send_ok(fd, cmd, seq, r);
    cJSON_Delete(r);
}
