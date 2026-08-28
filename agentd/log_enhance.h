/**
 * log_enhance.h - 日志增强模块（兼容现有log_msg）
 * 
 * 为现有log_msg系统添加功能扩展，不破坏现有代码
 */

#ifndef GG_LOG_ENHANCE_H
#define GG_LOG_ENHANCE_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

/* 日志级别 */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} log_level_t;

/* 外部日志函数（来自main.c） */
extern void log_msg(const char *fmt, ...);

/* 带级别的日志函数 */
static inline void log_debug(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    log_msg("[DEBUG] %s", buf);
    va_end(ap);
}

static inline void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    log_msg("[INFO] %s", buf);
    va_end(ap);
}

static inline void log_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    log_msg("[WARN] %s", buf);
    va_end(ap);
}

static inline void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    log_msg("[ERROR] %s", buf);
    va_end(ap);
}

/* 错误码定义（简单版，避免复杂依赖） */
#define GG_OK          0
#define GG_ERROR      -1
#define GG_NOMEM      -2
#define GG_INVAL      -3
#define GG_IOERR      -4
#define GG_TIMEOUT    -5

/* 错误处理宏（简化版） */
#define RETURN_ERROR(code, msg) do { \
    log_error("%s: " msg, __func__); \
    return (code); \
} while(0)

#define RETURN_IF_ERROR(expr, msg) do { \
    int ret = (expr); \
    if (ret != GG_OK) { \
        log_error("%s: " msg " (ret=%d)", __func__, ret); \
        return ret; \
    } \
} while(0)

/* NULL检查宏（简单版） */
#define CHECK_NULL(ptr, label) do { \
    if ((ptr) == NULL) { \
        log_error("NULL pointer: " #ptr); \
        goto label; \
    } \
} while(0)

/* 安全分配宏（简单版，带NULL检查） */
#define SAFE_MALLOC(ptr, size, label) do { \
    (ptr) = malloc(size); \
    CHECK_NULL(ptr, label); \
    memset(ptr, 0, size); \
} while(0)

/* 安全释放宏 */
#define SAFE_FREE(ptr) do { \
    if ((ptr) != NULL) { \
        free(ptr); \
        (ptr) = NULL; \
    } \
} while(0)

#endif /* GG_LOG_ENHANCE_H */
