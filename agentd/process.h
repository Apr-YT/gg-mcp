/* process.h — /proc 进程枚举、maps 解析、模块列表 */
#ifndef GG_AGENTD_PROCESS_H
#define GG_AGENTD_PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GG_PROC_NAME_MAX  256
#define GG_MOD_NAME_MAX   256

typedef struct gg_proc_entry {
    pid_t pid;
    char  name[GG_PROC_NAME_MAX];   /* cmdline 拼接（截断） */
    char  comm[64];
    int   uid;
} gg_proc_entry_t;

/* 返回 malloc 数组 + 计数；调用方 free()。 */
gg_proc_entry_t *process_list(size_t *out_count, const char *filter);

typedef struct gg_map_region {
    uint64_t start, end;
    char     perms[5];        /* rwxp */
    char     path[GG_MOD_NAME_MAX];  /* 匿名为 "" */
} gg_map_region_t;

/* 解析 /proc/<pid>/maps，返回 malloc 数组 + 计数（会话级缓存副本，TTL 5s）。
 * 调用方负责 free。 */
gg_map_region_t *process_maps(pid_t pid, size_t *out_count);

/* 失效 maps 会话缓存（attach/detach 时调用，防 pid 复用串会话）。 */
void maps_cache_invalidate(void);

/* 解析一行 /proc/<pid>/maps 文本到 region。成功返回 1 并填充 r
 * （匿名映射/特殊段 [anon][heap][stack] 等 path 置空，region_matches 依赖空 path 判 anonymous）；
 * 畸形行返回 0。单一事实源，process_maps 与单元测试共用。 */
int parse_maps_line(const char *line, gg_map_region_t *r);

/* 可搜索区域：根据 region 类型过滤，返回 malloc 数组（含 end 修正对齐）。 */
gg_map_region_t *process_search_regions(pid_t pid, size_t *out_count, int region_type);

/* region 类型过滤（REGION_* 见 wire_protocol.h；REGION_ALL 排除 ---p 不可读段）。
 * 返回 1=匹配可搜索, 0=跳过。单一事实源，process.c 与单元测试共用。 */
int region_matches(const gg_map_region_t *r, int region_type);

/* 模块列表（路径+基址+大小），返回 malloc 数组 + 计数。 */
typedef struct gg_module {
    char     path[GG_MOD_NAME_MAX];
    uint64_t base;
    uint64_t size;
} gg_module_t;
gg_module_t *process_modules(pid_t pid, size_t *out_count);

/* 地址归属解析：找包含 addr 的映射段，返回其文件路径/模块基址/段内偏移。
 * 命中匿名区域返回 -1（path 置空）。成功返回 0。 */
int process_resolve_addr(pid_t pid, uint64_t addr,
                         char *path, size_t pathsz, uint64_t *module_base,
                         uint64_t *offset);

int process_exists(pid_t pid);
int process_is_64bit(pid_t pid);   /* 1=64 位, 0=32 位, -1=未知 */
int process_uid(pid_t pid);        /* -1=未知 */

#ifdef __cplusplus
}
#endif

#endif /* GG_AGENTD_PROCESS_H */
