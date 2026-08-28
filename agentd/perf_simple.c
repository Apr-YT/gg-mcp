/**
 * perf_simple.c - 性能统计模块实现
 */

#include "perf_simple.h"
#include <string.h>

/* 全局统计 */
perf_global_t g_perf_stats;

/* 初始化全局统计 */
void perf_global_init(void) {
    memset(&g_perf_stats, 0, sizeof(g_perf_stats));
}
