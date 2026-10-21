/* cmd_view.c — 内存查看 / 符号解析 / 多级指针追踪（自 main.c 拆出，2026-08-28）
 *
 * 域内三工具：
 *   gg_view_memory   — 结构化内存查看（按 etype 解码 + region 注解）
 *   gg_resolve_symbol — 地址→符号静态解析（elfsym 本地层，无网络）
 *   gg_pointer_chain — 多级指针追踪（逐级读 + 越界/fault 容错）
 * 依赖：driver_read/process_maps（agentd 核心层）、elfsym（本地符号表）。
 * 引擎耦合仅 g_eng.drv / g_eng.session_pid，经 engine_internal.h 消费。
 */
#include "engine_internal.h"
#include "elfsym.h"
#include "process.h"

#include <string.h>
#include <stdlib.h>
/* ---------------- 结构化内存查看（gg_view_memory） ---------------- */

/* 元素解释类型: 1=int32 2=float32 3=pointer(arm64, 8B) */
static int parse_view_etype(const char *s) {
    if (!s || !*s) return 1;
    if (strcmp(s, "int32") == 0 || strcmp(s, "i32") == 0 || strcmp(s, "int") == 0) return 1;
    if (strcmp(s, "float32") == 0 || strcmp(s, "f32") == 0 || strcmp(s, "float") == 0) return 2;
    if (strcmp(s, "pointer") == 0 || strcmp(s, "ptr") == 0) return 3;
    return -1;
}

/* /proc/<pid>/maps 按地址升序 → 二分查找 addr 所在映射 */
static const gg_map_region_t *map_find(const gg_map_region_t *maps, size_t n, uint64_t addr) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (maps[mid].start <= addr) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return NULL;
    const gg_map_region_t *r = &maps[lo - 1];
    return (addr >= r->start && addr < r->end) ? r : NULL;
}

void h_view_memory(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_VIEW_MEMORY, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (!as) { send_err(fd, GGCMD_VIEW_MEMORY, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    const char *ets = cJSON_GetObjectItem(p, "element_type") ?
        cJSON_GetObjectItem(p, "element_type")->valuestring : "int32";
    int etype = parse_view_etype(ets);
    if (etype < 0) {
        send_err(fd, GGCMD_VIEW_MEMORY, seq, GGERR_BAD_REQUEST,
                 "element_type must be int32|float32|pointer");
        return;
    }
    long long range = cJSON_GetObjectItem(p, "range") ?
        (long long)cJSON_GetObjectItem(p, "range")->valuedouble : 8;
    if (range < 1) range = 1;
    if (range > 512) range = 512;

    int esz = (etype == 3) ? 8 : 4;
    uint64_t addr = strtoull(as, NULL, 0);
    size_t nbytes = (size_t)range * (size_t)esz;
    uint8_t *buf = malloc(nbytes);
    if (!buf) { send_err(fd, GGCMD_VIEW_MEMORY, seq, GGERR_INTERNAL, "oom"); return; }
    ssize_t got = driver_read_ex(&g_eng.drv, g_eng.session_pid, addr, buf, nbytes,
                                 parse_force(p));

    /* 一次取 maps：顶层标注基址所在区域，pointer 元素标注目标是否映射/所在模块 */
    size_t nreg = 0;
    gg_map_region_t *maps = process_maps(g_eng.session_pid, &nreg);

    cJSON *r = cJSON_CreateObject();
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)addr);
    cJSON_AddStringToObject(r, "addr", tmp);
    cJSON_AddStringToObject(r, "element_type",
                            etype == 1 ? "int32" : (etype == 2 ? "float32" : "pointer"));
    cJSON_AddNumberToObject(r, "element_size", esz);
    cJSON_AddNumberToObject(r, "range", (double)range);
    cJSON_AddBoolToObject(r, "base_mapped", map_find(maps, nreg, addr) ? 1 : 0);

    if (got < 0) {
        cJSON_AddNumberToObject(r, "count", 0);
        cJSON_AddNumberToObject(r, "bytes_read", 0);
        cJSON_AddStringToObject(r, "error", g_eng.drv.last_error);
    } else {
        size_t count = (size_t)got / (size_t)esz;   /* 只解析完整元素，尾部残缺丢弃 */
        cJSON_AddNumberToObject(r, "count", (double)count);
        cJSON_AddNumberToObject(r, "bytes_read", (double)got);
        cJSON *els = cJSON_AddArrayToObject(r, "elements");
        for (size_t i = 0; i < count; i++) {
            const uint8_t *ep = buf + i * (size_t)esz;
            uint64_t eaddr = addr + i * (size_t)esz;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "index", (double)i);
            cJSON_AddNumberToObject(o, "offset", (double)(i * (size_t)esz));
            snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)eaddr);
            cJSON_AddStringToObject(o, "addr", tmp);
            if (etype == 1) {          /* int32: 有符号十进制 + 原始位 */
                int32_t v;
                memcpy(&v, ep, 4);
                cJSON_AddNumberToObject(o, "value", (double)v);
                snprintf(tmp, sizeof(tmp), "0x%08x", (unsigned)(uint32_t)v);
                cJSON_AddStringToObject(o, "hex", tmp);
            } else if (etype == 2) {   /* float32: 浮点 + 原始位 */
                float f;
                uint32_t bits;
                memcpy(&f, ep, 4);
                memcpy(&bits, ep, 4);
                cJSON_AddNumberToObject(o, "value", (double)f);
                snprintf(tmp, sizeof(tmp), "0x%08x", (unsigned)bits);
                cJSON_AddStringToObject(o, "hex", tmp);
            } else {                   /* pointer (arm64, 8B): 指针值 + 映射标注 */
                uint64_t ptr;
                memcpy(&ptr, ep, 8);
                snprintf(tmp, sizeof(tmp), "0x%016llx", (unsigned long long)ptr);
                cJSON_AddStringToObject(o, "pointer", tmp);
                const gg_map_region_t *tr = map_find(maps, nreg, ptr);
                cJSON_AddBoolToObject(o, "mapped", tr ? 1 : 0);
                cJSON_AddStringToObject(o, "region",
                                        tr ? (tr->path[0] ? tr->path : "(anonymous)") : "");
                if (tr) cJSON_AddStringToObject(o, "region_perms", tr->perms);
            }
            cJSON_AddItemToArray(els, o);
        }
    }
    free(buf);
    free(maps);
    send_ok(fd, GGCMD_VIEW_MEMORY, seq, r);
    cJSON_Delete(r);
}

