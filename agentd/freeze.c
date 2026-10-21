/* freeze.c — 冻结注册表与循环线程
 *
 * 循环线程每 GG_FREEZE_INTERVAL_MS 毫秒把注册表内所有地址写回锁定值。
 * 进程退出（写入 ESRCH）时自动停掉对应条目并记 last_err。
 */
#include "freeze.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int freeze_init(gg_freeze_t *f, gg_driver_t *drv) {
    memset(f, 0, sizeof(*f));
    f->drv = drv;
    f->pid = -1;
    f->mtx = malloc(sizeof(pthread_mutex_t));
    f->thread = malloc(sizeof(pthread_t));
    if (!f->mtx || !f->thread) return -1;
    pthread_mutex_init((pthread_mutex_t *)f->mtx, NULL);
    return 0;
}

static void *freeze_loop(void *arg) {
    gg_freeze_t *f = (gg_freeze_t *)arg;
    while (atomic_load(&f->running)) {
        pthread_mutex_lock((pthread_mutex_t *)f->mtx);
        for (size_t i = 0; i < f->count; i++) {
            gg_freeze_entry_t *e = &f->entries[i];
            if (!e->active) continue;
            errno = 0;
            ssize_t r = driver_write(f->drv, f->pid, e->addr, e->value, e->size);
            if (r < 0) {
                /* 驱动失败分支不一定设 errno（如"节点未打开"），用 EIO 兜底，
                 * 否则 last_err=0 会被展示端当成"冻结正常" */
                e->last_err = errno ? errno : EIO;
                if (e->last_err == ESRCH) e->active = 0;   /* 进程退出 */
            } else if (r != (ssize_t)e->size) {
                e->last_err = EIO;   /* 短写：锁定值没完整落进目标 */
            } else {
                e->last_err = 0;
            }
        }
        pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
        usleep(GG_FREEZE_INTERVAL_MS * 1000);
    }
    return NULL;
}

int freeze_start(gg_freeze_t *f, pid_t pid) {
    if (!f || !f->mtx || !f->thread) return -1;
    pthread_mutex_lock((pthread_mutex_t *)f->mtx);
    if (f->thread_started) {   /* 已在运行：不重复创建，否则旧线程变孤儿 */
        pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
        return -1;
    }
    f->pid = pid;
    atomic_store(&f->running, 1);
    pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
    if (pthread_create((pthread_t *)f->thread, NULL, freeze_loop, f) != 0) {
        pthread_mutex_lock((pthread_mutex_t *)f->mtx);
        atomic_store(&f->running, 0);
        pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
        return -1;   /* 创建失败必须让调用方知道，否则冻结静默失效 */
    }
    pthread_mutex_lock((pthread_mutex_t *)f->mtx);
    f->thread_started = 1;
    pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
    return 0;
}

void freeze_stop(gg_freeze_t *f) {
    if (!f || !f->mtx) return;
    int started;
    pthread_mutex_lock((pthread_mutex_t *)f->mtx);
    atomic_store(&f->running, 0);
    started = f->thread_started;
    pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
    /* 只在确实创建过线程时 join。原实现无条件 join 一块 malloc 出来但从未
     * 初始化过的 pthread_t —— attach 流程在首次启动前就会调到这里。 */
    if (started && f->thread) {
        pthread_join(*(pthread_t *)f->thread, NULL);
        pthread_mutex_lock((pthread_mutex_t *)f->mtx);
        f->thread_started = 0;
        pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
    }
    pthread_mutex_lock((pthread_mutex_t *)f->mtx);
    memset(f->entries, 0, sizeof(f->entries));
    f->count = 0;
    pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
}

int freeze_add(gg_freeze_t *f, uint64_t addr, const void *value, size_t size) {
    if (size > 8) return -1;
    pthread_mutex_lock((pthread_mutex_t *)f->mtx);
    /* 查重：同地址更新值 */
    for (size_t i = 0; i < f->count; i++) {
        if (f->entries[i].addr == addr) {
            memcpy(f->entries[i].value, value, size);
            f->entries[i].size = (uint8_t)size;
            f->entries[i].active = 1;
            f->entries[i].last_err = 0;
            pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
            return 0;
        }
    }
    if (f->count >= GG_FREEZE_MAX) { pthread_mutex_unlock((pthread_mutex_t *)f->mtx); return -1; }
    gg_freeze_entry_t *e = &f->entries[f->count++];
    e->addr = addr;
    memcpy(e->value, value, size);
    e->size = (uint8_t)size;
    e->active = 1;
    pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
    return 0;
}

int freeze_remove(gg_freeze_t *f, uint64_t addr) {
    pthread_mutex_lock((pthread_mutex_t *)f->mtx);
    for (size_t i = 0; i < f->count; i++) {
        if (f->entries[i].addr == addr) {
            memmove(&f->entries[i], &f->entries[i + 1],
                    (f->count - i - 1) * sizeof(gg_freeze_entry_t));
            f->count--;
            pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
            return 0;
        }
    }
    pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
    return -1;
}

void freeze_clear(gg_freeze_t *f) {
    pthread_mutex_lock((pthread_mutex_t *)f->mtx);
    memset(f->entries, 0, sizeof(f->entries));
    f->count = 0;
    pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
}

size_t freeze_count(gg_freeze_t *f) {
    pthread_mutex_lock((pthread_mutex_t *)f->mtx);
    size_t n = f->count;
    pthread_mutex_unlock((pthread_mutex_t *)f->mtx);
    return n;
}
