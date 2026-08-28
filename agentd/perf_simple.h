/**
 * perf_simple.h - 轻量级性能统计模块
 */

#ifndef GG_PERF_SIMPLE_H
#define GG_PERF_SIMPLE_H

#include <stdint.h>
#include <time.h>
#include "atomic_simple.h"

extern void log_msg(const char *fmt, ...);

typedef struct {
    const char *name;
    atomic_int64_t count;
    atomic_int64_t total_ns;
    atomic_int64_t min_ns;
    atomic_int64_t max_ns;
} perf_counter_t;

typedef struct {
    const char *name;
    uint64_t start_ns;
} perf_timer_t;

typedef struct {
    atomic_int64_t read_bytes;
    atomic_int64_t write_bytes;
    atomic_int64_t read_count;
    atomic_int64_t write_count;
    atomic_int64_t search_count;
    atomic_int64_t search_results;
} perf_global_t;

extern perf_global_t g_perf_stats;

static inline uint64_t perf_get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline void perf_counter_init(perf_counter_t *c, const char *name) {
    c->name = name;
    atomic_store64(&c->count, 0);
    atomic_store64(&c->total_ns, 0);
    atomic_store64(&c->min_ns, UINT64_MAX);
    atomic_store64(&c->max_ns, 0);
}

static inline void perf_counter_record(perf_counter_t *c, uint64_t elapsed_ns) {
    atomic_fetch_add64(&c->count, 1);
    atomic_fetch_add64(&c->total_ns, elapsed_ns);
    
    int64_t current_min = atomic_load64(&c->min_ns);
    if (elapsed_ns < (uint64_t)current_min) {
        atomic_compare_exchange64(&c->min_ns, &current_min, elapsed_ns);
    }
    
    int64_t current_max = atomic_load64(&c->max_ns);
    if (elapsed_ns > (uint64_t)current_max) {
        atomic_compare_exchange64(&c->max_ns, &current_max, elapsed_ns);
    }
}

static inline void perf_timer_start(perf_timer_t *t, const char *name) {
    t->name = name;
    t->start_ns = perf_get_time_ns();
}

static inline uint64_t perf_timer_stop(perf_timer_t *t) {
    uint64_t end_ns = perf_get_time_ns();
    return end_ns - t->start_ns;
}

#define PERF_DECLARE(name) static perf_counter_t perf_##name = { .name = #name }
#define PERF_INIT(name) perf_counter_init(&perf_##name, #name)
#define PERF_RECORD(name, ns) perf_counter_record(&perf_##name, ns)
#define PERF_SCOPE(name) perf_timer_t __perf_timer_##name; perf_timer_start(&__perf_timer_##name, #name)
#define PERF_SCOPE_END(name) do { \
    uint64_t __perf_elapsed = perf_timer_stop(&__perf_timer_##name); \
    PERF_RECORD(name, __perf_elapsed); \
} while(0)

#define PERF_READ_ADD(bytes) atomic_fetch_add64(&g_perf_stats.read_bytes, bytes)
#define PERF_READ_INC() atomic_fetch_add64(&g_perf_stats.read_count, 1)
#define PERF_WRITE_ADD(bytes) atomic_fetch_add64(&g_perf_stats.write_bytes, bytes)
#define PERF_WRITE_INC() atomic_fetch_add64(&g_perf_stats.write_count, 1)
#define PERF_SEARCH_INC() atomic_fetch_add64(&g_perf_stats.search_count, 1)
#define PERF_SEARCH_RESULTS(n) atomic_fetch_add64(&g_perf_stats.search_results, n)

static inline void perf_counter_print(perf_counter_t *c) {
    uint64_t count = atomic_load64(&c->count);
    uint64_t total_ns = atomic_load64(&c->total_ns);
    
    if (count > 0) {
        uint64_t avg_ns = total_ns / count;
        log_msg("[PERF] %s: count=%llu, total=%llu us, avg=%llu us",
                 c->name,
                 (unsigned long long)count,
                 (unsigned long long)(total_ns / 1000),
                 (unsigned long long)(avg_ns / 1000));
    }
}

static inline void perf_global_print(void) {
    uint64_t read_bytes = atomic_load64(&g_perf_stats.read_bytes);
    uint64_t write_bytes = atomic_load64(&g_perf_stats.write_bytes);
    uint64_t read_count = atomic_load64(&g_perf_stats.read_count);
    uint64_t write_count = atomic_load64(&g_perf_stats.write_count);
    uint64_t search_count = atomic_load64(&g_perf_stats.search_count);
    uint64_t search_results = atomic_load64(&g_perf_stats.search_results);
    
    log_msg("[PERF] GLOBAL:");
    log_msg("  read: %llu bytes, %llu ops", 
             (unsigned long long)read_bytes,
             (unsigned long long)read_count);
    log_msg("  write: %llu bytes, %llu ops",
             (unsigned long long)write_bytes,
             (unsigned long long)write_count);
    log_msg("  search: %llu ops, %llu results",
             (unsigned long long)search_count,
             (unsigned long long)search_results);
}

#endif /* GG_PERF_SIMPLE_H */
