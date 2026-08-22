/* process.c — /proc 进程枚举、maps 解析、模块列表 */
#include "process.h"
#include "../shared/wire_protocol.h"   /* REGION_* */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#ifdef __MINGW32__
/* MinGW-w64 msvcrt 无 readlink 声明/符号：仅单测链接需要（test 提供 shim 实现）。
 * 产品目标（Android bionic / Linux glibc）由 <unistd.h> 正常声明，不受影响。 */
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
#endif

static int is_pid_dir(const char *name) {
    if (!*name) return 0;
    for (const char *p = name; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

static void read_cmdline(pid_t pid, char *out, size_t outsz) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    int fd = open(path, O_RDONLY);
    out[0] = 0;
    if (fd < 0) return;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    /* NUL 分隔 → 空格拼接 */
    size_t o = 0;
    for (ssize_t i = 0; i < n && o + 1 < outsz; i++) {
        char c = buf[i];
        if (c == 0) c = ' ';
        out[o++] = c;
    }
    while (o > 0 && out[o - 1] == ' ') o--;   /* 去尾空格 */
    out[o] = 0;
}

static void read_comm(pid_t pid, char *out, size_t outsz) {
    char path[64], buf[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    int fd = open(path, O_RDONLY);
    out[0] = 0;
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    snprintf(out, outsz, "%s", buf);
}

gg_proc_entry_t *process_list(size_t *out_count, const char *filter) {
    DIR *dp = opendir("/proc");
    if (!dp) { *out_count = 0; return NULL; }

    size_t cap = 64, n = 0;
    gg_proc_entry_t *arr = malloc(cap * sizeof(*arr));
    if (!arr) { closedir(dp); *out_count = 0; return NULL; }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (!is_pid_dir(de->d_name)) continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        gg_proc_entry_t e;
        memset(&e, 0, sizeof(e));
        e.pid = pid;
        read_cmdline(pid, e.name, sizeof(e.name));
        read_comm(pid, e.comm, sizeof(e.comm));
        e.uid = process_uid(pid);
        if (!e.name[0]) snprintf(e.name, sizeof(e.name), "[%s]", e.comm);
        if (filter && *filter && !strstr(e.name, filter) && !strstr(e.comm, filter)) continue;
        if (n == cap) {
            cap *= 2;
            gg_proc_entry_t *na = realloc(arr, cap * sizeof(*arr));
            if (!na) break;
            arr = na;
        }
        arr[n++] = e;
    }
    closedir(dp);
    *out_count = n;
    return arr;
}

int parse_maps_line(const char *line, gg_map_region_t *r) {
    unsigned long long s, e, off, inode;
    char perms[8], dev[16], p[GG_MOD_NAME_MAX] = "";
    int parsed = sscanf(line, "%llx-%llx %7s %llx %15s %llu %255s",
                        &s, &e, perms, &off, dev, &inode, p);
    if (parsed < 6) return 0;
    r->start = (uint64_t)s;
    r->end   = (uint64_t)e;
    /* perms 字段仅 4+null（rwxp），sscanf %7s 理论上可超——显式截断防编译器告警 */
    snprintf(r->perms, sizeof(r->perms), "%.*s",
             (int)(sizeof(r->perms) - 1), perms);
    /* 匿名映射/特殊段([anon][heap][stack] 等)统一置空 path：
     * region_matches 依赖 path[0]==0 判 anonymous；显式 [anon] 等无 " /" 前缀也会清空。 */
    p[0] = 0;
    char *sp = strstr(line, " /");   /* 路径可能含空格(如 "... base.apk") — sscanf %s 只取首段, 手动补全 */
    if (sp) {
        sp++;
        /* 注意: 不得修改 line（调用方可传只读字符串字面量）——用长度而非 *nl=0 截断 */
        char *nl = strchr(sp, '\n');
        size_t plen = nl ? (size_t)(nl - sp) : strlen(sp);
        while (*sp == ' ') { sp++; if (plen) plen--; }
        if (plen >= sizeof(p)) plen = sizeof(p) - 1;
        snprintf(p, sizeof(p), "%.*s", (int)plen, sp);
    }
    snprintf(r->path, sizeof(r->path), "%s", p);
    return 1;
}

/* ---- maps 会话级缓存 ----
 * 高頻 /proc/<pid>/maps 读取是行为特征（每次 handler 都 fopen 全量解析）。
 * 缓存当前 attach 会话的 maps，TTL 5s；attach/detach 时 maps_cache_invalidate 强制失效。
 * 返回副本（调用方 free 不影响缓存）。 */
#define MAPS_CACHE_TTL_SEC 5
static pid_t      g_maps_pid = -1;
static gg_map_region_t *g_maps_cache = NULL;
static size_t     g_maps_n = 0;
static time_t     g_maps_ts = 0;
static pthread_mutex_t g_maps_mtx = PTHREAD_MUTEX_INITIALIZER;

/* 内部版: 调用方须持 g_maps_mtx 锁（process_maps 持锁时复用, 避免递归死锁） */
static void maps_cache_invalidate_locked(void) {
    free(g_maps_cache);
    g_maps_cache = NULL;
    g_maps_n = 0;
    g_maps_pid = -1;
    g_maps_ts = 0;
}

void maps_cache_invalidate(void) {
    pthread_mutex_lock(&g_maps_mtx);
    maps_cache_invalidate_locked();
    pthread_mutex_unlock(&g_maps_mtx);
}

static gg_map_region_t *maps_dup_cached(size_t *out_count) {
    gg_map_region_t *dup = malloc(g_maps_n * sizeof(*dup));
    if (!dup) { *out_count = 0; return NULL; }
    memcpy(dup, g_maps_cache, g_maps_n * sizeof(*dup));
    *out_count = g_maps_n;
    return dup;
}

gg_map_region_t *process_maps(pid_t pid, size_t *out_count) {
    pthread_mutex_lock(&g_maps_mtx);
    /* 命中缓存（同 pid + TTL 内）→ 返回副本 */
    if (g_maps_pid == pid && g_maps_cache &&
        (time(NULL) - g_maps_ts) < MAPS_CACHE_TTL_SEC) {
        gg_map_region_t *d = maps_dup_cached(out_count);
        pthread_mutex_unlock(&g_maps_mtx);
        return d;
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { *out_count = 0; pthread_mutex_unlock(&g_maps_mtx); return NULL; }

    size_t cap = 256, n = 0;
    gg_map_region_t *arr = malloc(cap * sizeof(*arr));
    if (!arr) { fclose(f); *out_count = 0; pthread_mutex_unlock(&g_maps_mtx); return NULL; }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (n == cap) {
            cap *= 2;
            gg_map_region_t *na = realloc(arr, cap * sizeof(*arr));
            if (!na) break;
            arr = na;
        }
        if (parse_maps_line(line, &arr[n])) n++;
    }
    fclose(f);

    /* 写入缓存（持有 arr），返回副本 */
    maps_cache_invalidate_locked();
    g_maps_cache = arr;
    g_maps_n = n;
    g_maps_pid = pid;
    g_maps_ts = time(NULL);
    *out_count = n;
    gg_map_region_t *d = maps_dup_cached(out_count);
    pthread_mutex_unlock(&g_maps_mtx);
    return d;
}

int region_matches(const gg_map_region_t *r, int region_type) {
    switch (region_type) {
    case REGION_ANONYMOUS:
        return r->path[0] == 0 && strchr(r->perms, 'w') != NULL;
    case REGION_WRITABLE:
        return strchr(r->perms, 'w') != NULL;
    case REGION_ALL:
        /* 排除 PROT_NONE(---p/---s) 区域: 无读权限必然不可读, 搜索纯浪费
         * (zygote64 实测 12.9GB/17GB 是 ---p, 782 万次驱动读大部分花在它上面)。
         * 无路径的特殊段([vdso] 等 r-xp 可读) 保留。 */
        return r->perms[0] == 'r';
    default:
        return 1;
    }
}

gg_map_region_t *process_search_regions(pid_t pid, size_t *out_count, int region_type) {
    size_t all_n = 0;
    gg_map_region_t *all = process_maps(pid, &all_n);
    if (!all) { *out_count = 0; return NULL; }

    size_t cap = all_n ? all_n : 1, n = 0;
    gg_map_region_t *arr = malloc(cap * sizeof(*arr));
    if (!arr) { free(all); *out_count = 0; return NULL; }

    for (size_t i = 0; i < all_n; i++) {
        if (!region_matches(&all[i], region_type)) continue;
        uint64_t len = all[i].end - all[i].start;
        if (len < 4) continue;                     /* 太小跳过 */
        if (len > (64ull << 30)) continue;         /* 单区域上限 64GB 防御 */
        arr[n++] = all[i];
    }
    free(all);
    *out_count = n;
    return arr;
}

gg_module_t *process_modules(pid_t pid, size_t *out_count) {
    size_t n = 0;
    gg_map_region_t *maps = process_maps(pid, &n);
    if (!maps) { *out_count = 0; return NULL; }

    size_t cap = 64, m = 0;
    gg_module_t *mods = malloc(cap * sizeof(*mods));
    if (!mods) { free(maps); *out_count = 0; return NULL; }

    for (size_t i = 0; i < n; i++) {
        if (!maps[i].path[0]) continue;
        /* 找同路径最低 start 与最高 end */
        uint64_t lo = maps[i].start, hi = maps[i].end;
        for (size_t j = i + 1; j < n; j++) {
            if (strcmp(maps[j].path, maps[i].path) == 0) {
                if (maps[j].start < lo) lo = maps[j].start;
                if (maps[j].end > hi) hi = maps[j].end;
            }
        }
        /* 去重：若该路径已收录则跳过 */
        int dup = 0;
        for (size_t k = 0; k < m; k++) if (strcmp(mods[k].path, maps[i].path) == 0) { dup = 1; break; }
        if (dup) continue;
        if (m == cap) {
            cap *= 2;
            gg_module_t *na = realloc(mods, cap * sizeof(*na));
            if (!na) break;
            mods = na;
        }
        snprintf(mods[m].path, sizeof(mods[m].path), "%s", maps[i].path);
        mods[m].base = lo;
        mods[m].size = hi - lo;
        m++;
    }
    free(maps);
    *out_count = m;
    return mods;
}

int process_resolve_addr(pid_t pid, uint64_t addr,
                         char *path, size_t pathsz, uint64_t *module_base,
                         uint64_t *offset) {
    size_t n = 0;
    gg_map_region_t *maps = process_maps(pid, &n);
    if (!maps) return -1;
    const char *hit_path = NULL;
    for (size_t i = 0; i < n; i++) {
        if (addr >= maps[i].start && addr < maps[i].end) {
            hit_path = maps[i].path;
            break;
        }
    }
    if (!hit_path) { free(maps); return -1; }
    if (!hit_path[0]) { free(maps); return -1; }   /* 匿名区域 */

    /* 模块基址 = 同路径映射段的最低 start */
    uint64_t base = UINT64_MAX;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(maps[i].path, hit_path) == 0 && maps[i].start < base)
            base = maps[i].start;
    }
    /* ⚠️ hit_path 指向 maps 内部, 必须先拷贝再 free（否则 use-after-free:
     * 堆复用后 snprintf %s 读到垃圾地址 → strlen 段错误） */
    char hit_path_copy[GG_MOD_NAME_MAX];
    snprintf(hit_path_copy, sizeof(hit_path_copy), "%s", hit_path);
    free(maps);
    if (base == UINT64_MAX) return -1;
    snprintf(path, pathsz, "%s", hit_path_copy);
    *module_base = base;
    *offset = addr - base;
    return 0;
}

