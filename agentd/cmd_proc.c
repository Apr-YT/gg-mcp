/* cmd_proc.c — 进程与会话域（自 main.c 拆出，2026-10-23，Phase 2·④）
 *
 * 域内命令（wire 层）：
 *   GGCMD_PING / SELF_TEST / LIST_PROCESSES / ATTACH / DETACH /
 *   TARGET_INFO / MODULE_LIST
 *
 * 三条不能动的约束：
 *
 *   1. **session_guard() 的调用时机不能让。** dispatch 里的顺序是
 *        lock(g_eng.mtx) -> session_guard() -> switch(cmd)
 *      它负责在每个请求前自愈会话（目标退出/pid 复用就清空结果集并报错）。
 *      搬文件不影响这个顺序 —— 但如果哪天顺手把它挪到锁外，就会出现
 *      "检查完会话、还没执行 handler，目标已经退出"的窗口。
 *
 *   2. **attach 必须先清旧会话再建新的。** session_drop() 会清结果集、
 *      清冻结表、清 watch；漏一个就会带着上一个进程的地址去读写新进程。
 *
 *   3. **pid 复用检测靠 session_starttime 比对**，不是靠"pid 还在不在"。
 *      目标退出后 pid 会很快被内核重新分配，只比 pid 会读写到无辜进程。
 */
#include "engine_internal.h"
#include "process.h"   /* process_exists / process_is_64bit / process_starttime /
                         * maps_cache_invalidate —— attach 与自检要用 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void h_ping(int fd, uint32_t seq, cJSON *p) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "pong", 1);
    cJSON_AddStringToObject(r, "agentd", "1.0.0");
    send_ok(fd, GGCMD_PING, seq, r);
    cJSON_Delete(r);
}

void h_self_test(int fd, uint32_t seq, cJSON *p) {
    int st = driver_self_test(&g_eng.drv);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "state", g_eng.drv.state);
    cJSON_AddStringToObject(r, "state_str", driver_state_str(g_eng.drv.state));
    cJSON_AddNumberToObject(r, "result", st);
    cJSON_AddNumberToObject(r, "reads", (double)g_eng.drv.stat_reads);
    cJSON_AddNumberToObject(r, "writes", (double)g_eng.drv.stat_writes);
    cJSON_AddNumberToObject(r, "fallbacks", (double)g_eng.drv.stat_fallbacks);
    cJSON_AddStringToObject(r, "error", g_eng.drv.last_error);
    send_ok(fd, GGCMD_SELF_TEST, seq, r);
    cJSON_Delete(r);
}

void h_list_processes(int fd, uint32_t seq, cJSON *p) {
    const char *filter = cJSON_GetObjectItem(p, "filter") ?
        cJSON_GetObjectItem(p, "filter")->valuestring : NULL;
    size_t n = 0;
    gg_proc_entry_t *arr = process_list(&n, filter);
    cJSON *r = cJSON_CreateObject();
    cJSON *procs = cJSON_AddArrayToObject(r, "processes");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "pid", arr[i].pid);
        cJSON_AddStringToObject(o, "name", arr[i].name);
        cJSON_AddStringToObject(o, "comm", arr[i].comm);
        cJSON_AddNumberToObject(o, "uid", arr[i].uid);
        cJSON_AddItemToArray(procs, o);
    }
    free(arr);
    send_ok(fd, GGCMD_LIST_PROCESSES, seq, r);
    cJSON_Delete(r);
}

pid_t resolve_pid(cJSON *p) {
    cJSON *pidj = cJSON_GetObjectItem(p, "pid");
    cJSON *namej = cJSON_GetObjectItem(p, "name");
    if (pidj && pidj->valuedouble > 0) return (pid_t)pidj->valuedouble;
    if (namej && namej->valuestring && *namej->valuestring) {
        size_t n = 0;
        gg_proc_entry_t *arr = process_list(&n, NULL);
        pid_t found = -1;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(arr[i].name, namej->valuestring) == 0 ||
                strcmp(arr[i].comm, namej->valuestring) == 0) {
                found = arr[i].pid;
                break;
            }
        }
        free(arr);
        return found;
    }
    return -1;
}

/* 会话自愈：目标已退出、或 pid 已被内核复用给别的进程时清理会话。
 *
 * 为什么必须做：session_pid 只是个编号。目标被杀后内核会回收它，很快又分配
 * 给新进程（最典型的就是刚被 kill 又自动重启的同一个 App），此后所有读写都会
 * 落到这个无关进程上 —— 而且**写操作会成功、不报任何错**，只有目标莫名崩溃或
 * 数据异常时才暴露。starttime（进程启动时刻）在同一个 pid 的新进程上必然不同，
 * 是唯一可靠判据；comm/uid/maps 都可能雷同。
 *
 * 调用方持 g_eng.mtx（与 h_attach/h_detach 同一临界区），内部不再加锁。
 * 返回 1 = 有可用会话；0 = 无会话或刚被清理（handler 会照常回 NO_SESSION，
 * 与"从未 attach"走同一条路径，客户端无需区分两种情形）。 */
