/**
 * error.h - GG-MCP Agentd 错误处理框架
 * 
 * 提供统一的错误处理宏和函数，增强代码健壮性
 */

#ifndef AGENTD_ERROR_H
#define AGENTD_ERROR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* 错误码定义 */
typedef enum {
    ERR_OK = 0,
    ERR_NULL_PTR = 1,
    ERR_ALLOC_FAILED = 2,
    ERR_INVALID_PARAM = 3,
    ERR_IO_FAILED = 4,
    ERR_TIMEOUT = 5,
    ERR_PERMISSION = 6,
    ERR_NOT_FOUND = 7,
    ERR_BUFFER_OVERFLOW = 8,
    ERR_INTERNAL = 9
} error_code_t;

/* 错误上下文 */
typedef struct {
    error_code_t code;
    const char *message;
    const char *file;
    int line;
    int system_errno;
} error_context_t;

/* 线程局部错误上下文 */
#define ERROR_BUFFER_SIZE 256
static __thread error_context_t g_last_error = {ERR_OK, NULL, NULL, 0, 0};
static __thread char g_error_buffer[ERROR_BUFFER_SIZE] = {0};

/* 错误处理宏 */

// 检查指针非NULL
#define CHECK_NULL(ptr, label) do { \
    if ((ptr) == NULL) { \
        SET_ERROR(ERR_NULL_PTR, "NULL pointer: " #ptr); \
        goto label; \
    } \
} while(0)

// 检查分配成功
#define CHECK_ALLOC(ptr, label) do { \
    if ((ptr) == NULL) { \
        SET_ERROR(ERR_ALLOC_FAILED, "Memory allocation failed: " #ptr); \
        goto label; \
    } \
} while(0)

// 检查参数有效性
#define CHECK_PARAM(cond, label) do { \
    if (!(cond)) { \
        SET_ERROR(ERR_INVALID_PARAM, "Invalid parameter: " #cond); \
        goto label; \
    } \
} while(0)

// 检查IO操作成功
#define CHECK_IO(ret, label) do { \
    if ((ret) < 0) { \
        SET_ERROR(ERR_IO_FAILED, "IO operation failed"); \
        g_last_error.system_errno = errno; \
        goto label; \
    } \
} while(0)

// 检查边界条件
#define CHECK_BOUNDS(index, size, label) do { \
    if ((index) >= (size)) { \
        SET_ERROR(ERR_BUFFER_OVERFLOW, "Buffer overflow detected"); \
        goto label; \
    } \
} while(0)

// 安全的malloc（带NULL检查和错误设置）
#define SAFE_MALLOC(ptr, size, label) do { \
    (ptr) = malloc(size); \
    CHECK_ALLOC(ptr, label); \
    memset(ptr, 0, size); \
} while(0)

// 安全的calloc（带错误设置）
#define SAFE_CALLOC(ptr, nmemb, size, label) do { \
    (ptr) = calloc(nmemb, size); \
    CHECK_ALLOC(ptr, label); \
} while(0)

// 安全的free（带NULL检查）
#define SAFE_FREE(ptr) do { \
    if ((ptr) != NULL) { \
        free(ptr); \
        (ptr) = NULL; \
    } \
} while(0)

// 设置错误
#define SET_ERROR(code, msg) do { \
    g_last_error.code = (error_code_t)code; \
    g_last_error.message = msg; \
    g_last_error.file = __FILE__; \
    g_last_error.line = __LINE__; \
    g_last_error.system_errno = errno; \
    snprintf(g_error_buffer, ERROR_BUFFER_SIZE, "[%s:%d] %s (errno=%d)", \
             __FILE__, __LINE__, msg, errno); \
} while(0)

// 获取最后的错误信息
static inline const char* get_last_error(void) {
    return g_error_buffer;
}

// 获取最后的错误码
static inline error_code_t get_last_error_code(void) {
    return g_last_error.code;
}

// 清除错误
static inline void clear_error(void) {
    memset(&g_last_error, 0, sizeof(g_last_error));
    g_error_buffer[0] = '\0';
}

// 日志宏
#define LOG_ERROR(fmt, ...) do { \
    fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); \
} while(0)

#define LOG_WARN(fmt, ...) do { \
    fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__); \
} while(0)

#define LOG_INFO(fmt, ...) do { \
    fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__); \
} while(0)

#define LOG_DEBUG(fmt, ...) do { \
    fprintf(stdout, "[DEBUG] " fmt "\n", ##__VA_ARGS__); \
} while(0)

#endif /* AGENTD_ERROR_H */
