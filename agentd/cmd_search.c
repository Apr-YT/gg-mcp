/* cmd_search.c — 搜索与结果集（自 main.c 拆出，2026-10-22）
 *
 * 域内命令（wire 层）：
 *   GGCMD_SEARCH / GGCMD_SEARCH_RANGE / GGCMD_SEARCH_UNKNOWN_INIT /
 *   GGCMD_SEARCH_REFINE / GGCMD_RESULT_COUNT / GGCMD_GET_RESULTS /
 *   GGCMD_CLEAR_RESULTS / GGCMD_SET_RESULTS
 *
 * 拆分纪律（与 cmd_shell.c / cmd_view.c 一致）：
 *   - 只消费 engine_internal.h 的共享声明（g_eng / send_ok / send_err / 事件广播）。
 *   - 引擎耦合仅 g_eng.drv / g_eng.session_pid / g_eng.results，搜索算法本身在 search.c。
 *   - 域内 helper（parse_region / parse_refine_op）保持 static，不污染共享头；
 *     parse_vt 因为读写冻结四域都在用，已上移到 engine_internal.h。
 *
 * 两个曾经的线上缺陷在这里留了注释，改之前先读：
 *   - parse_refine_op：cJSON 字符串节点的 valueint 恒 0，而 REFINE_* 从 1 起
 *     → 四个 op 全落 default，候选被整体清空（真机实测 refine 完全不可用）。
 *   - h_get_results：addrs/vals 分配失败时必须报错，不能回空结果集
 *     （调用方会当成"这页确实没数据"）。
 */
#include "engine_internal.h"

#include <stdlib.h>
#include <string.h>


static int parse_region(const cJSON *p, int def) {
    const cJSON *j = cJSON_GetObjectItem(p, "region");
    if (!j) return def;
    if (j->type == cJSON_Number) {
        int v = j->valueint;
        if (v >= REGION_ANONYMOUS && v <= REGION_ALL) return v;
        return -1;
    }
    if (j->type == cJSON_String) {
        const char *s = j->valuestring;
        if (s && *s) {
            if (!strcmp(s, "anonymous") || !strcmp(s, "anon")) return REGION_ANONYMOUS;
            if (!strcmp(s, "writable")) return REGION_WRITABLE;
            if (!strcmp(s, "all")) return REGION_ALL;
        }
        return -1;
    }
    return def;
}

void h_search(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_NO_SESSION, "no session"); return; }
    int type = parse_vt(p, VT_DWORD);
    int region = parse_region(p, REGION_ANONYMOUS);
    if (type < 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_BAD_REQUEST, "invalid " VT_HINT); return; }
    if (region < 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_BAD_REQUEST, "invalid " REGION_HINT); return; }
    char vbuf[64];
    const char *vs = j_scalar_str(cJSON_GetObjectItem(p, "value"), vbuf, sizeof(vbuf));
    int64_t found = 0;

    if (type == VT_BYTES) {
        /* AoB 特征码搜索: value = hex 字符串 "48 65 4C 4C" */
        uint8_t pat[256];
        int plen = parse_hex_pattern(vs, pat, sizeof(pat));
        if (plen <= 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_BAD_REQUEST, "bad aob pattern"); return; }
        found = search_aob(&g_eng.drv, g_eng.session_pid, &g_eng.results, pat, (size_t)plen, region);
        if (found < 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_INTERNAL, "aob search failed"); return; }
    } else {
        int ok = 0;
        uint64_t value = parse_value_str(type, vs, &ok);
        if (!ok) { send_err(fd, GGCMD_SEARCH, seq, GGERR_BAD_REQUEST, "bad value"); return; }
        found = search_exact(&g_eng.drv, g_eng.session_pid, &g_eng.results, type, value, region);
        if (found < 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_INTERNAL, "search failed"); return; }
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "count", (double)found);
    cJSON_AddBoolToObject(r, "truncated", found >= (int64_t)g_eng.results.cap);
    send_ok(fd, GGCMD_SEARCH, seq, r);
    cJSON_Delete(r);
}