int process_exists(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", (int)pid);
    return access(path, F_OK) == 0;
}

int process_is_64bit(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);
    char buf[64];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        /* zygote 进程直接看 exe 名: app_process64 → 64 位, app_process32 → 32 位 */
        if (strstr(buf, "app_process64")) return 1;
        if (strstr(buf, "app_process32")) return 0;
        /* 原生进程（非 app_process）: 继续走 maps 扫描 */
    }
    /* 扫描全部 maps: 任一地址超过 8 位 hex → 64 位。
     * ⚠️ 不能只看首行: 64 位 ART 进程首行常是低地址 dalvik 匿名映射
     * （如 boot.art @ 0x02ddc000, 8 位 hex），会误判 32 位；
     * 64 位进程必有高位映射（linker/libc/二进制 @ 0x6x/0x7x）。 */
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int found = -1;   /* -1 未知, 0 全 8 位内(32 位), 1 发现 64 位地址 */
    while (fgets(line, sizeof(line), f)) {
        char *dash = strchr(line, '-');
        if (!dash) continue;
        size_t hlen = (size_t)(dash - line);
        if (hlen > 8) { found = 1; break; }   /* 9+ 位 hex → 64 位地址 */
        if (hlen == 8) found = 0;             /* 8 位内: 待定, 继续找高位 */
    }
    fclose(f);
    return found;
}

int process_uid(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int uid = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            uid = atoi(line + 4);
            break;
        }
    }
    fclose(f);
    return uid;
}
