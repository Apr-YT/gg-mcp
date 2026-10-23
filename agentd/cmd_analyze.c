/* cmd_analyze.c — 分析域（自 main.c 拆出，2026-10-23，Phase 2·⑤）
 *
 * 域内命令（wire 层）：
 *   GGCMD_DISASSEMBLE / WATCH（set/clear/poll/inject 四合一）/ MODULE_BASE /
 *   CALC_OFFSET / POINTER_SCAN
 *
 * 这是用户"搜到值之后"最常用的下一步：反汇编看看、下个硬件断点看谁访问、
 * 算个 base+offset、反查谁指向它。
 *
 * 三条不能动的约束：
 *
 *   1. **hwbp 探测结果 g_hwbp_probe 是全局缓存的，不是每次调用都探。**
 *      探测本身会往断点槽里注册东西（有副作用），一次进程生命周期只该探一次。
 *      g_hwbp_probe: -1 未探 / 0 不安全 / 1 可用；原始返回值存在
 *      g_hwbp_probe_rc，用于把错误说准（-1 是节点没开、4 才是 0 起始枚举的旧模块）。
 *
 *   2. **h_watch 是四合一（set/clear/poll/inject），靠 p 里的 "op" 分流。**
 *      没有 op 时默认按旧语义处理 —— 别拆成四个 handler，schema 那边
 *      只有一个 gg_watch_* 家族共用一个 wire 命令。
 *
 *   3. **h_pointer_scan 依赖 g_eng.results 里有结果。** 没有结果集时它扫的是
 *      空数组，会"成功返回 0 条"，看起来像"没有指针指向" —— 所以先判
 *      session_pid 和结果数，别让调用方误判。
 */
#include "engine_internal.h"
#include "disasm.h"
#include "elfsym.h"
#include "process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void h_disassemble(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) {
        send_err(fd, GGCMD_DISASSEMBLE, seq, GGERR_NO_SESSION, "no session");
        return;
    }
    const char *as = cJSON_GetObjectItem(p, "addr") ?
        cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (!as) { send_err(fd, GGCMD_DISASSEMBLE, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    uint64_t addr = strtoull(as, NULL, 0);
    int length = cJSON_GetObjectItem(p, "length") ?
        cJSON_GetObjectItem(p, "length")->valueint : 64;
    if (length <= 0) length = 64;
    if (length > 4096) length = 4096;   /* 上限 4KB */

    /* ARM64 指令必须 4 字节对齐。给个非对齐地址，Capstone 会从错误边界解码，
     * 吐出一两条"看着像指令"的垃圾然后停住 —— 这比直接报错更误导：调用方
     * 会以为那里真有个奇怪函数（实测 base+0x76392e 只回 1 条 `and w0,w0,w13`）。 */
    if (addr % 4 != 0) {
        send_err(fd, GGCMD_DISASSEMBLE, seq, GGERR_BAD_REQUEST,
                 "addr must be 4-byte aligned for arm64");
        return;
    }

    uint8_t *buf = malloc((size_t)length);
    if (!buf) { send_err(fd, GGCMD_DISASSEMBLE, seq, GGERR_INTERNAL, "oom"); return; }
    ssize_t got = driver_read(&g_eng.drv, g_eng.session_pid, addr, buf, (size_t)length);
    if (got <= 0) {
        free(buf);
        send_err(fd, GGCMD_DISASSEMBLE, seq, GGERR_INTERNAL, "read failed");
        return;
    }

    char mnem[GG_DISASM_MAX_INSNS][64];
    char ops[GG_DISASM_MAX_INSNS][128];
    char bytes[GG_DISASM_MAX_INSNS][32];
    char comment[GG_DISASM_MAX_INSNS][64];
    int n = disasm_a64(buf, (size_t)got, addr, mnem, ops, bytes, comment,
                       GG_DISASM_MAX_INSNS);
    free(buf);
    if (n < 0) {
        send_err(fd, GGCMD_DISASSEMBLE, seq, GGERR_INTERNAL, "capstone init failed");
        return;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "arch", "arm64");
    cJSON_AddStringToObject(r, "start", as);
    cJSON_AddNumberToObject(r, "bytes_read", (double)got);
    cJSON_AddNumberToObject(r, "count", (double)n);
    cJSON *arr = cJSON_AddArrayToObject(r, "insns");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        char ab[32];
        snprintf(ab, sizeof(ab), "0x%llx", (unsigned long long)(addr + (uint64_t)(i * 4)));
        cJSON_AddStringToObject(o, "addr", ab);
        cJSON_AddStringToObject(o, "bytes", bytes[i]);
        cJSON_AddStringToObject(o, "mnemonic", mnem[i]);
        cJSON_AddStringToObject(o, "op_str", ops[i]);
        cJSON_AddStringToObject(o, "comment", comment[i]);
        cJSON_AddItemToArray(arr, o);
    }
    send_ok(fd, GGCMD_DISASSEMBLE, seq, r);
    cJSON_Delete(r);
}

