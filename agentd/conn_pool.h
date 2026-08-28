/**
 * conn_pool.h - 连接池模块
 * 复用连接，减少握手开销
 */

#ifndef GG_CONN_POOL_H
#define GG_CONN_POOL_H

#include <stdint.h>
#include <stdbool.h>

#define POOL_SIZE 4

typedef struct {
    int fd;
    bool in_use;
    uint64_t last_used;
} conn_slot_t;

typedef struct {
    conn_slot_t slots[POOL_SIZE];
    uint32_t next_id;
} conn_pool_t;

/* 初始化连接池 */
static inline void conn_pool_init(conn_pool_t *pool) {
    for (int i = 0; i < POOL_SIZE; i++) {
        pool->slots[i].fd = -1;
        pool->slots[i].in_use = false;
        pool->slots[i].last_used = 0;
    }
    pool->next_id = 0;
}

/* 获取连接 */
static inline int conn_pool_get(conn_pool_t *pool) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!pool->slots[i].in_use && pool->slots[i].fd >= 0) {
            pool->slots[i].in_use = true;
            pool->slots[i].last_used = 0;
            return pool->slots[i].fd;
        }
    }
    return -1;
}

/* 释放连接 */
static inline void conn_pool_put(conn_pool_t *pool, int fd) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (pool->slots[i].fd == fd) {
            pool->slots[i].in_use = false;
            pool->slots[i].last_used = 0;
            return;
        }
    }
}

/* 添加连接 */
static inline void conn_pool_add(conn_pool_t *pool, int fd) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (pool->slots[i].fd == -1) {
            pool->slots[i].fd = fd;
            pool->slots[i].in_use = false;
            return;
        }
    }
}

/* 检查所有连接 */
static inline void conn_pool_check(conn_pool_t *pool) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (pool->slots[i].in_use) {
            // 检查连接是否仍然有效
            // 实际实现需要具体检查逻辑
        }
    }
}

#endif /* GG_CONN_POOL_H */