/* ---------------- 地址解析（gg_resolve_symbol 本地静态层） ---------------- */

static const char *path_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* 同路径映射的最低 start = 模块加载基址（.so 多段映射的首段） */
static uint64_t map_load_base(const gg_map_region_t *maps, size_t n,
                              const gg_map_region_t *r) {
    if (!r->path[0]) return r->start;
    uint64_t lo = r->start;
    for (size_t i = 0; i < n; i++)
        if (maps[i].path[0] && strcmp(maps[i].path, r->path) == 0 && maps[i].start < lo)
            lo = maps[i].start;
    return lo;
}

void h_resolve_symbol(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_RESOLVE_SYMBOL, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (!as) { send_err(fd, GGCMD_RESOLVE_SYMBOL, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    uint64_t addr = strtoull(as, NULL, 0);

    size_t nreg = 0;
    gg_map_region_t *maps = process_maps(g_eng.session_pid, &nreg);

    cJSON *r = cJSON_CreateObject();
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)addr);
    cJSON_AddStringToObject(r, "addr", tmp);

    const gg_map_region_t *rg = map_find(maps, nreg, addr);
    if (!rg) {
        cJSON_AddBoolToObject(r, "mapped", 0);
    } else {
        cJSON_AddBoolToObject(r, "mapped", 1);
        cJSON_AddStringToObject(r, "region", rg->path[0] ? rg->path : "");
        cJSON_AddStringToObject(r, "perms", rg->perms);
        if (rg->path[0]) {
            uint64_t base = map_load_base(maps, nreg, rg);
            uint64_t off = addr - base;
            cJSON_AddStringToObject(r, "module", path_basename(rg->path));
            snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)base);
            cJSON_AddStringToObject(r, "module_base", tmp);
            snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)off);
            cJSON_AddStringToObject(r, "offset_in_module", tmp);
            /* 纯静态层：ELF .dynsym/.symtab 最近导出符号（无 IDA 也能拿到函数名） */
            char sym[256];
            uint64_t sval = 0, sdelta = 0;
            int skind = 0;
            int rc = elfsym_nearest(rg->path, off, sym, sizeof(sym), &sval, &sdelta, &skind);
            if (rc == 1 && sym[0]) {
                cJSON_AddStringToObject(r, "symbol", sym);
                cJSON_AddNumberToObject(r, "symbol_delta", (double)sdelta);
                cJSON_AddStringToObject(r, "symbol_kind",
                                        skind == ELFSYM_FUNC ? "func" :
                                        (skind == ELFSYM_OBJECT ? "object" : "notype"));
            } else if (rc == -1) {
                cJSON_AddStringToObject(r, "elf_error", "unreadable or not ELF64");
            }
        }
    }
    free(maps);
    send_ok(fd, GGCMD_RESOLVE_SYMBOL, seq, r);
    cJSON_Delete(r);
}

/* ---------------- 多级指针追踪（gg_pointer_chain） ---------------- */

#define GG_PC_MAX_DEPTH 32

