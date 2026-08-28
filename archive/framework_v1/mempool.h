/**
 * mempool.h - 高性能内存池实现
 * 
 * 减少频繁的malloc/free调用，提高性能，避免内存碎片
 */

#ifndef AGENTD_MEMPOOL_H
#define AGENTD_MEMPOOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* 内存池配置 */
#define MEMPOOL_BLOCK_SIZE 4096      /* 每个块4KB */
#define MEMPOOL_MAX_BLOCKS 1024     /* 最大块数 */
#define MEMPOOL_ALIGNMENT 16        /* 16字节对齐 */

/* 内存块 */
typedef struct mempool_block {
    struct mempool_block *next;
    uint8_t data[MEMPOOL_BLOCK_SIZE];
    size_t used;
    bool free;
} mempool_block_t;

/* 内存池 */
typedef struct {
    mempool_block_t *blocks;
    mempool_block_t *free_list;
    size_t total_blocks;
    size_t used_blocks;
    size_t total_allocated;
    size_t total_freed;
    pthread_mutex_t mutex;
    bool initialized;
} mempool_t;

/* 内存池统计信息 */
typedef struct {
    size_t total_blocks;
    size_t used_blocks;
    size_t free_blocks;
    size_t total_allocated;
    size_t total_freed;
    size_t fragmentation;  /* 碎片率 (0-100) */
} mempool_stats_t;

/* API函数 */

/**
 * 初始化内存池
 * @param pool 内存池指针
 * @param initial_blocks 初始块数
 * @return 0成功，-1失败
 */
int mempool_init(mempool_t *pool, size_t initial_blocks);

/**
 * 销毁内存池
 * @param pool 内存池指针
 */
void mempool_destroy(mempool_t *pool);

/**
 * 从内存池分配内存
 * @param pool 内存池指针
 * @param size 分配大小
 * @param alignment 对齐要求
 * @return 分配的内存指针，失败返回NULL
 */
void* mempool_alloc(mempool_t *pool, size_t size, size_t alignment);

/**
 * 释放内存到池
 * @param pool 内存池指针
 * @param ptr 要释放的指针
 */
void mempool_free(mempool_t *pool, void *ptr);

/**
 * 清空内存池（释放所有块）
 * @param pool 内存池指针
 */
void mempool_clear(mempool_t *pool);

/**
 * 获取内存池统计信息
 * @param pool 内存池指针
 * @param stats 输出的统计信息
 * @return 0成功，-1失败
 */
int mempool_get_stats(mempool_t *pool, mempool_stats_t *stats);

/**
 * 打印内存池统计信息到日志
 * @param pool 内存池指针
 */
void mempool_print_stats(mempool_t *pool);

/* 全局内存池实例（用于常用小内存分配） */
extern mempool_t g_small_pool;    /* 小对象池 (<= 256B) */
extern mempool_t g_medium_pool;   /* 中对象池 (<= 4KB) */
extern mempool_t g_large_pool;    /* 大对象池 (<= 64KB) */

/**
 * 初始化全局内存池
 * @return 0成功，-1失败
 */
int mempool_global_init(void);

/**
 * 销毁全局内存池
 */
void mempool_global_destroy(void);

/* 便捷宏 */

// 从全局池分配
#define POOL_ALLOC(size) mempool_alloc_from_pool(&g_medium_pool, size)
#define POOL_ALLOC_SMALL(size) mempool_alloc_from_pool(&g_small_pool, size)
#define POOL_ALLOC_LARGE(size) mempool_alloc_from_pool(&g_large_pool, size)

// 释放到全局池
#define POOL_FREE(ptr) mempool_free_from_pool(&g_medium_pool, ptr)
#define POOL_FREE_SMALL(ptr) mempool_free_from_pool(&g_small_pool, ptr)
#define POOL_FREE_LARGE(ptr) mempool_free_from_pool(&g_large_pool, ptr)

#endif /* AGENTD_MEMPOOL_H */
