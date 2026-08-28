/**
 * perf.c - 性能监控实现
 */

#include "perf.h"
#include "error.h"
#include <stdio.h>
#include <string.h>

/* 全局性能统计 */
perf_global_t g_perf_stats;

void perf_init(void) {
    memset(&g_perf_stats, 0, sizeof(g_perf_stats));
    LOG_INFO("Performance monitoring initialized");
}

void perf_shutdown(void) {
    LOG_INFO("Performance monitoring shutdown");
    perf_print_stats();
}

void perf_print_stats(void) {
    LOG_INFO("=== Performance Statistics ===");
    perf_print_memory_stats();
    perf_print_io_stats();
    perf_print_search_stats();
    
    // 打印其他统计
    uint64_t freeze_count = atomic_load_64(&g_perf_stats.freeze_count);
    uint64_t watch_hits = atomic_load_64(&g_perf_stats.watch_hit_count);
    uint64_t watch_misses = atomic_load_64(&g_perf_stats.watch_miss_count);
    uint64_t errors = atomic_load_64(&g_perf_stats.error_count);
    uint64_t timeouts = atomic_load_64(&g_perf_stats.timeout_count);
    
    LOG_INFO("Freeze: active=%llu", (unsigned long long)freeze_count);
    LOG_INFO("Watch: hits=%llu, misses=%llu", 
             (unsigned long long)watch_hits, 
             (unsigned long long)watch_misses);
    LOG_INFO("Errors: %llu, Timeouts: %llu", 
             (unsigned long long)errors, 
             (unsigned long long)timeouts);
}

void perf_print_memory_stats(void) {
    uint64_t allocs = atomic_load_64(&g_perf_stats.total_allocs);
    uint64_t frees = atomic_load_64(&g_perf_stats.total_frees);
    uint64_t current = atomic_load_64(&g_perf_stats.current_allocated);
    uint64_t peak = atomic_load_64(&g_perf_stats.peak_allocated);
    
    LOG_INFO("Memory: allocs=%llu, frees=%llu, current=%llu bytes, peak=%llu bytes",
             (unsigned long long)allocs,
             (unsigned long long)frees,
             (unsigned long long)current,
             (unsigned long long)peak);
}

void perf_print_io_stats(void) {
    uint64_t bytes_read = atomic_load_64(&g_perf_stats.bytes_read);
    uint64_t bytes_written = atomic_load_64(&g_perf_stats.bytes_written);
    uint64_t read_count = atomic_load_64(&g_perf_stats.read_count);
    uint64_t write_count = atomic_load_64(&g_perf_stats.write_count);
    
    double read_avg = read_count > 0 ? (double)bytes_read / read_count : 0.0;
    double write_avg = write_count > 0 ? (double)bytes_written / write_count : 0.0;
    
    LOG_INFO("I/O: read=%llu bytes (%llu ops, %.2f avg), write=%llu bytes (%llu ops, %.2f avg)",
             (unsigned long long)bytes_read,
             (unsigned long long)read_count,
             read_avg,
             (unsigned long long)bytes_written,
             (unsigned long long)write_count,
             write_avg);
}

void perf_print_search_stats(void) {
    uint64_t search_count = atomic_load_64(&g_perf_stats.search_count);
    uint64_t bytes_scanned = atomic_load_64(&g_perf_stats.search_bytes_scanned);
    uint64_t results_found = atomic_load_64(&g_perf_stats.search_results_found);
    uint64_t search_time_ns = atomic_load_64(&g_perf_stats.search_time_ns);
    
    if (search_count > 0) {
        double avg_bytes = (double)bytes_scanned / search_count;
        double avg_time_us = (double)search_time_ns / search_count / 1000.0;
        double throughput_mbps = (double)bytes_scanned / (search_time_ns / 1000.0) / 1000.0;
        
        LOG_INFO("Search: count=%llu, scanned=%.2f MB, results=%llu, avg_time=%.2f us, throughput=%.2f MB/s",
                 (unsigned long long)search_count,
                 avg_bytes / 1024.0 / 1024.0,
                 (unsigned long long)results_found,
                 avg_time_us,
                 throughput_mbps);
    }
}

void perf_reset_stats(void) {
    memset(&g_perf_stats, 0, sizeof(g_perf_stats));
    LOG_INFO("Performance statistics reset");
}
