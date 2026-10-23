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
            event_watch_hit(&hits[i]);   /* 实时推送给订阅端（手机悬浮窗） */
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
        event_watch_hit(&hit);
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

    gg_pointer_hit_t *hits = malloc(cap * sizeof(gg_pointer_hit_t));
    if (!hits) { send_err(fd, GGCMD_POINTER_SCAN, seq, GGERR_INTERNAL, "oom"); return; }
    size_t n = pointer_scan(&g_eng.drv, g_eng.session_pid,
                            g_eng.results.addrs, g_eng.results.count, type, hits, cap);
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

/* GG-TOUCH 内核虚拟触摸注入（rwproc33 驱动 CMD_GGTOUCH_TAP/SWIPE）。
 * 不依赖 attach（触摸是系统级输入, 与目标进程无关）。 */
