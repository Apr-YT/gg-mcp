/**
 * atomic_simple.h - 轻量级原子操作模块
 */

#ifndef GG_ATOMIC_SIMPLE_H
#define GG_ATOMIC_SIMPLE_H

#include <stdint.h>

typedef struct {
    volatile int32_t value;
} atomic_int32_t;

typedef struct {
    volatile int64_t value;
} atomic_int64_t;

#define ATOMIC_INIT(val) { (val) }

static inline int32_t atomic_load32(atomic_int32_t *a) {
    return __atomic_load_n(&a->value, __ATOMIC_ACQUIRE);
}

static inline void atomic_store32(atomic_int32_t *a, int32_t v) {
    __atomic_store_n(&a->value, v, __ATOMIC_RELEASE);
}

static inline int32_t atomic_fetch_add32(atomic_int32_t *a, int32_t v) {
    return __atomic_fetch_add(&a->value, v, __ATOMIC_ACQ_REL);
}

static inline int32_t atomic_fetch_sub32(atomic_int32_t *a, int32_t v) {
    return __atomic_fetch_sub(&a->value, v, __ATOMIC_ACQ_REL);
}

static inline int32_t atomic_compare_exchange32(atomic_int32_t *a, int32_t *exp, int32_t des) {
    return __atomic_compare_exchange_n(&a->value, exp, des, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline int64_t atomic_load64(atomic_int64_t *a) {
    return __atomic_load_n(&a->value, __ATOMIC_ACQUIRE);
}

static inline void atomic_store64(atomic_int64_t *a, int64_t v) {
    __atomic_store_n(&a->value, v, __ATOMIC_RELEASE);
}

static inline int64_t atomic_fetch_add64(atomic_int64_t *a, int64_t v) {
    return __atomic_fetch_add(&a->value, v, __ATOMIC_ACQ_REL);
}

static inline int32_t atomic_compare_exchange64(atomic_int64_t *a, int64_t *exp, int64_t des) {
    return __atomic_compare_exchange_n(&a->value, exp, des, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline void atomic_inc32(atomic_int32_t *a) {
    atomic_fetch_add32(a, 1);
}

static inline void atomic_dec32(atomic_int32_t *a) {
    atomic_fetch_sub32(a, 1);
}

static inline int32_t atomic_get_and_inc32(atomic_int32_t *a) {
    return atomic_fetch_add32(a, 1);
}

typedef struct {
    atomic_int32_t count;
} atomic_refcount_t;

static inline void refcount_init(atomic_refcount_t *ref, int32_t val) {
    atomic_store32(&ref->count, val);
}

static inline int32_t refcount_inc(atomic_refcount_t *ref) {
    return atomic_get_and_inc32(&ref->count);
}

static inline int32_t refcount_dec(atomic_refcount_t *ref) {
    return atomic_fetch_sub32(&ref->count, 1);
}

static inline int32_t refcount_get(atomic_refcount_t *ref) {
    return atomic_load32(&ref->count);
}

#endif /* GG_ATOMIC_SIMPLE_H */