void h_watch(int fd, uint32_t seq, cJSON *p) {
    cJSON *opj = cJSON_GetObjectItem(p, "op");
    /* 字段存在但不是字符串时 valuestring 为 NULL，后面 strcmp(NULL, ...) 直接崩 */
    if (opj && !cJSON_IsString(opj)) {
        send_err(fd, GGCMD_WATCH, seq, GGERR_BAD_REQUEST,
                 "op must be a string (set|clear|poll|...)");
        return;
    }
    const char *op = opj ? opj->valuestring : "set";
    cJSON *r = cJSON_CreateObject();
    if (strcmp(op, "set") == 0) {
        /* 首次使用时探测一次模块的命令码基准，而不是一律拒绝：
         * 自编译模块 cmd=3 -> GET_NUM_BRPS 返回 6，可以安全使用；
         * afei 的 0 起始 prebuilt 在同一条命令上返回 4，它发 INST 会把
         * put_pid 打到用户态整数上直接崩内核。--hwbp 可跳过探测（自担风险）。 */
        if (g_hwbp_probe < 0) {
            int probe = rwproc_hwbp_probe(&g_eng.drv);
            g_hwbp_probe_rc = probe;
            g_hwbp_probe = (probe == 6) ? 1 : 0;
            log_msg("hwbp probe: GET_NUM_BRPS=%d (expect 6) -> %s",
                    probe, g_hwbp_probe ? "usable" : "refused");
        }
        if (!g_hwbp_allowed && !g_hwbp_probe) {
            cJSON_Delete(r);
            /* 两种失败长得很像但原因完全不同，别再合成一句话：
             *   -1 = 断点节点没打开 / 打开的是内存节点 → 看部署与 key 配置
             *    4 = 0 起始枚举的旧模块（afei prebuilt）→ 换自编译模块 */
            if (g_hwbp_probe_rc < 0) {
                send_err(fd, GGCMD_WATCH, seq, GGERR_DEGRADED,
                         "hwbp unavailable: the breakpoint module node is not usable "
                         "(GET_NUM_BRPS failed). Check that rwNova / hwBreakpointProc_module "
                         "is loaded and that --rwproc-bp-key points at it, not at the memory "
                         "module. See gg_kernel_status / agentd log for the node path.");
            } else {
                send_err(fd, GGCMD_WATCH, seq, GGERR_DEGRADED,
                         "hwbp unavailable: GET_NUM_BRPS returned 4, which means the loaded "
                         "module uses a 0-based command enum (afei prebuilt), so INST would "
                         "panic the kernel. Deploy the self-built module "
                         "(scripts/build_hwbp_ko.sh), or force with agentd --hwbp");
            }
            return;
        }
        if (g_eng.session_pid <= 0) {
            cJSON_Delete(r);
            send_err(fd, GGCMD_WATCH, seq, GGERR_NO_SESSION, "no session");
            return;
        }
        const char *as = cJSON_GetObjectItem(p, "addr") ?
            cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
        if (!as) { cJSON_Delete(r); send_err(fd, GGCMD_WATCH, seq, GGERR_BAD_REQUEST, "addr required"); return; }
        uint64_t addr = strtoull(as, NULL, 0);
        int btype = cJSON_GetObjectItem(p, "type") ?
            cJSON_GetObjectItem(p, "type")->valueint : HW_BP_TYPE_W;
        int blen = cJSON_GetObjectItem(p, "len") ?
            cJSON_GetObjectItem(p, "len")->valueint : 4;
        if (g_watch.active) watch_stop(&g_watch);   /* 重复 set 前先清旧的, 防断点槽位泄漏 */
        int rc = watch_start(&g_watch, &g_eng.drv, g_eng.session_pid, addr, btype, blen);
        cJSON_AddBoolToObject(r, "active", rc == 0 ? 1 : 0);
        cJSON_AddNumberToObject(r, "threads", g_watch.ntids);
        cJSON_AddStringToObject(r, "info", g_watch.last_error);
    } else if (strcmp(op, "clear") == 0) {
        watch_stop(&g_watch);
        cJSON_AddBoolToObject(r, "active", 0);
    } else if (strcmp(op, "poll") == 0) {
        gg_watch_hit_t hits[64];
        int n = watch_poll(&g_watch, &g_eng.drv, hits, 64);
        cJSON_AddNumberToObject(r, "hits", n);
        cJSON_AddBoolToObject(r, "active", g_watch.active ? 1 : 0);
        cJSON *arr = cJSON_AddArrayToObject(r, "list");
        for (int i = 0; i < n; i++) {
            cJSON_AddItemToArray(arr, watch_hit_json(&hits[i]));
        }
    } else if (strcmp(op, "inject") == 0) {
        /* 调试: 手动构造命中并走完整 enrich 管道（断点不可用环境验证日志） */
        if (g_eng.session_pid <= 0) {
            cJSON_Delete(r);
            send_err(fd, GGCMD_WATCH, seq, GGERR_NO_SESSION, "no session");
            return;
        }
        uint64_t pc = cJSON_GetObjectItem(p, "pc") ?
            strtoull(cJSON_GetObjectItem(p, "pc")->valuestring, NULL, 0) : 0;
        uint64_t fp = cJSON_GetObjectItem(p, "fp") ?
            strtoull(cJSON_GetObjectItem(p, "fp")->valuestring, NULL, 0) : 0;
        uint64_t sp = cJSON_GetObjectItem(p, "sp") ?
            strtoull(cJSON_GetObjectItem(p, "sp")->valuestring, NULL, 0) : 0;
        uint64_t lr = cJSON_GetObjectItem(p, "lr") ?
            strtoull(cJSON_GetObjectItem(p, "lr")->valuestring, NULL, 0) : 0;
        gg_watch_hit_t hit;
        watch_inject(&g_watch, &g_eng.drv, g_eng.session_pid, pc, fp, sp, lr, 0, &hit);
        cJSON_AddNumberToObject(r, "hits", 1);
        cJSON_AddItemToObject(r, "hit", watch_hit_json(&hit));
    } else {
        cJSON_Delete(r);
        send_err(fd, GGCMD_WATCH, seq, GGERR_BAD_REQUEST, "op must be set|clear|poll");
        return;
    }
    send_ok(fd, GGCMD_WATCH, seq, r);
    cJSON_Delete(r);
}