/* 丢弃当前会话（停 watch/freeze、清结果集、失效 maps 缓存）。
 * reason 只用于 session_change 事件与日志。
 * 调用方持 g_eng.mtx。 */
void session_drop(const char *reason) {
    (void)reason;
    watch_stop(&g_watch);
    freeze_stop(&g_eng.freeze);
    /* 结果集必须一起清：里面存的都是**旧进程**的地址，留着会被当成新会话的
     * 结果直接拿去读写（正是要防的错进程写）。 */
    results_clear(&g_eng.results);
    maps_cache_invalidate();
    g_eng.session_pid = -1;
    g_eng.session_starttime = 0;
    g_eng.session_name[0] = 0;
}

int session_guard(void) {
    if (g_eng.session_pid <= 0) return 0;
    uint64_t st = process_starttime(g_eng.session_pid);
    int gone   = (st == 0);
    int reused = (!gone && g_eng.session_starttime != 0 &&
                  st != g_eng.session_starttime);
    if (!gone && !reused) return 1;

    log_msg("session self-heal: pid %d %s (starttime now=%llu expected=%llu)",
            (int)g_eng.session_pid, gone ? "exited" : "reused by another process",
            (unsigned long long)st, (unsigned long long)g_eng.session_starttime);
    session_drop(gone ? "target_exit" : "pid_reused");
    return 0;
}

void h_attach(int fd, uint32_t seq, cJSON *p) {
    pid_t pid = resolve_pid(p);
    /* attach 失败要**清掉旧会话**：否则客户端以为已经切到新目标，实际后续
     * 读写还落在上一个目标上（实测：attach 一个已退出的 pid 报错后，
     * search/read 仍然正常返回 —— 操作的是旧进程）。宁可让调用方重新 attach，
     * 也不能让它以为切了却没切。 */
    if (pid <= 0 || !process_exists(pid)) {
        if (g_eng.session_pid > 0) session_drop("attach_failed");
        send_err(fd, GGCMD_ATTACH, seq, GGERR_NOT_FOUND,
                 "process not found (previous session cleared)");
        return;
    }
    if (process_is_64bit(pid) == 0) {
        if (g_eng.session_pid > 0) session_drop("attach_failed");
        send_err(fd, GGCMD_ATTACH, seq, GGERR_ARCH_UNSUPPORTED,
                 "32-bit target not supported (previous session cleared)");
        return;
    }
    /* 清理旧会话 */
    watch_stop(&g_watch);
    freeze_stop(&g_eng.freeze);
    results_clear(&g_eng.results);
    results_init(&g_eng.results, g_eng.result_cap);
    maps_cache_invalidate();   /* maps 会话缓存随会话切换失效 */

    g_eng.session_pid = pid;
    g_eng.session_starttime = process_starttime(pid);
    snprintf(g_eng.session_name, sizeof(g_eng.session_name), "%d", (int)pid);
    size_t n = 0;
    gg_proc_entry_t *arr = process_list(&n, NULL);
    for (size_t i = 0; i < n; i++) {
        if (arr[i].pid == pid) {
            snprintf(g_eng.session_name, sizeof(g_eng.session_name), "%s", arr[i].name);
            break;
        }
    }
    free(arr);
    /* 冻结线程是可选能力：创建失败不该让 attach 整体失败，但必须留痕，
     * 否则冻结静默失效、调用方以为已生效 */
    if (freeze_start(&g_eng.freeze, pid) != 0)
        log_msg("freeze thread start failed (errno=%d %s) - freeze unavailable",
                errno, strerror(errno));

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "pid", pid);
    cJSON_AddStringToObject(r, "name", g_eng.session_name);
    cJSON_AddNumberToObject(r, "arch", 64);
    cJSON_AddNumberToObject(r, "driver_state", g_eng.drv.state);
    send_ok(fd, GGCMD_ATTACH, seq, r);
    cJSON_Delete(r);
}

void h_detach(int fd, uint32_t seq, cJSON *p) {
    watch_stop(&g_watch);
    freeze_stop(&g_eng.freeze);
    results_clear(&g_eng.results);
    maps_cache_invalidate();   /* maps 会话缓存随会话切换失效 */
    g_eng.session_pid = -1;
    g_eng.session_starttime = 0;
    g_eng.session_name[0] = 0;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    send_ok(fd, GGCMD_DETACH, seq, r);
    cJSON_Delete(r);
}

