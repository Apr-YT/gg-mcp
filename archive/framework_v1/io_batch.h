/**
 * io_batch.h - I/O批处理模块
 * 减少系统调用次数，提升吞吐量
 */

#ifndef GG_IO_BATCH_H
#define GG_IO_BATCH_H

#include <stdint.h>
#include <stddef.h>

#define IO_BATCH_SIZE 4096

typedef struct {
    uint8_t buffer[IO_BATCH_SIZE];
    size_t used;
} io_batch_t;

/* 初始化批处理缓冲区 */
static inline void io_batch_init(io_batch_t *batch) {
    batch->used = 0;
}

/* 添加数据到批处理 */
static inline int io_batch_add(io_batch_t *batch, const uint8_t *data, size_t len) {
    if (batch->used + len > IO_BATCH_SIZE) {
        return -1;  // 缓冲区满
    }
    
    if (data) {
        memcpy(batch->buffer + batch->used, data, len);
    }
    batch->used += len;
    return 0;
}

/* 发送批处理数据 */
static inline int io_batch_flush(int fd, io_batch_t *batch) {
    if (batch->used == 0) return 0;
    
    ssize_t sent = send(fd, batch->buffer, batch->used, 0);
    batch->used = 0;
    
    return sent > 0 ? 0 : -1;
}

/* 获取批处理中的可用空间 */
static inline size_t io_batch_available(io_batch_t *batch) {
    return IO_BATCH_SIZE - batch->used;
}

#endif /* GG_IO_BATCH_H */