void h_search_range(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_SEARCH_RANGE, seq, GGERR_NO_SESSION, "no session"); return; }
    int type = parse_vt(p, VT_DWORD);
    int region = parse_region(p, REGION_ANONYMOUS);
    if (type < 0) { send_err(fd, GGCMD_SEARCH_RANGE, seq, GGERR_BAD_REQUEST, "invalid " VT_HINT); return; }
    if (region < 0) { send_err(fd, GGCMD_SEARCH_RANGE, seq, GGERR_BAD_REQUEST, "invalid " REGION_HINT); return; }
    double vmin = cJSON_GetObjectItem(p, "min") ? cJSON_GetObjectItem(p, "min")->valuedouble : 0;
    double vmax = cJSON_GetObjectItem(p, "max") ? cJSON_GetObjectItem(p, "max")->valuedouble : 0;
    int64_t found = search_range(&g_eng.drv, g_eng.session_pid, &g_eng.results, type, vmin, vmax, region);
    if (found < 0) { send_err(fd, GGCMD_SEARCH_RANGE, seq, GGERR_INTERNAL, "range search failed"); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "count", (double)found);
    cJSON_AddBoolToObject(r, "truncated", found >= (int64_t)g_eng.results.cap);
    send_ok(fd, GGCMD_SEARCH_RANGE, seq, r);
    cJSON_Delete(r);
}

void h_unknown_init(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_SEARCH_UNKNOWN_INIT, seq, GGERR_NO_SESSION, "no session"); return; }
    int type = parse_vt(p, VT_DWORD);
    int region = parse_region(p, REGION_ANONYMOUS);
    if (type < 0) { send_err(fd, GGCMD_SEARCH_UNKNOWN_INIT, seq, GGERR_BAD_REQUEST, "invalid " VT_HINT); return; }
    if (region < 0) { send_err(fd, GGCMD_SEARCH_UNKNOWN_INIT, seq, GGERR_BAD_REQUEST, "invalid " REGION_HINT); return; }
    int64_t elems = search_unknown_init(&g_eng.drv, g_eng.session_pid, &g_eng.results, type, region);
    if (elems < 0) { send_err(fd, GGCMD_SEARCH_UNKNOWN_INIT, seq, GGERR_INTERNAL, "unknown-init failed"); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "elements", (double)elems);
    send_ok(fd, GGCMD_SEARCH_UNKNOWN_INIT, seq, r);
    cJSON_Delete(r);
}

/* refine 的 op：同时接受字符串与数字。
 *
 * schema 里 `gg_search_refine.op` 声明的是字符串 enum
 * （increased / decreased / unchanged / changed），PC 端会 norm 成数字再下发；
 * 但直连 wire 的脚本/客户端发的是字符串。原实现直接取 `->valueint` ——
 * cJSON 字符串节点该字段恒为 0，而枚举从 1 开始，于是四个 op 全部落到
 * switch 的 default，**结果集被整体清空**。
 * 真机实测：任何 refine 调用后 remaining 都是 0，refine 功能实际不可用。
 *
 * 非法值一律返回 -1 让调用方报 BAD_REQUEST，不要静默退化成某个默认操作
 * —— 猜错的默认会直接毁掉结果集。 */
static int parse_refine_op(const cJSON *p) {
    const cJSON *jo = cJSON_GetObjectItem(p, "op");
    if (!jo) return REFINE_CHANGED;
    if (cJSON_IsNumber(jo)) {
        int v = (int)jo->valuedouble;
        if (v >= REFINE_INCREASED && v <= REFINE_CHANGED) return v;
        return -1;
    }
    if (cJSON_IsString(jo)) {
        const char *s = jo->valuestring;
        if (!strcmp(s, "increased")) return REFINE_INCREASED;
        if (!strcmp(s, "decreased")) return REFINE_DECREASED;
        if (!strcmp(s, "unchanged")) return REFINE_UNCHANGED;
        if (!strcmp(s, "changed"))   return REFINE_CHANGED;
        /* undo 用 0 表示（枚举从 1 开始，不冲突）：回退最近一次模糊 refine */
        if (!strcmp(s, "undo"))      return 0;
    }
    return -1;
}