void h_module_base(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_MODULE_BASE, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *name = cJSON_GetObjectItem(p, "name") ? cJSON_GetObjectItem(p, "name")->valuestring : NULL;
    if (!name) { send_err(fd, GGCMD_MODULE_BASE, seq, GGERR_BAD_REQUEST, "name required"); return; }
    size_t nm = 0;
    gg_module_t *mods = process_modules(g_eng.session_pid, &nm);
    cJSON *r = cJSON_CreateObject();
    int found = 0;
    for (size_t i = 0; i < nm; i++) {
        if (strstr(mods[i].path, name)) {
            char base[32];
            snprintf(base, sizeof(base), "0x%llx", (unsigned long long)mods[i].base);
            cJSON_AddStringToObject(r, "path", mods[i].path);
            cJSON_AddStringToObject(r, "base", base);
            cJSON_AddNumberToObject(r, "size", (double)mods[i].size);
            found = 1;
            break;
        }
    }
    free(mods);
    if (!found) { send_err(fd, GGCMD_MODULE_BASE, seq, GGERR_NOT_FOUND, "module not found"); return; }
    send_ok(fd, GGCMD_MODULE_BASE, seq, r);
    cJSON_Delete(r);
}

void h_calc_offset(int fd, uint32_t seq, cJSON *p) {
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (!as) { send_err(fd, GGCMD_CALC_OFFSET, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    uint64_t addr = strtoull(as, NULL, 0);
    const char *modname = cJSON_GetObjectItem(p, "module") ?
        cJSON_GetObjectItem(p, "module")->valuestring : NULL;
    if (modname && g_eng.session_pid > 0) {
        size_t nm = 0;
        gg_module_t *mods = process_modules(g_eng.session_pid, &nm);
        for (size_t i = 0; i < nm; i++) {
            if (strstr(mods[i].path, modname)) {
                cJSON *r = cJSON_CreateObject();
                char off[32];
                snprintf(off, sizeof(off), "0x%llx", (unsigned long long)(addr - mods[i].base));
                cJSON_AddStringToObject(r, "offset", off);
                cJSON_AddStringToObject(r, "base", mods[i].path);
                free(mods);
                send_ok(fd, GGCMD_CALC_OFFSET, seq, r);
                cJSON_Delete(r);
                return;
            }
        }
        free(mods);
        send_err(fd, GGCMD_CALC_OFFSET, seq, GGERR_NOT_FOUND, "module not found");
        return;
    }
    /* 无模块: 相对匿名堆基址（当前结果首地址近似） */
    cJSON *r = cJSON_CreateObject();
    char off[32];
    snprintf(off, sizeof(off), "0x%llx", (unsigned long long)addr);
    cJSON_AddStringToObject(r, "offset", off);
    cJSON_AddStringToObject(r, "note", "no module given; raw address returned");
    send_ok(fd, GGCMD_CALC_OFFSET, seq, r);
    cJSON_Delete(r);
}

void h_pointer_scan(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_POINTER_SCAN, seq, GGERR_NO_SESSION, "no session"); return; }
    int type = parse_vt(p, VT_QWORD);
    size_t cap = cJSON_GetObjectItem(p, "cap") ? (size_t)cJSON_GetObjectItem(p, "cap")->valuedouble : 5000;
    /* cap 直接来自客户端，必须钳制：否则 cap=1e9 会 malloc 数十 GB，
     * 而 malloc 失败后原代码仍把 NULL 交给 pointer_scan 和下面的解引用。 */
    if (cap == 0) cap = 5000;
    if (cap > 100000) cap = 100000;
    if (g_eng.results.fuzzy_active) results_materialize(&g_eng.results);

    /* lookback：指针指向"对象首址"而目标在对象内部时，精确匹配必然搜不到
     * （C++ 对象普遍如此）。给一个回看范围，命中条件放宽成
     * [目标-lookback, 目标+lookback]，一次抓出所有指向该对象的指针。
     * 上限 1MB：再大就把整片堆都算进来了，那还不如不做。
     * 默认 0 = 精确匹配，与旧行为一致。 */
    uint64_t lookback = 0;
    cJSON *jl = cJSON_GetObjectItem(p, "lookback");
    if (cJSON_IsNumber(jl) && jl->valuedouble > 0) {
        lookback = (uint64_t)jl->valuedouble;
        if (lookback > 0x100000) lookback = 0x100000;
    }
    /* aligned：默认 1（只扫自然对齐位置）。非对齐命中基本是二进制数据巧合。 */
    int aligned = 1;
    cJSON *ja = cJSON_GetObjectItem(p, "aligned");
    if (ja) {
        if (cJSON_IsBool(ja)) aligned = cJSON_IsTrue(ja) ? 1 : 0;
        else if (cJSON_IsNumber(ja)) aligned = ja->valueint ? 1 : 0;
    }

    gg_pointer_hit_t *hits = malloc(cap * sizeof(gg_pointer_hit_t));
    if (!hits) { send_err(fd, GGCMD_POINTER_SCAN, seq, GGERR_INTERNAL, "oom"); return; }

    /* addr：单目标模式。不传则扫"当前结果集的每个地址"（旧行为）。
     *
     * 为什么需要：结果集里通常有几百上千个地址（一次常见值搜索就有几千条），
     * 拿它当目标列表时，lookback 会把"指向任何一个目标附近"的指针全算进来，
     * 目标一多就彻底稀释 —— 想判断"到底有没有指针指向这个对象"时根本看不清。
     * CE / ptrscan 都是直接对单个地址做扫描的。 */
    uint64_t single = 0;
    const uint64_t *tg = g_eng.results.addrs;
    size_t nt = g_eng.results.count;
    cJSON *jaddr = cJSON_GetObjectItem(p, "addr");
    if (jaddr) {
        if (!j_addr_u64(jaddr, &single)) {
            free(hits);
            send_err(fd, GGCMD_POINTER_SCAN, seq, GGERR_BAD_REQUEST,
                     "addr must be a non-negative integer or hex string");
            return;
        }
        tg = &single;
        nt = 1;
    }
    if (nt == 0) {
        free(hits);
        send_err(fd, GGCMD_POINTER_SCAN, seq, GGERR_BAD_REQUEST,
                 "no targets: result set is empty and addr not given");
        return;
    }
    size_t n = pointer_scan(&g_eng.drv, g_eng.session_pid,
                            tg, nt, type,
                            lookback, aligned, hits, cap);
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "hits");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        char c[32], t[32];
        snprintf(c, sizeof(c), "0x%llx", (unsigned long long)hits[i].container);
        snprintf(t, sizeof(t), "0x%llx", (unsigned long long)hits[i].target);
        cJSON_AddStringToObject(o, "container", c);
        cJSON_AddStringToObject(o, "target", t);
        cJSON_AddNumberToObject(o, "ptype", hits[i].ptype);
        cJSON_AddItemToArray(arr, o);
    }
    free(hits);
    send_ok(fd, GGCMD_POINTER_SCAN, seq, r);
    cJSON_Delete(r);
}

