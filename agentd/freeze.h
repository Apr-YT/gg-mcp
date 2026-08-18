/* freeze.h — 冻结注册表与循环线程 */
#ifndef GG_AGENTD_FREEZE_H
#define GG_AGENTD_FREEZE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "driver.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GG_FREEZE_MAX       256
#define GG_FREEZE_INTERVAL_MS 100

typedef struct gg_freeze_entry {
    uint64_t addr;
    uint8_t  value[8];
    uint8_t  size;
    int      active;
    int      last_err;      /* 最近一次写入错误号 */
} gg_freeze_entry_t;

typedef struct gg_freeze {
    gg_driver_t *drv;
    pid_t        pid;
    gg_freeze_entry_t entries[GG_FREEZE_MAX];
    size_t       count;
    int          running;
    void        *mtx;       /* pthread_mutex_t* */
    void        *thread;    /* pthread_t* */
} gg_freeze_t;

int  freeze_init(gg_freeze_t *f, gg_driver_t *drv);
int  freeze_start(gg_freeze_t *f, pid_t pid);        /* 启动循环线程 */
void freeze_stop(gg_freeze_t *f);                    /* 停止线程并清空 */
int  freeze_add(gg_freeze_t *f, uint64_t addr, const void *value, size_t size);
int  freeze_remove(gg_freeze_t *f, uint64_t addr);   /* 移除单条 */
void freeze_clear(gg_freeze_t *f);
size_t freeze_count(gg_freeze_t *f);

#ifdef __cplusplus
}
#endif

#endif /* GG_AGENTD_FREEZE_H */