void h_refine(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_SEARCH_REFINE, seq, GGERR_NO_SESSION, "no session"); return; }
    int op = parse_refine_op(p);
    if (op < 0) {
        send_err(fd, GGCMD_SEARCH_REFINE, seq, GGERR_BAD_REQUEST,
                 "invalid op (increased|decreased|unchanged|changed|undo)");
        return;
    }
    if (op == 0) {
        /* 撤销上一步 refine：模糊搜索回退位图，精确/区间搜索回退地址数组。 */
        int64_t back = search_refine_undo(&g_eng.results);
        if (back < 0) back = search_refine_undo_exact(&g_eng.results);
        if (back < 0) {
            send_err(fd, GGCMD_SEARCH_REFINE, seq, GGERR_BAD_REQUEST,
                     "nothing to undo (no refine step on record)");
            return;
        }
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "remaining", (double)back);
        cJSON_AddBoolToObject(r, "undone", 1);
        send_ok(fd, GGCMD_SEARCH_REFINE, seq, r);
        cJSON_Delete(r);
        return;
    }
    int64_t rem;
    if (g_eng.results.fuzzy_active) {
        rem = search_refine(&g_eng.drv, g_eng.session_pid, &g_eng.results, op);
    } else {
        /* 精确/区间搜索结果: 按"当前值 vs 搜索时快照" refine（GG 标准流程） */
        rem = search_refine_exact(&g_eng.drv, g_eng.session_pid, &g_eng.results, op);
    }
    if (rem < 0) { send_err(fd, GGCMD_SEARCH_REFINE, seq, GGERR_INTERNAL, "refine failed (search first?)"); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "remaining", (double)rem);
    send_ok(fd, GGCMD_SEARCH_REFINE, seq, r);
    cJSON_Delete(r);
}

/* 反汇编目标进程内存（Capstone ARM64）: {addr, length} → {insns:[{addr,bytes,mnemonic,op_str,comment}]} */

void h_result_count(int fd, uint32_t seq, cJSON *p) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "count", (double)g_eng.results.count);
    cJSON_AddNumberToObject(r, "cap", (double)g_eng.results.cap);
    cJSON_AddBoolToObject(r, "fuzzy_active", g_eng.results.fuzzy_active ? 1 : 0);
    if (g_eng.results.fuzzy_active) {
        cJSON_AddNumberToObject(r, "fuzzy_candidates",
            (double)results_fuzzy_count(&g_eng.results));
    }
    send_ok(fd, GGCMD_RESULT_COUNT, seq, r);
    cJSON_Delete(r);
}

void h_get_results(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_GET_RESULTS, seq, GGERR_NO_SESSION, "no session"); return; }
    size_t offset = cJSON_GetObjectItem(p, "offset") ? (size_t)cJSON_GetObjectItem(p, "offset")->valuedouble : 0;
    size_t limit = cJSON_GetObjectItem(p, "limit") ? (size_t)cJSON_GetObjectItem(p, "limit")->valuedouble : 100;
    /* 值类型以结果集自身为准（精确搜索 exact_vt / 模糊物化 fuzzy_vt），调用方 type 仅兜底：
       避免类型不匹配读错字节数/错值（2026-08-18 CF 实测：带 type 偶发串旧 buffer） */
    int type = VT_DWORD;
    if (g_eng.results.exact_vt > 0) type = g_eng.results.exact_vt;
    else if (g_eng.results.fuzzy_vt > 0) type = g_eng.results.fuzzy_vt;
    if (limit > 10000) limit = 10000;

    uint64_t *addrs = malloc(limit * sizeof(uint64_t));
    uint64_t *vals = malloc(limit * sizeof(uint64_t));
    /* 分配失败必须明确报错：旧实现在 addrs/vals 任一为 NULL 时静默返回空结果集，
     * 调用方会把它当成「这页确实没数据」，从而误判搜索结果已空。 */
    if (!addrs || !vals) {
        free(addrs);
        free(vals);
        send_err(fd, GGCMD_GET_RESULTS, seq, GGERR_INTERNAL, "oom");
        return;
    }
    size_t got = 0;
    if (results_get_page(&g_eng.drv, g_eng.session_pid, &g_eng.results,
                         offset, limit, addrs, vals, type, &got) != 0) {
        free(addrs);
        free(vals);
        send_err(fd, GGCMD_GET_RESULTS, seq, GGERR_INTERNAL, "page read failed");
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(r, "results");
    for (size_t i = 0; i < got; i++) {
        cJSON *o = cJSON_CreateObject();
        char addr[32], val[32], vhex[24];
        snprintf(addr, sizeof(addr), "0x%llx", (unsigned long long)addrs[i]);
        snprintf(val, sizeof(val), "%llu", (unsigned long long)vals[i]);
        snprintf(vhex, sizeof(vhex), "0x%llx", (unsigned long long)vals[i]);
        cJSON_AddStringToObject(o, "addr", addr);
        cJSON_AddStringToObject(o, "value", val);      /* 十进制字符串（u64 精确） */
        cJSON_AddStringToObject(o, "value_hex", vhex);
        cJSON_AddItemToArray(items, o);
    }
    cJSON_AddNumberToObject(r, "total", (double)g_eng.results.count);
    free(addrs);
    free(vals);
    send_ok(fd, GGCMD_GET_RESULTS, seq, r);
    cJSON_Delete(r);
}