/* watch_hit_json: 把一次硬件断点命中转成 JSON
 * 之前同时被 event_watch_hit（事件广播）和 h_watch poll 调用；
 * 事件系统删除后只剩 poll 一条消费路径，移到 watch 域更合理。 */
cJSON *watch_hit_json(const gg_watch_hit_t *h) {
    cJSON *o = cJSON_CreateObject();
    char tmp[32];
    cJSON_AddNumberToObject(o, "tid", h->tid);
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->pc);
    cJSON_AddStringToObject(o, "pc", tmp);
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->addr);
    cJSON_AddStringToObject(o, "addr", tmp);
    char rx[5][32];
    for (int k = 0; k < 5; k++) snprintf(rx[k], sizeof(rx[k]), "0x%llx",
        (unsigned long long)(k == 0 ? h->r0 : k == 1 ? h->r1 :
         k == 2 ? h->r2 : k == 3 ? h->r3 : h->r4));
    cJSON_AddStringToObject(o, "r0", rx[0]);
    cJSON_AddStringToObject(o, "r1", rx[1]);
    cJSON_AddStringToObject(o, "r2", rx[2]);
    cJSON_AddStringToObject(o, "r3", rx[3]);
    cJSON_AddStringToObject(o, "r4", rx[4]);
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->fp);
    cJSON_AddStringToObject(o, "fp", tmp);
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->sp);
    cJSON_AddStringToObject(o, "sp", tmp);
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->lr);
    cJSON_AddStringToObject(o, "lr", tmp);
    if (h->module[0]) {
        cJSON_AddStringToObject(o, "module", h->module);
        cJSON_AddStringToObject(o, "module_path", h->module_path);
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->module_base);
        cJSON_AddStringToObject(o, "module_base", tmp);
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->module_offset);
        cJSON_AddStringToObject(o, "module_offset", tmp);
    }
    cJSON_AddNumberToObject(o, "stack_depth", h->stack_depth);
    cJSON *stk = cJSON_AddArrayToObject(o, "stack");
    for (int s = 0; s < h->stack_depth; s++) {
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->stack_pc[s]);
        cJSON_AddItemToArray(stk, cJSON_CreateString(tmp));
    }
    cJSON_AddNumberToObject(o, "disasm_count", h->disasm_count);
    cJSON *dis = cJSON_AddArrayToObject(o, "disasm");
    for (int k = 0; k < h->disasm_count; k++) {
        cJSON *di = cJSON_CreateObject();
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->disasm[k].addr);
        cJSON_AddStringToObject(di, "addr", tmp);
        cJSON_AddStringToObject(di, "bytes", h->disasm[k].bytes);
        cJSON_AddStringToObject(di, "mnemonic", h->disasm[k].mnemonic);
        cJSON_AddStringToObject(di, "op_str", h->disasm[k].op_str);
        cJSON_AddStringToObject(di, "comment", h->disasm[k].comment);
        cJSON_AddItemToArray(dis, di);
    }
    return o;
}

