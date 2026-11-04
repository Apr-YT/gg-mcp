/* freeze.h — 冻结注册表与循环线程 */
#ifndef GG_AGENTD_FREEZE_H
#define GG_AGENTD_FREEZE_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
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
    /* attach 时目标的 starttime（见 process_starttime）。循环线程每轮比对，
     * 不等就说明 pid 已被复用 —— 必须立刻停写，否则会持续改写一个无关进程。 */
    uint64_t     pid_starttime;
    gg_freeze_entry_t entries[GG_FREEZE_MAX];
    size_t       count;
    _Atomic int running;    /* 跨线程可见性: freeze_loop 轮询、freeze_stop 置 0，用原子读避免数据竞争 */
    int          thread_started;  /* 线程确实创建成功才允许 join —— 否则会 join 一个未初始化的 pthread_t */
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

/* ---- 冻结表持久化（agentd 重启后恢复冻结）----
 *
 * 序列化格式（value 为 size 字节的 hex，寄存器字节序即存储字节序）：
 *   {"count":N,"entries":[{"addr":"0x..","size":n,"active":0|1,"value":"0x.."}]}
 *
 * serialize：手工拼接（条目结构定长，hex 无转义问题）；空表返回 NULL。
 * deserialize：全或无 —— 任一条目非法（size 不在 1..8 / count 超
 * GG_FREEZE_MAX / 字段缺失）则原表原样保留并返回 -1；全部合法才在
 * **同一把锁内**替换注册表（循环线程可能正在跑，逐条 add 会露出
 * 半旧半新的中间态）。active 保留，last_err 清零。
 * 返回条目数；-1 = 解析/校验失败。 */
char *freeze_serialize(const gg_freeze_t *f);
int64_t freeze_deserialize(gg_freeze_t *f, const char *json_text);

#ifdef __cplusplus
}
#endif

#endif /* GG_AGENTD_FREEZE_H */