void h_clear_results(int fd, uint32_t seq, cJSON *p) {
    results_clear(&g_eng.results);
    results_init(&g_eng.results, g_eng.result_cap);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    send_ok(fd, GGCMD_CLEAR_RESULTS, seq, r);
    cJSON_Delete(r);
}


void h_set_results(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.safe_mode) { send_err(fd, GGCMD_SET_RESULTS, seq, GGERR_SAFE_MODE, "safe mode: write denied"); return; }
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_SET_RESULTS, seq, GGERR_NO_SESSION, "no session"); return; }
    int type = parse_vt(p, VT_DWORD);
    cJSON *vals = cJSON_GetObjectItem(p, "values");
    if (!cJSON_IsArray(vals)) { send_err(fd, GGCMD_SET_RESULTS, seq, GGERR_BAD_REQUEST, "values array required"); return; }
    /* vt_size(VT_BYTES)=0、非法 type 为负：逐条写 0 字节会被计成成功，
     * written 虚报成「全改完了」。与 h_write / h_freeze 对齐，入口一次拒绝。 */
    int isz = vt_size(type);
    if (isz <= 0) { send_err(fd, GGCMD_SET_RESULTS, seq, GGERR_BAD_REQUEST,
                             "type bytes not supported for set_results"); return; }
    if (g_eng.results.fuzzy_active) results_materialize(&g_eng.results);

    int n = cJSON_GetArraySize(vals);
    if (n > 10000) n = 10000;

    /* 逐条: 按 index 或 addr 解析目标地址, 按 value 写入 */
    size_t written = 0;
    size_t skipped = 0;   /* 解析失败/写入失败的条目：必须回报，否则 written 偏小却像是「全成功」 */
    int m = 0;
    for (int i = 0; i < n && m < n; i++) {
        cJSON *o = cJSON_GetArrayItem(vals, i);
        if (!cJSON_IsObject(o)) { skipped++; continue; }
        cJSON *idxj = cJSON_GetObjectItem(o, "index");
        cJSON *addj = cJSON_GetObjectItem(o, "addr");
        cJSON *vj = cJSON_GetObjectItem(o, "value");
        char vbuf[64];
        int ok = 0;
        uint64_t value = parse_value_str(type, j_scalar_str(vj, vbuf, sizeof(vbuf)), &ok);
        if (!ok) { skipped++; continue; }
        uint64_t addr = 0;
        if (addj) {
            /* 数字形式与字符串形式都要吃：这里在数组元素里，走不到 dispatch 的顶层地址闸门 */
            if (!j_addr_u64(addj, &addr)) { skipped++; continue; }
        } else if (idxj && cJSON_IsNumber(idxj) && idxj->valuedouble >= 0 &&
                   (size_t)idxj->valuedouble < g_eng.results.count) {
            addr = g_eng.results.addrs[(size_t)idxj->valuedouble];
        } else { skipped++; continue; }
        uint8_t buf[8];
        encode_value_local(type, value, buf);
        if (driver_write(&g_eng.drv, g_eng.session_pid, addr, buf, (size_t)isz) == (ssize_t)isz) {
            written++;
        } else {
            skipped++;
        }
        m++;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "written", (double)written);
    cJSON_AddNumberToObject(r, "skipped", (double)skipped);
    send_ok(fd, GGCMD_SET_RESULTS, seq, r);
    cJSON_Delete(r);
}

