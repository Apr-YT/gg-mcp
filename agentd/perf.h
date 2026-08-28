/**
 * perf.h - 性能监控和统计模块
 * 
 * 提供轻量级的性能指标采集和报告
 */

#ifndef AGENTD_PERF_H
#define AGENTD_PERF_H

#include <stdint.h>
#include <time.h>
#include "atomic.h"

/* 性能计数器 */
typedef struct {
    const char *name;
    atomic_stats_t stats;
    uint64_t start_time_ns;
    uint64_t last_update_ns;
} perf_counter_t;

/* 性能计时器 */
typedef struct {
    const char *name;
    uint64_t start_ns;
    uint64_t elapsed_ns;
    uint32_t call_count;
} perf_timer_t;

/* 全局性能指标 */
typedef struct {
    /* 内存统计 */
    atomic_uint64_t total_allocs;
    atomic_uint64_t total_frees;
    atomic_uint64_t current_allocated;
    atomic_uint64_t peak_allocated;
    
    /* I/O统计 */
    atomic_uint64_t bytes_read;
    atomic_uint64_t bytes_written;
    atomic_uint64_t read_count;
    atomic_uint64_t write_count;
    
    /* 搜索统计 */
    atomic_uint64_t search_count;
    atomic_uint64_t search_bytes_scanned;
    atomic_uint64_t search_results_found;
    atomic_uint64_t search_time_ns;
    
    /* 冻结统计 */
    atomic_uint64_t freeze_count;
    atomic_uint64_t freeze_active;
    
    /* 监控统计 */
    atomic_uint64_t watch_hit_count;
    atomic_uint64_t watch_miss_count;
    
    /* 错误统计 */
    atomic_uint64_t error_count;
    atomic_uint64_t timeout_count;
} perf_global_t;

extern perf_global_t g_perf_stats;

/* API函数 */

/**
 * 初始化性能监控
 */
void perf_init(void);

/**
 * 销毁性能监控（输出统计报告）
 */
void perf_shutdown(void);

/**
 * 初始化性能计数器
 */
void perf_counter_init(perf_counter_t *counter, const char *name);

/**
 * 增加计数器值
 */
static inline void perf_counter_inc(perf_counter_t *counter, uint64_t delta) {
    atomic_stats_inc_count(&counter->stats, delta);
    counter->last_update_ns = counter->start_time_ns + delta;
}

/**
 * 获取计数器值
 */
static inline uint64_t perf_counter_get(perf_counter_t *counter) {
    return atomic_stats_get_count(&counter->stats);
}

/**
 * 启动性能计时器
 */
static inline void perf_timer_start(perf_timer_t *timer) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    timer->start_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/**
 * 停止性能计时器
 */
static inline void perf_timer_stop(perf_timer_t *timer) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t end_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    timer->elapsed_ns = end_ns - timer->start_ns;
    timer->call_count++;
}

/**
 * 获取计时器耗时（纳秒）
 */
static inline uint64_t perf_timer_elapsed_ns(perf_timer_t *timer) {
    return timer->elapsed_ns;
}

/**
 * 获取计时器耗时（微秒）
 */
static inline uint64_t perf_timer_elapsed_us(perf_timer_t *timer) {
    return timer->elapsed_ns / 1000;
}

/**
 * 获取计时器耗时（毫秒）
 */
static inline uint64_t perf_timer_elapsed_ms(perf_timer_t *timer) {
    return timer->elapsed_ns / 1000000;
}

/**
 * 获取平均耗时（纳秒）
 */
static inline uint64_t perf_timer_avg_ns(perf_timer_t *timer) {
    return timer->call_count > 0 ? timer->elapsed_ns / timer->call_count : 0;
}

/* 全局统计宏 */

