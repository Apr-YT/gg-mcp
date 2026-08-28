/**
 * atomic.h - 原子操作封装
 * 
 * 提供跨平台的原子操作接口，减少锁竞争，提高并发性能
 */

#ifndef AGENTD_ATOMIC_H
#define AGENTD_ATOMIC_H

#include <stdint.h>
#include <stdbool.h>

/* 原子类型 */
typedef struct {
    volatile int32_t value;
} atomic_int_t;

typedef struct {
    volatile uint32_t value;
} atomic_uint_t;

typedef struct {
    volatile int64_t value;
} atomic_int64_t;

typedef struct {
    volatile uint64_t value;
} atomic_uint64_t;

typedef struct {
    volatile void* value;
} atomic_ptr_t;

/* 原子操作宏 - GCC/Clang builtin */
#if defined(__GNUC__) || defined(__clang__)

#define ATOMIC_INIT(val) { (val) }

// 32位整数原子操作
#define atomic_load(ptr) __atomic_load_n(&(ptr)->value, __ATOMIC_ACQUIRE)
#define atomic_store(ptr, val) __atomic_store_n(&(ptr)->value, (val), __ATOMIC_RELEASE)
#define atomic_fetch_add(ptr, val) __atomic_fetch_add(&(ptr)->value, (val), __ATOMIC_ACQ_REL)
#define atomic_fetch_sub(ptr, val) __atomic_fetch_sub(&(ptr)->value, (val), __ATOMIC_ACQ_REL)
#define atomic_compare_exchange(ptr, expected, desired) \
    __atomic_compare_exchange_n(&(ptr)->value, expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
#define atomic_exchange(ptr, desired) __atomic_exchange_n(&(ptr)->value, desired, __ATOMIC_ACQ_REL)

// 无符号版本
#define atomic_load_u(ptr) __atomic_load_n(&(ptr)->value, __ATOMIC_ACQUIRE)
#define atomic_store_u(ptr, val) __atomic_store_n(&(ptr)->value, (val), __ATOMIC_RELEASE)
#define atomic_fetch_add_u(ptr, val) __atomic_fetch_add(&(ptr)->value, (val), __ATOMIC_ACQ_REL)

// 64位整数原子操作
#define atomic_load_64(ptr) __atomic_load_n(&(ptr)->value, __ATOMIC_ACQUIRE)
#define atomic_store_64(ptr, val) __atomic_store_n(&(ptr)->value, (val), __ATOMIC_RELEASE)
#define atomic_fetch_add_64(ptr, val) __atomic_fetch_add(&(ptr)->value, (val), __ATOMIC_ACQ_REL)

// 指针原子操作
#define atomic_load_ptr(ptr) __atomic_load_n(&(ptr)->value, __ATOMIC_ACQUIRE)
#define atomic_store_ptr(ptr, val) __atomic_store_n(&(ptr)->value, (val), __ATOMIC_RELEASE)
#define atomic_compare_exchange_ptr(ptr, expected, desired) \
    __atomic_compare_exchange_n(&(ptr)->value, expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

// 内存屏障
#define atomic_fence() __atomic_thread_fence(__ATOMIC_ACQ_REL)
#define atomic_fence_acquire() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define atomic_fence_release() __atomic_thread_fence(__ATOMIC_RELEASE)

#else
#error "Unsupported compiler: atomic operations require GCC or Clang"
#endif

/* 高级原子操作 */

/**
 * 原子自增并返回旧值
 */
static inline int32_t atomic_inc_and_fetch(atomic_int_t *ptr) {
    return atomic_fetch_add(ptr, 1) + 1;
}

/**
 * 原子自减并返回旧值
 */
static inline int32_t atomic_dec_and_fetch(atomic_int_t *ptr) {
    return atomic_fetch_sub(ptr, 1) - 1;
}

/**
 * 原子CAS循环（直到成功）
 */
static inline bool atomic_compare_exchange_loop(atomic_int_t *ptr, int32_t *expected, int32_t desired) {
    return atomic_compare_exchange(ptr, expected, desired);
}

/**
 * 原位原子加（返回新值）
 */
static inline int32_t atomic_add_fetch(atomic_int_t *ptr, int32_t val) {
    return atomic_fetch_add(ptr, val) + val;
}

/**
 * 原位原子减（返回新值）
 */
static inline int32_t atomic_sub_fetch(atomic_int_t *ptr, int32_t val) {
    return atomic_fetch_sub(ptr, val) - val;
}

/**
 * 原子位操作 - 设置位
 */
static inline uint32_t atomic_fetch_or(atomic_uint_t *ptr, uint32_t mask) {
    return __atomic_fetch_or(&(ptr)->value, mask, __ATOMIC_ACQ_REL);
}

/**
 * 原子位操作 - 清除位
 */
static inline uint32_t atomic_fetch_and(atomic_uint_t *ptr, uint32_t mask) {
    return __atomic_fetch_and(&(ptr)->value, mask, __ATOMIC_ACQ_REL);
}

/**
 * 原子位操作 - 异或位
 */
static inline uint32_t atomic_fetch_xor(atomic_uint_t *ptr, uint32_t mask) {
    return __atomic_fetch_xor(&(ptr)->value, mask, __ATOMIC_ACQ_REL);
}

/* 原子标志位（用于状态机） */
typedef struct {
    atomic_uint_t flags;
} atomic_flags_t;

#define atomic_flag_set(ptr, flag) atomic_fetch_or(ptr, (flag))
#define atomic_flag_clear(ptr, flag) atomic_fetch_and(ptr, ~(flag))
#define atomic_flag_test(ptr, flag) (atomic_load_u(ptr) & (flag))
#define atomic_flag_test_and_set(ptr, flag) (atomic_fetch_or(ptr, (flag)) & (flag))
#define atomic_flag_test_and_clear(ptr, flag) (atomic_fetch_and(ptr, ~(flag)) & (flag))

/* 原子引用计数 */
typedef struct {
    atomic_int_t ref_count;
} atomic_refcount_t;

static inline void atomic_ref_init(atomic_refcount_t *ref, int32_t count) {
    atomic_store(ref, count);
}

static inline int32_t atomic_ref_inc(atomic_refcount_t *ref) {
    return atomic_inc_and_fetch(ref);
}

static inline int32_t atomic_ref_dec(atomic_refcount_t *ref) {
    return atomic_dec_and_fetch(ref);
}

static inline int32_t atomic_ref_get(atomic_refcount_t *ref) {
    return atomic_load(ref);
}

/* 原子自旋锁（基于CAS） */
typedef struct {
    volatile int32_t locked;
} atomic_spinlock_t;

static inline void atomic_spinlock_init(atomic_spinlock_t *lock) {
    lock->locked = 0;
}

static inline void atomic_spinlock_lock(atomic_spinlock_t *lock) {
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        // CPU relax hint
        __asm__ __volatile__("pause" ::: "memory");
    }
}

static inline void atomic_spinlock_unlock(atomic_spinlock_t *lock) {
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

static inline bool atomic_spinlock_trylock(atomic_spinlock_t *lock) {
    return !__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE);
}

/* 性能统计用的原子计数器 */
typedef struct {
    atomic_uint64_t count;
    atomic_uint64_t bytes;
    atomic_uint64_t errors;
} atomic_stats_t;

static inline void atomic_stats_inc_count(atomic_stats_t *stats, uint64_t delta) {
    atomic_fetch_add_64(&stats->count, delta);
}

static inline void atomic_stats_inc_bytes(atomic_stats_t *stats, uint64_t delta) {
    atomic_fetch_add_64(&stats->bytes, delta);
}

static inline void atomic_stats_inc_errors(atomic_stats_t *stats) {
    atomic_fetch_add_64(&stats->errors, 1);
}

static inline uint64_t atomic_stats_get_count(atomic_stats_t *stats) {
    return atomic_load_64(&stats->count);
}

static inline uint64_t atomic_stats_get_bytes(atomic_stats_t *stats) {
    return atomic_load_64(&stats->bytes);
}

static inline uint64_t atomic_stats_get_errors(atomic_stats_t *stats) {
    return atomic_load_64(&stats->errors);
}

#endif /* AGENTD_ATOMIC_H */