/* ---------- 签名 → 全局变量地址（GGCMD_AOB_RESOLVE） ----------
 *
 * 为什么把三步合成一条命令：手工做一遍要来回 5 轮 ——
 *   1) gg_search 扫签名    2) 记下命中地址
 *   3) gg_disassemble 读命中点之前的字节   4) 人肉看 ADRP/ADD
 *   5) 心算 page+imm，再读那个地址验证
 * AI 每轮都要重新组织上下文，实测一次定位要好几分钟。
 * 合成后一次调用拿到"命中地址 + 指令证据 + 目标地址 + 解引用值"。
 *
 * 只解两种真实存在的寻址套路（编译器访问全局变量就这两条）：
 *   ADRP xN, page ; ADD xM, xN, #off    → target = page + off   （取址）
 *   ADRP xN, page ; LDR xM, [xN, #off]  → target = page + off   （取指针内容）
 * 后者 is_load=1，deref 时把 target 处的 8 字节读出来 —— 那才是全局变量本身。
 */

/* 把 Capstone 的 ops 串按逗号切开（方括号内的逗号不算分隔符） */
static int split_operands(const char *ops, char parts[][48], int max) {
    int n = 0, depth = 0;
    size_t len = 0;
    if (!ops || max <= 0) return 0;
    memset(parts[0], 0, 48);
    for (const char *s = ops; *s && n < max; s++) {
        if (*s == '[') depth++;
        else if (*s == ']') depth--;
        if (*s == ',' && depth == 0) {
            parts[n][len] = 0;
            n++; len = 0;
            if (n < max) memset(parts[n], 0, 48);
            while (s[1] == ' ') s++;
            continue;
        }
        if (*s != ' ' && len < 47) parts[n][len++] = *s;
    }
    if (len && n < max) { parts[n][len] = 0; n++; }
    return n;
}