// 内存分配
#define PERF_ALLOC(size) do { \
    atomic_fetch_add_64(&g_perf_stats.total_allocs, 1); \
    atomic_fetch_add_64(&g_perf_stats.current_allocated, size); \
    uint64_t current = atomic_load_64(&g_perf_stats.current_allocated); \
    uint64_t peak = atomic_load_64(&g_perf_stats.peak_allocated); \
    if (current > peak) { \
        atomic_store_64((atomic_int64_t*)&g_perf_stats.peak_allocated, current); \
    } \
} while(0)

#define PERF_FREE(size) do { \
    atomic_fetch_add_64(&g_perf_stats.total_frees, 1); \
    atomic_fetch_sub_64(&g_perf_stats.current_allocated, size); \
} while(0)

// I/O操作
#define PERF_READ(bytes) do { \
    atomic_fetch_add_64(&g_perf_stats.bytes_read, bytes); \
    atomic_fetch_add_64(&g_perf_stats.read_count, 1); \
} while(0)

#define PERF_WRITE(bytes) do { \
    atomic_fetch_add_64(&g_perf_stats.bytes_written, bytes); \
    atomic_fetch_add_64(&g_perf_stats.write_count, 1); \
} while(0)

// 搜索操作
#define PERF_SEARCH_START() perf_timer_t __perf_search_timer = { .name = "search" }; perf_timer_start(&__perf_search_timer)
#define PERF_SEARCH_END(bytes, results) do { \
    perf_timer_stop(&__perf_search_timer); \
    atomic_fetch_add_64(&g_perf_stats.search_count, 1); \
    atomic_fetch_add_64(&g_perf_stats.search_bytes_scanned, bytes); \
    atomic_fetch_add_64(&g_perf_stats.search_results_found, results); \
    atomic_fetch_add_64(&g_perf_stats.search_time_ns, __perf_search_timer.elapsed_ns); \
} while(0)

// 冻结操作
#define PERF_FREEZE_ADD() atomic_fetch_add_64(&g_perf_stats.freeze_count, 1)
#define PERF_FREEZE_REMOVE() atomic_fetch_sub_64(&g_perf_stats.freeze_count, 1)

// 监控操作
#define PERF_WATCH_HIT() atomic_fetch_add_64(&g_perf_stats.watch_hit_count, 1)
#define PERF_WATCH_MISS() atomic_fetch_add_64(&g_perf_stats.watch_miss_count, 1)

// 错误和超时
#define PERF_ERROR() atomic_fetch_add_64(&g_perf_stats.error_count, 1)
#define PERF_TIMEOUT() atomic_fetch_add_64(&g_perf_stats.timeout_count, 1)

/* 性能报告 */

/**
 * 打印全局性能统计
 */
void perf_print_stats(void);

/**
 * 打印内存统计
 */
void perf_print_memory_stats(void);

/**
 * 打印I/O统计
 */
void perf_print_io_stats(void);

/**
 * 打印搜索统计
 */
void perf_print_search_stats(void);

/**
 * 重置所有统计
 */
void perf_reset_stats(void);

/* 自动作用域计时器 */
#define PERF_TIMER_SCOPE(name) \
    perf_timer_t __perf_scope_timer = { .name = (name) }; \
    perf_timer_start(&__perf_scope_timer); \
    /* 用户代码后需要调用 PERF_TIMER_SCOPE_END */
#define PERF_TIMER_SCOPE_END() \
    perf_timer_stop(&__perf_scope_timer); \
    if (__perf_scope_timer.elapsed_ns > 1000000) { \  /* 超过1ms才打印 */
        LOG_INFO("[PERF] %s: %llu us (calls: %u, avg: %llu us)", \
                 __perf_scope_timer.name, \
                 (unsigned long long)perf_timer_elapsed_us(&__perf_scope_timer), \
                 __perf_scope_timer.call_count, \
                 (unsigned long long)perf_timer_avg_ns(&__perf_scope_timer) / 1000); \
    }

#endif /* AGENTD_PERF_H */