void h_pointer_chain(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_POINTER_CHAIN, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (!as) { send_err(fd, GGCMD_POINTER_CHAIN, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    long long max_depth = cJSON_GetObjectItem(p, "max_depth") ?
        (long long)cJSON_GetObjectItem(p, "max_depth")->valuedouble : 8;
    if (max_depth < 1) max_depth = 1;
    if (max_depth > GG_PC_MAX_DEPTH) max_depth = GG_PC_MAX_DEPTH;

    uint64_t addr = strtoull(as, NULL, 0);
    size_t nreg = 0;
    gg_map_region_t *maps = process_maps(g_eng.session_pid, &nreg);

    cJSON *r = cJSON_CreateObject();
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)addr);
    cJSON_AddStringToObject(r, "start", tmp);
    cJSON_AddNumberToObject(r, "max_depth", (double)max_depth);
    cJSON *levels = cJSON_AddArrayToObject(r, "levels");

    /* 记录每级读到的指针值，防环 */
    uint64_t visited[GG_PC_MAX_DEPTH];
    int nvisited = 0;
    const char *stopped = "ok";
    uint64_t cur = addr;

    /* 最后一个有效映射指针（链的"基址"） */
    int have_base = 0;
    uint64_t base_addr = 0, base_ptr = 0;
    const gg_map_region_t *base_rg = NULL;
    uint64_t base_depth = 0;

    for (int d = 0; d < (int)max_depth; d++) {
        uint8_t buf[8];
        ssize_t got = driver_read(&g_eng.drv, g_eng.session_pid, cur, buf, 8);
        if (got < 8) { stopped = "read_failed"; break; }
        uint64_t ptr;
        memcpy(&ptr, buf, 8);

        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "depth", d);
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)cur);
        cJSON_AddStringToObject(o, "addr", tmp);
        snprintf(tmp, sizeof(tmp), "0x%016llx", (unsigned long long)ptr);
        cJSON_AddStringToObject(o, "pointer", tmp);

        const gg_map_region_t *rg = map_find(maps, nreg, ptr);
        cJSON_AddBoolToObject(o, "mapped", rg ? 1 : 0);
        if (rg) {
            cJSON_AddStringToObject(o, "region", rg->path[0] ? rg->path : "(anonymous)");
            cJSON_AddStringToObject(o, "perms", rg->perms);
            cJSON_AddBoolToObject(o, "at_base", rg->start == ptr ? 1 : 0);
            if (rg->path[0]) {
                uint64_t lb = map_load_base(maps, nreg, rg);
                cJSON_AddStringToObject(o, "module", path_basename(rg->path));
                snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)(ptr - lb));
                cJSON_AddStringToObject(o, "module_offset", tmp);
            } else {
                cJSON_AddStringToObject(o, "module", "");
                cJSON_AddStringToObject(o, "module_offset", "");
            }
        } else {
            cJSON_AddStringToObject(o, "region", "");
            cJSON_AddStringToObject(o, "perms", "");
            cJSON_AddStringToObject(o, "module", "");
            cJSON_AddStringToObject(o, "module_offset", "");
            cJSON_AddBoolToObject(o, "at_base", 0);
        }
        cJSON_AddItemToArray(levels, o);

        if (ptr == 0) { stopped = "null_pointer"; break; }
        if (!rg) { stopped = "unmapped"; break; }

        /* 环检测 */
        int dup = 0;
        for (int i = 0; i < nvisited; i++) if (visited[i] == ptr) { dup = 1; break; }
        if (dup) { stopped = "cycle"; break; }
        visited[nvisited++] = ptr;

        /* 记录链基址（本级指针是有效映射） */
        have_base = 1;
        base_addr = cur;
        base_ptr = ptr;
        base_rg = rg;
        base_depth = (uint64_t)d;

        cur = ptr;
        if (d == (int)max_depth - 1) stopped = "max_depth";
    }

    cJSON_AddStringToObject(r, "stopped", stopped);
    if (have_base && base_rg) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "depth", (double)base_depth);
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)base_addr);
        cJSON_AddStringToObject(b, "addr", tmp);
        snprintf(tmp, sizeof(tmp), "0x%016llx", (unsigned long long)base_ptr);
        cJSON_AddStringToObject(b, "pointer", tmp);
        cJSON_AddStringToObject(b, "region", base_rg->path[0] ? base_rg->path : "(anonymous)");
        cJSON_AddStringToObject(b, "perms", base_rg->perms);
        if (base_rg->path[0]) {
            uint64_t lb = map_load_base(maps, nreg, base_rg);
            cJSON_AddStringToObject(b, "module", path_basename(base_rg->path));
            snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)(base_ptr - lb));
            cJSON_AddStringToObject(b, "module_offset", tmp);
        }
        cJSON_AddItemToObject(r, "base", b);
    }
    free(maps);
    send_ok(fd, GGCMD_POINTER_CHAIN, seq, r);
    cJSON_Delete(r);
}