/* 模块名 = path basename（供客户端按 name 索引, 免解析 path） */
static const char *mod_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void h_target_info(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_TARGET_INFO, seq, GGERR_NO_SESSION, "no session"); return; }
    size_t nr = 0;
    gg_map_region_t *regs = process_maps(g_eng.session_pid, &nr);
    size_t nm = 0;
    gg_module_t *mods = process_modules(g_eng.session_pid, &nm);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "pid", g_eng.session_pid);
    cJSON_AddStringToObject(r, "name", g_eng.session_name);
    cJSON *regions = cJSON_AddArrayToObject(r, "regions");
    if (regs) {
        for (size_t i = 0; i < nr; i++) {
            cJSON *o = cJSON_CreateObject();
            char start[32], end[32];
            snprintf(start, sizeof(start), "0x%llx", (unsigned long long)regs[i].start);
            snprintf(end, sizeof(end), "0x%llx", (unsigned long long)regs[i].end);
            cJSON_AddStringToObject(o, "start", start);
            cJSON_AddStringToObject(o, "end", end);
            cJSON_AddStringToObject(o, "perms", regs[i].perms);
            cJSON_AddStringToObject(o, "path", regs[i].path);
            cJSON_AddItemToArray(regions, o);
        }
        free(regs);
    }
    cJSON *modules = cJSON_AddArrayToObject(r, "modules");
    if (mods) {
        for (size_t i = 0; i < nm; i++) {
            cJSON *o = cJSON_CreateObject();
            char base[32];
            snprintf(base, sizeof(base), "0x%llx", (unsigned long long)mods[i].base);
            cJSON_AddStringToObject(o, "path", mods[i].path);
            cJSON_AddStringToObject(o, "name", mod_basename(mods[i].path));
            cJSON_AddStringToObject(o, "base", base);
            cJSON_AddNumberToObject(o, "size", (double)mods[i].size);
            cJSON_AddItemToArray(modules, o);
        }
        free(mods);
    }
    send_ok(fd, GGCMD_TARGET_INFO, seq, r);
    cJSON_Delete(r);
}

void h_module_list(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_MODULE_LIST, seq, GGERR_NO_SESSION, "no session"); return; }
    size_t nm = 0;
    gg_module_t *mods = process_modules(g_eng.session_pid, &nm);
    cJSON *r = cJSON_CreateObject();
    cJSON *modules = cJSON_AddArrayToObject(r, "modules");
    for (size_t i = 0; i < nm; i++) {
        cJSON *o = cJSON_CreateObject();
        char base[32];
        snprintf(base, sizeof(base), "0x%llx", (unsigned long long)mods[i].base);
        cJSON_AddStringToObject(o, "path", mods[i].path);
        cJSON_AddStringToObject(o, "name", mod_basename(mods[i].path));
        cJSON_AddStringToObject(o, "base", base);
        cJSON_AddNumberToObject(o, "size", (double)mods[i].size);
        cJSON_AddItemToArray(modules, o);
    }
    free(mods);
    send_ok(fd, GGCMD_MODULE_LIST, seq, r);
    cJSON_Delete(r);
}

/* ---- type/region 参数兼容解析（2026-08-19） ----
 * MCP 层（PC 端 session.cpp）会把字符串转 int 再转发，但直连 agentd 的
 * 客户端（gg_direct.py 等）传字符串会拿到 valueint=0 → 静默失败/走默认。
 * 统一在此兼容两种形式：数字直接取，字符串按名字映射（与 PC 端
 * norm_type/norm_region 同一套名字）。 */
int parse_vt(const cJSON *p, int def) {
    const cJSON *j = cJSON_GetObjectItem(p, "type");
    if (!j) return def;
    if (j->type == cJSON_Number) {
        int v = j->valueint;
        if (v >= VT_BYTE && v <= VT_BYTES) return v;
        return -1;   /* 越界数字 → 调用方报错（替代静默走默认） */
    }
    if (j->type == cJSON_String) {
        const char *s = j->valuestring;
        if (s && *s) {
            if (!strcmp(s, "byte") || !strcmp(s, "u8")) return VT_BYTE;
            if (!strcmp(s, "word") || !strcmp(s, "u16")) return VT_WORD;
            if (!strcmp(s, "dword") || !strcmp(s, "u32") || !strcmp(s, "int")) return VT_DWORD;
            if (!strcmp(s, "qword") || !strcmp(s, "u64") || !strcmp(s, "long")) return VT_QWORD;
            if (!strcmp(s, "float") || !strcmp(s, "f32")) return VT_FLOAT;
            if (!strcmp(s, "double") || !strcmp(s, "f64")) return VT_DOUBLE;
            if (!strcmp(s, "bytes") || !strcmp(s, "aob")) return VT_BYTES;
        }
        return -1;   /* 字符串不识别 → 调用方报错 */
    }
    return def;
}
