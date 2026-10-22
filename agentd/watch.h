/* watch.h — 硬件断点监视（CE 式"什么访问了这个地址"）
 *
 * 用 ptrace 附加目标进程全部线程，对指定地址设置 ARM64 硬件监视点
 * （NT_ARM_HW_WATCH / DBGWVR+DBGWCR）。目标线程写该地址时触发
 * SIGTRAP(TRAP_HWBKPT)，由本模块捕获并记录 {tid, pc, 寄存器}，
 * 据此可反查写者指令 → 找到权威变量（如金币本体）。
 *
 * 
 *
 * 命中日志增强（缺口 3）：poll 时对每条命中自动
 *   1) 解析 pc 所属模块 + 偏移（process_resolve_addr）
 *   2) FP 链调用栈回溯（最多 GG_WATCH_STACK_MAX 层）
 *   3) 预反汇编 pc±16 条（GG_WATCH_DISASM_MAX 条）随日志返回
 */
#ifndef GG_AGENTD_WATCH_H
#define GG_AGENTD_WATCH_H

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

#define GG_WATCH_MAX_HITS 128
#define GG_WATCH_STACK_MAX 5
#define GG_WATCH_DISASM_MAX 32      /* ±16 条（ARM64 定长 4B → pc-64..pc+64） */
#define GG_RWPROC_MAX_BPS 128        /* 全部线程注册（per-task 断点每线程独立 debug 槽位, 不受每 CPU WRP 4 槽限） */

typedef struct gg_watch_insn {
    uint64_t addr;
    char     bytes[32];
    char     mnemonic[64];
    char     op_str[128];
    char     comment[64];
} gg_watch_insn_t;

typedef struct gg_watch_hit {
    pid_t    tid;
    uint64_t pc;                 /* 触发指令地址 */
    uint64_t addr;               /* 被访问地址 */
    uint64_t r0, r1, r2, r3, r4; /* 触发时刻寄存器（x0-x4） */
    uint64_t fp;                 /* x29 (frame pointer, 回溯用) */
    uint64_t sp;                 /* sp */
    uint64_t lr;                 /* x30 (link register) */

    /* 模块归属（pc 所在模块） */
    char     module[256];        /* basename */
    char     module_path[512];
    uint64_t module_base;
    uint64_t module_offset;

    /* 调用栈（stack_pc[0] = 命中 pc, 依次上层返回地址） */
    int      stack_depth;
    uint64_t stack_pc[GG_WATCH_STACK_MAX];

    /* 预反汇编 pc±16 条 */
    int           disasm_count;
    gg_watch_insn_t disasm[GG_WATCH_DISASM_MAX];
} gg_watch_hit_t;

typedef struct gg_watch {
    pid_t   pid;
    int     active;
    int     use_driver;            /* 0=ptrace, 1=QX 驱动, 2=rwproc33 驱动 */
    struct gg_driver *drv;         /* use_driver 时的驱动 */
    uint64_t bp_addr;              /* 断点地址（get_hits 用） */
    pid_t  *tids;                /* 已附加线程（ptrace 路径） */
    int     ntids;
    pthread_t th;                /* waitpid 等待线程（ptrace 路径） */
    volatile int stop;
    pthread_mutex_t mtx;
    gg_watch_hit_t hits[GG_WATCH_MAX_HITS];
    int  hit_count;
    /* rwproc33 路径（hwBreakpointProc_module）:
     * 硬件断点默认只绑目标 pid 的 leader 线程（模块 CMD_INST 用 pid_task(PIDTYPE_PID)），
     * 而游戏写入常来自工作线程 → 命中率为 0。因此按线程注册: 最多 GG_RWPROC_MAX_BPS 个
     * 断点（主线程 + 最活跃线程），每个 bp 一个 perf_event + 独立命中消费游标
     * （模块命中累计不清零, 去重用）。 */
    int      rwproc_bp_count;
    pid_t    rwproc_bp_tids[GG_RWPROC_MAX_BPS];
    uint64_t rwproc_bp_ids[GG_RWPROC_MAX_BPS];
    uint64_t rwproc_hit_consumed[GG_RWPROC_MAX_BPS];
    char last_error[256];
} gg_watch_t;

/* 启动监视: drv 为 QX 驱动时走驱动硬件断点（0x807-0x809）,
 * 否则 ptrace 附加全部线程设 ARM64 硬件监视点。
 * type ∈ HW_BP_TYPE_*（默认 W）, len 默认 4。返回 0 成功；负值失败。 */
int watch_start(gg_watch_t *w, struct gg_driver *drv, pid_t pid, uint64_t addr,
                int type, int len);

/* 停止监视并 detach 全部线程。 */
void watch_stop(gg_watch_t *w);

/* 取走当前命中列表（清空内部计数），并对每条命中做
 * 模块解析 + FP 链回溯 + 预反汇编（需 drv 可读目标内存）。
 * 返回数量。 */
int watch_poll(gg_watch_t *w, struct gg_driver *drv, gg_watch_hit_t *out, int cap);

/* 调试注入：手动构造一条命中并走完整 enrich 管道（模块/回溯/反汇编）。
 * 供断点不可用环境验证日志管道。返回 0 成功。 */
int watch_inject(gg_watch_t *w, struct gg_driver *drv, pid_t pid,
                 uint64_t pc, uint64_t fp, uint64_t sp, uint64_t lr,
                 uint64_t addr, gg_watch_hit_t *out);

#endif /* GG_AGENTD_WATCH_H */
