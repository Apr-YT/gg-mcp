/**
 * mempool.c - 内存池实现
 */

#include "mempool.h"
#include "error.h"
#include <stdlib.h>
#include <string.h>

/* 全局内存池实例 */
mempool_t g_small_pool;
mempool_t g_medium_pool;
mempool_t g_large_pool;

int mempool_init(mempool_t *pool, size_t initial_blocks) {
    if (pool == NULL) {
        SET_ERROR(ERR_NULL_PTR, "pool is NULL");
        return -1;
    }

    memset(pool, 0, sizeof(mempool_t));
    
    // 分配块数组
    pool->blocks = (mempool_block_t*)calloc(initial_blocks, sizeof(mempool_block_t));
    if (pool->blocks == NULL) {
        SET_ERROR(ERR_ALLOC_FAILED, "Failed to allocate block array");
        return -1;
    }

    // 初始化空闲链表
    pool->free_list = &pool->blocks[0];
    for (size_t i = 0; i < initial_blocks - 1; i++) {
        pool->blocks[i].next = &pool->blocks[i + 1];
        pool->blocks[i].free = true;
    }
    pool->blocks[initial_blocks - 1].next = NULL;
    pool->blocks[initial_blocks - 1].free = true;

    pool->total_blocks = initial_blocks;
    pool->used_blocks = 0;
    
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        SET_ERROR(ERR_INTERNAL, "Failed to init mutex");
        free(pool->blocks);
        return -1;
    }

    pool->initialized = true;
    return 0;
}

void mempool_destroy(mempool_t *pool) {
    if (pool == NULL || !pool->initialized) {
        return;
    }

    pthread_mutex_destroy(&pool->mutex);
    free(pool->blocks);
    memset(pool, 0, sizeof(mempool_t));
}

void* mempool_alloc(mempool_t *pool, size_t size, size_t alignment) {
    if (pool == NULL || !pool->initialized) {
        SET_ERROR(ERR_NULL_PTR, "pool not initialized");
        return NULL;
    }

    if (size == 0 || size > MEMPOOL_BLOCK_SIZE) {
        SET_ERROR(ERR_INVALID_PARAM, "Invalid size");
        return NULL;
    }

    pthread_mutex_lock(&pool->mutex);

    // 查找空闲块
    mempool_block_t *block = pool->free_list;
    mempool_block_t *prev = NULL;
    
    while (block != NULL) {
        if (block->free && (block->used + size <= MEMPOOL_BLOCK_SIZE)) {
            // 找到合适的块
            if (prev != NULL) {
                prev->next = block->next;
            } else {
                pool->free_list = block->next;
            }
            
            block->free = false;
            block->used = size;
            pool->used_blocks++;
            pool->total_allocated += size;
            
            pthread_mutex_unlock(&pool->mutex);
            return block->data;
        }
        prev = block;
        block = block->next;
    }

    // 没有找到合适的块
    pthread_mutex_unlock(&pool->mutex);
    SET_ERROR(ERR_ALLOC_FAILED, "No free block available");
    return NULL;
}

void mempool_free(mempool_t *pool, void *ptr) {
    if (pool == NULL || !pool->initialized || ptr == NULL) {
        return;
    }

    pthread_mutex_lock(&pool->mutex);

    // 查找对应的块
    for (size_t i = 0; i < pool->total_blocks; i++) {
        if (pool->blocks[i].data == ptr) {
            mempool_block_t *block = &pool->blocks[i];
            
            if (block->free) {
                LOG_WARN("Double free detected: %p", ptr);
                pthread_mutex_unlock(&pool->mutex);
                return;
            }

            // 添加到空闲链表
            block->next = pool->free_list;
            pool->free_list = block;
            block->free = true;
            
            pool->total_freed += block->used;
            block->used = 0;
            pool->used_blocks--;
            
            pthread_mutex_unlock(&pool->mutex);
            return;
        }
    }

    pthread_mutex_unlock(&pool->mutex);
    LOG_WARN("Invalid pointer freed: %p", ptr);
}

void mempool_clear(mempool_t *pool) {
    if (pool == NULL || !pool->initialized) {
        return;
    }

    pthread_mutex_lock(&pool->mutex);

    // 重置所有块
    pool->free_list = &pool->blocks[0];
    for (size_t i = 0; i < pool->total_blocks - 1; i++) {
        pool->blocks[i].next = &pool->blocks[i + 1];
        pool->blocks[i].free = true;
        pool->blocks[i].used = 0;
    }
    pool->blocks[pool->total_blocks - 1].next = NULL;
    pool->blocks[pool->total_blocks - 1].free = true;
    pool->blocks[pool->total_blocks - 1].used = 0;

    pool->used_blocks = 0;
    
    pthread_mutex_unlock(&pool->mutex);
}

int mempool_get_stats(mempool_t *pool, mempool_stats_t *stats) {
    if (pool == NULL || !pool->initialized || stats == NULL) {
        SET_ERROR(ERR_NULL_PTR, "Invalid parameters");
        return -1;
    }

    pthread_mutex_lock(&pool->mutex);

    stats->total_blocks = pool->total_blocks;
    stats->used_blocks = pool->used_blocks;
    stats->free_blocks = pool->total_blocks - pool->used_blocks;
    stats->total_allocated = pool->total_allocated;
    stats->total_freed = pool->total_freed;
    
    // 计算碎片率
    if (pool->total_allocated > 0) {
        stats->fragmentation = 
            (stats->used_blocks * 100) / stats->total_blocks;
    } else {
        stats->fragmentation = 0;
    }

    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

void mempool_print_stats(mempool_t *pool) {
    if (pool == NULL || !pool->initialized) {
        LOG_ERROR("Pool not initialized");
        return;
    }

    mempool_stats_t stats;
    if (mempool_get_stats(pool, &stats) == 0) {
        LOG_INFO("Memory Pool Stats:");
        LOG_INFO("  Total blocks: %zu", stats.total_blocks);
        LOG_INFO("  Used blocks: %zu", stats.used_blocks);
        LOG_INFO("  Free blocks: %zu", stats.free_blocks);
        LOG_INFO("  Total allocated: %zu bytes", stats.total_allocated);
        LOG_INFO("  Total freed: %zu bytes", stats.total_freed);
        LOG_INFO("  Fragmentation: %zu%%", stats.fragmentation);
    }
}

int mempool_global_init(void) {
    int ret = 0;
    
    if (mempool_init(&g_small_pool, 64) != 0) {
        LOG_ERROR("Failed to init small pool");
        ret = -1;
    }
    
    if (mempool_init(&g_medium_pool, 256) != 0) {
        LOG_ERROR("Failed to init medium pool");
        ret = -1;
    }
    
    if (mempool_init(&g_large_pool, 128) != 0) {
        LOG_ERROR("Failed to init large pool");
        ret = -1;
    }
    
    return ret;
}

void mempool_global_destroy(void) {
    mempool_destroy(&g_small_pool);
    mempool_destroy(&g_medium_pool);
    mempool_destroy(&g_large_pool);
}