/* "x8" -> 8；不是 x 寄存器返回 -1 */
static int reg_num(const char *s) {
    if (!s || s[0] != 'x') return -1;
    int v = (int)strtol(s + 1, NULL, 10);
    return (v >= 0 && v <= 30) ? v : -1;
}

/* "#0x940" / "#2368" -> 值 */
static int parse_imm(const char *s, uint64_t *out) {
    if (!s || s[0] != '#') return 0;
    *out = strtoull(s + 1, NULL, 0);
    return 1;
}

/* "[x0, #0x28]" -> (reg=0, disp=0x28)；也接受 "[x0]" 形态 */
static int parse_mem_operand(const char *s, int *reg, uint64_t *disp) {
    if (!s || s[0] != '[') return 0;
    s++;
    if (s[0] != 'x') return 0;
    char *endp = NULL;
    long r = strtol(s + 1, &endp, 10);
    if (!endp || r < 0 || r > 30) return 0;
    *reg = (int)r;
    s = endp;
    *disp = 0;
    while (*s == ' ' || *s == ',') s++;
    if (*s == '#') *disp = strtoull(s + 1, NULL, 0);
    return 1;
}

void h_aob_resolve(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) {
        send_err(fd, GGCMD_AOB_RESOLVE, seq, GGERR_NO_SESSION, "no session");
        return;
    }
    const char *ps = cJSON_GetObjectItem(p, "pattern") ?
        cJSON_GetObjectItem(p, "pattern")->valuestring : NULL;
    if (!ps) { send_err(fd, GGCMD_AOB_RESOLVE, seq, GGERR_BAD_REQUEST, "pattern required"); return; }

    int back = cJSON_GetObjectItem(p, "back") ? cJSON_GetObjectItem(p, "back")->valueint : 32;
    int max_hits = cJSON_GetObjectItem(p, "count") ? cJSON_GetObjectItem(p, "count")->valueint : 8;
    int do_deref = 1;
    cJSON *jd = cJSON_GetObjectItem(p, "deref");
    if (jd) do_deref = cJSON_IsBool(jd) ? (cJSON_IsTrue(jd) ? 1 : 0) : (jd->valueint != 0);
    if (back < 0) back = 0;
    if (back > 256) back = 256;
    if (max_hits <= 0) max_hits = 1;
    if (max_hits > 32) max_hits = 32;

    uint8_t pat[256], mask[256];
    int plen = parse_hex_pattern_masked(ps, pat, mask, sizeof(pat));
    if (plen <= 0) { send_err(fd, GGCMD_AOB_RESOLVE, seq, GGERR_BAD_REQUEST, "bad pattern"); return; }

    gg_results_t *r = &g_eng.results;
    int64_t found = search_aob(&g_eng.drv, g_eng.session_pid, r, pat, mask, (size_t)plen, REGION_WRITABLE);
    if (found < 0) { send_err(fd, GGCMD_AOB_RESOLVE, seq, GGERR_INTERNAL, "aob search failed"); return; }
    /* 签名通常在代码段（r-x），可写区扫不到 —— 这时必须扫 REGION_ALL。
     * 注意不能用 REGION_ANONYMOUS：那只是匿名映射（堆/栈），不含 .so 的代码段。 */
    if (found == 0) {
        found = search_aob(&g_eng.drv, g_eng.session_pid, r, pat, mask, (size_t)plen, REGION_ALL);
        if (found < 0) found = 0;
    }

    cJSON *out = cJSON_CreateObject();
    cJSON *hits = cJSON_AddArrayToObject(out, "hits");
    cJSON_AddNumberToObject(out, "scanned_hits", (double)found);

    size_t take = (size_t)found < (size_t)max_hits ? (size_t)found : (size_t)max_hits;
    for (size_t h = 0; h < take; h++) {
        uint64_t hit = r->addrs[h];
        uint64_t start = (hit >= (uint64_t)back) ? hit - (uint64_t)back : hit;
        start &= ~(uint64_t)3;                  /* adrp 必须落在 4 字节边界 */
        size_t want = (size_t)back + 64;
        uint8_t *code = malloc(want);
        if (!code) continue;
        ssize_t got = driver_read(&g_eng.drv, g_eng.session_pid, start, code, want);
        if (got <= 0) { free(code); continue; }

        char mnem[64][64], ops[64][128], byt[64][32], cmt[64][64];
        int n = disasm_a64(code, (size_t)got, start, mnem, ops, byt, cmt, 64);
        free(code);
        if (n <= 0) continue;

        cJSON *o = cJSON_CreateObject();
        char buf[48];
        snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)hit);
        cJSON_AddStringToObject(o, "addr", buf);
        cJSON *refs = cJSON_AddArrayToObject(o, "refs");

        uint64_t adrp_val[31];
        uint64_t adrp_pc[31];
        int adrp_set[31];
        memset(adrp_set, 0, sizeof(adrp_set));

        for (int i = 0; i < n; i++) {
            char parts[4][48];
            int np = split_operands(ops[i], parts, 4);
            if (np < 1) continue;

            if (strcmp(mnem[i], "adrp") == 0 && np >= 2) {
                int rd = reg_num(parts[0]);
                uint64_t page = 0;
                if (rd >= 0 && parse_imm(parts[1], &page)) {
                    adrp_val[rd] = page;
                    adrp_set[rd] = 1;
                    adrp_pc[rd] = start + (uint64_t)i * 4;
                }
                continue;
            }

            int is_load = 0;
            int base_reg = -1;
            uint64_t off = 0;
            if ((strcmp(mnem[i], "add") == 0 || strcmp(mnem[i], "sub") == 0) && np >= 3) {
                base_reg = reg_num(parts[1]);
                parse_imm(parts[2], &off);
            } else if (strncmp(mnem[i], "ldr", 3) == 0 && np >= 2) {
                if (parse_mem_operand(parts[1], &base_reg, &off)) is_load = 1;
            } else {
                continue;
            }
            if (base_reg < 0 || !adrp_set[base_reg]) continue;

            uint64_t target = adrp_val[base_reg];
            target = (strcmp(mnem[i], "sub") == 0) ? (target - off) : (target + off);

            cJSON *ref = cJSON_CreateObject();
            snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(start + (uint64_t)i * 4));
            cJSON_AddStringToObject(ref, "insn", buf);
            snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)target);
            cJSON_AddStringToObject(ref, "target", buf);
            cJSON_AddBoolToObject(ref, "is_load", is_load);
            snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)adrp_pc[base_reg]);
            cJSON_AddStringToObject(ref, "adrp_at", buf);
            cJSON_AddStringToObject(ref, "text", ops[i]);

            if (do_deref) {
                uint64_t val = 0;
                ssize_t vn = driver_read(&g_eng.drv, g_eng.session_pid, target, &val, 8);
                if (vn == 8 && val != 0) {
                    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)val);
                    cJSON_AddStringToObject(ref, "value", buf);
                }
            }
            cJSON_AddItemToArray(refs, ref);
        }
        cJSON_AddItemToArray(hits, o);
    }
    send_ok(fd, GGCMD_AOB_RESOLVE, seq, out);
    cJSON_Delete(out);
}
