/* watch.c — 硬件断点监视实现（ptrace + ARM64 NT_ARM_HW_WATCH）
 *
 * 流程: 枚举 /proc/<pid>/task 全部线程 → PTRACE_ATTACH（全部停住）
 * → 每线程 PTRACE_SETREGSET(NT_ARM_HW_WATCH) 设 4 字节写监视点
 * → PTRACE_CONT 全部恢复 → 后台线程 waitpid 收 SIGTRAP(TRAP_HWBKPT)
 * → 记录 {tid, pc, x0-x4} → CONT 继续（写操作在 trap 后照常执行）。
 *
 * 说明: 附加期间目标被暂停仅数毫秒；命中线程的 SIGTRAP 由本模块吞掉，
 * 目标进程无感知（不会崩）。其他信号原样转交。
 */
#define _GNU_SOURCE

#include "watch.h"
#include "disasm.h"
#include "driver.h"
#include "process.h"
#include "../shared/wire_protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NT_ARM_HW_WATCH
#define NT_ARM_HW_WATCH 0x403
#endif
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef TRAP_HWBKPT
#define TRAP_HWBKPT 4
#endif

/* user_hwdebug_state / user_pt_regs 由 <asm/ptrace.h> 提供（NDK 自带） */

/* 枚举目标进程全部 tid。返回 malloc 数组 + 数量；失败返回 NULL。 */
static pid_t *list_threads(pid_t pid, int *out_n) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
    DIR *dp = opendir(path);
    if (!dp) return NULL;
    pid_t *arr = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            pid_t *na = realloc(arr, (size_t)cap * sizeof(pid_t));
            if (!na) { free(arr); closedir(dp); return NULL; }
            arr = na;
        }
        arr[n++] = (pid_t)atoi(de->d_name);
    }
    closedir(dp);
    *out_n = n;
    return arr;
}

/* 对单线程设 4 字节硬件写监视点。线程须处于 stop 状态。失败返回 -1。 */
static int set_watchpoint(pid_t tid, uint64_t addr) {
    struct user_hwdebug_state st;
    memset(&st, 0, sizeof(st));
    st.dbg_regs[0].addr = addr & ~(uint64_t)3;
    /* DBGWCR: E(0) | LSC=write(3:2=10) | BAS=0xf(12:5, 4 字节) */
    st.dbg_regs[0].ctrl = (1ull << 0) | (2ull << 2) | (0xfull << 5);
    struct iovec io = { &st, sizeof(st) };
    if (ptrace(PTRACE_SETREGSET, tid, (void *)(long)NT_ARM_HW_WATCH, &io) != 0) {
        return -1;
    }
    return 0;
}

static void record_hit(gg_watch_t *w, pid_t tid, uint64_t pc, uint64_t addr,
                       const uint64_t *r, uint64_t fp, uint64_t sp, uint64_t lr) {
    pthread_mutex_lock(&w->mtx);
    if (w->hit_count < GG_WATCH_MAX_HITS) {
        gg_watch_hit_t *h = &w->hits[w->hit_count++];
        h->tid = tid;
        h->pc = pc;
        h->addr = addr;
        h->r0 = r[0]; h->r1 = r[1]; h->r2 = r[2]; h->r3 = r[3]; h->r4 = r[4];
        h->fp = fp;
        h->sp = sp;
        h->lr = lr;
        h->stack_depth = 0;
        h->disasm_count = 0;
        h->module[0] = 0;
    }
    pthread_mutex_unlock(&w->mtx);
}

/* 后台等待线程: waitpid 循环, 捕获硬件断点命中 */
static void *watch_loop(void *arg) {
    gg_watch_t *w = (gg_watch_t *)arg;
    for (;;) {
        if (w->stop) break;
        int st = 0;
        pid_t tid = waitpid(-1, &st, __WALL);
        if (tid < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) break;   /* 全部线程已退出/分离 */
            usleep(20000);
            continue;
        }
        if (WIFEXITED(st) || WIFSIGNALED(st)) continue;   /* 线程退出, 忽略 */
        if (!WIFSTOPPED(st)) continue;
        int sig = WSTOPSIG(st);
        if (sig == SIGTRAP) {
            siginfo_t si;
            memset(&si, 0, sizeof(si));
            int hit = 0;
            if (ptrace(PTRACE_GETSIGINFO, tid, 0, &si) == 0 &&
                si.si_code == TRAP_HWBKPT) {
                struct user_pt_regs regs;
                struct iovec io = { &regs, sizeof(regs) };
                if (ptrace(PTRACE_GETREGSET, tid, (void *)(long)NT_PRSTATUS, &io) == 0) {
                    uint64_t r[5];
                    for (int i = 0; i < 5; i++) r[i] = regs.regs[i];
                    record_hit(w, tid, regs.pc, (uint64_t)(uintptr_t)si.si_addr, r,
                               regs.regs[29], regs.sp, regs.regs[30]);
                    hit = 1;
                }
            }
            if (hit) { ptrace(PTRACE_CONT, tid, 0, 0); continue; }
            /* 非监视点 SIGTRAP（目标自身断点等）: 原样转交 */
        }
        /* 用 intptr_t 而不是 long：MinGW 是 LLP64（long 只有 32 位），
         * 转指针会被 -Wint-to-pointer-cast 警告且在高位地址上截断；目标平台 arm64 是 LP64。 */
        ptrace(PTRACE_CONT, tid, 0, (void *)(intptr_t)sig);
    }
    return NULL;
}

int watch_start(gg_watch_t *w, gg_driver_t *drv, pid_t pid, uint64_t addr,
                int type, int len) {
    memset(w, 0, sizeof(*w));
    w->pid = pid;
    w->bp_addr = addr;
    pthread_mutex_init(&w->mtx, NULL);
    if (type <= 0) type = HW_BP_TYPE_W;
    if (len <= 0) len = 4;

    /* 路径 1: QX 驱动硬件断点（0x807 add + 0x809 enable） */
    if (drv && drv->ko_type == KO_TYPE_QX && drv->dev_fd >= 0) {
        errno = 0;
        if (qx_hwbp_add(drv, pid, addr, type, len) != 0) {
            snprintf(w->last_error, sizeof(w->last_error),
                     "qx_hwbp_add(0x807 type=%d len=%d) failed: %s", type, len,
                     strerror(errno));
            return -1;
        }
        errno = 0;
        if (qx_hwbp_enable(drv, pid) != 0) {
            snprintf(w->last_error, sizeof(w->last_error),
                     "qx_hwbp_enable(0x809) failed: %s", strerror(errno));
            qx_hwbp_clear(drv, pid);
            return -1;
        }
        w->use_driver = 1;
        w->drv = drv;
        w->active = 1;
        snprintf(w->last_error, sizeof(w->last_error),
                 "driver hwbp: pid %d addr 0x%llx type=%d len=%d", (int)pid,
                 (unsigned long long)addr, type, len);
        return 0;
    }

    /* 路径 2: rwproc33 驱动硬件断点（hwBreakpointProc_module, perf_event, 无密钥门控）。
     * 驱动 read() 命令分发 + CMD 13 INST_PROCESS_HWBP_ALL: 驱动内 for_each_thread 枚举
     * 进程全部线程各注册一个断点（per-task 断点, 任务切换自动换装, 不受每 CPU WRP 4 槽限）。
     * 一次调用全覆盖, 不依赖用户态枚举线程。 */
    if (drv && drv->ko_type == KO_TYPE_RWPROC && drv->rwproc_bp_fd >= 0) {
        int nb = 0;
        if (rwproc_hwbp_start_all(drv, pid, addr, type, len,
                                  w->rwproc_bp_ids, GG_RWPROC_MAX_BPS, &nb) != 0) {
            snprintf(w->last_error, sizeof(w->last_error),
                     "rwproc_hwbp_start_all failed: %s", drv->last_error);
            return -1;
        }
        if (nb == 0) {
            snprintf(w->last_error, sizeof(w->last_error),
                     "rwproc hwbp: no threads registered (%s)", drv->last_error);
            return -1;
        }
        for (int i = 0; i < nb; i++) {
            w->rwproc_hit_consumed[i] = 0;
        }
        w->use_driver = 2;
        w->drv = drv;
        w->rwproc_bp_count = nb;
        w->ntids = nb;          /* 显示修复: rwproc33 路径也写回 ntids（原只在 ptrace 路径赋值） */
        w->active = 1;
        snprintf(w->last_error, sizeof(w->last_error),
                 "rwproc hwbp: pid %d addr 0x%llx type=%d len=%d bps=%d",
                 (int)pid, (unsigned long long)addr, type, len, nb);
        return 0;
    }

    /* 路径 2: ptrace（无 QX 驱动/驱动失败时兜底） */
    int n = 0;
    pid_t *tids = list_threads(pid, &n);
    if (!tids || n == 0) {
        snprintf(w->last_error, sizeof(w->last_error), "no threads for pid %d", (int)pid);
        free(tids);
        return -1;
    }

    /* 1) 全部附加（逐个 wait 其 stop） */
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (ptrace(PTRACE_ATTACH, tids[i], 0, 0) != 0) continue;
        int st = 0;
        if (waitpid(tids[i], &st, __WALL) < 0 || !WIFSTOPPED(st)) continue;
        ok++;
    }
    if (ok == 0) {
        snprintf(w->last_error, sizeof(w->last_error),
                 "ptrace attach failed (all %d threads)", n);
        free(tids);
        return -1;
    }

    /* 2) 设监视点（已附加线程处于 stop） */
    int watched = 0;
    int first_errno = 0;
    for (int i = 0; i < n; i++) {
        errno = 0;
        if (set_watchpoint(tids[i], addr) == 0) watched++;
        else if (!first_errno) first_errno = errno;
    }
    if (watched == 0) {
        snprintf(w->last_error, sizeof(w->last_error),
                 "NT_ARM_HW_WATCH set failed on all threads (errno=%d %s); "
                 "kernel may lack hw-breakpoint support",
                 first_errno, first_errno ? strerror(first_errno) : "?");
    }
    /* 3) 全部恢复 */
    for (int i = 0; i < n; i++) ptrace(PTRACE_CONT, tids[i], 0, 0);

    w->tids = tids;
    w->ntids = n;
    w->active = 1;
    if (pthread_create(&w->th, NULL, watch_loop, w) != 0) {
        for (int i = 0; i < n; i++) ptrace(PTRACE_DETACH, tids[i], 0, 0);
        w->active = 0;
        free(tids);
        w->tids = NULL;
        snprintf(w->last_error, sizeof(w->last_error), "pthread_create failed");
        return -1;
    }
    if (watched == 0) {
        snprintf(w->last_error, sizeof(w->last_error),
                 "NT_ARM_HW_WATCH set failed on all threads (errno=%d %s); "
                 "kernel may lack hw-breakpoint support", first_errno,
                 first_errno ? strerror(first_errno) : "?");
    } else {
        snprintf(w->last_error, sizeof(w->last_error), "watching pid %d addr 0x%llx threads=%d watched=%d",
                 (int)pid, (unsigned long long)addr, n, watched);
    }
    return 0;
}

void watch_stop(gg_watch_t *w) {
    if (!w->active) return;
    if (w->use_driver == 2) {
        /* rwproc33 路径: 卸载全部线程断点 */
        for (int i = 0; i < w->rwproc_bp_count; i++) {
            rwproc_hwbp_stop(w->drv, w->rwproc_bp_ids[i]);
        }
        w->use_driver = 0;
        w->drv = NULL;
        w->active = 0;
        w->hit_count = 0;
        w->rwproc_bp_count = 0;
        return;
    }
    if (w->use_driver) {
        qx_hwbp_clear(w->drv, w->pid);
        w->use_driver = 0;
        w->drv = NULL;
        w->active = 0;
        w->hit_count = 0;
        return;
    }
    w->stop = 1;
    /* 唤醒阻塞在 waitpid(-1) 的后台线程: 先 detach 所有 tid, 使其不再是 traced
     * child, 后台线程的 waitpid 最终返回 ECHILD → loop 退出。否则若目标不持续
     * 触发硬件断点, waitpid 不返回, loop 永远检查不到 w->stop, pthread_join 死锁。 */
    for (int i = 0; i < w->ntids; i++) ptrace(PTRACE_DETACH, w->tids[i], 0, 0);
    pthread_join(w->th, NULL);
    free(w->tids);
    w->tids = NULL;
    w->ntids = 0;
    w->active = 0;
    w->hit_count = 0;
}

/* ---------- 命中日志增强（缺口 3） ---------- */

/* FP 链调用栈回溯（ARM64: [fp]=上一帧 fp, [fp+8]=返回地址）。
 * out_pc[0] = 命中 pc；返回层数。链结束帧的返回地址仍计入。 */
static int backtrace_fp(gg_driver_t *d, pid_t pid, uint64_t fp, uint64_t pc,
                        uint64_t *out_pc, int cap) {
    int n = 0;
    uint64_t cur_pc = pc, cur_fp = fp;
    while (n < cap) {
        out_pc[n++] = cur_pc;
        if (n >= cap || cur_fp == 0) break;
        uint64_t next_fp = 0, ret = 0;
        if (driver_read(d, pid, cur_fp, &next_fp, sizeof(next_fp)) != 8) break;
        if (driver_read(d, pid, cur_fp + 8, &ret, sizeof(ret)) != 8) break;
        if (next_fp <= cur_fp) {        /* 链结束: 返回地址仍算一层 */
            if (n < cap) out_pc[n++] = ret;
            break;
        }
        cur_pc = ret;
        cur_fp = next_fp;
    }
    return n;
}

/* 对单条命中做 enrich: 模块归属 + 调用栈 + 预反汇编 pc±16 条 */
static void enrich_hit(gg_driver_t *d, pid_t pid, gg_watch_hit_t *h) {
    h->stack_depth = 0;
    h->disasm_count = 0;
    h->module[0] = 0;
    h->module_path[0] = 0;
    h->module_base = 0;
    h->module_offset = 0;

    /* 1) 模块归属 */
    char path[512];
    uint64_t base = 0, off = 0;
    if (process_resolve_addr(pid, h->pc, path, sizeof(path), &base, &off) == 0) {
        snprintf(h->module_path, sizeof(h->module_path), "%s", path);
        const char *slash = strrchr(path, '/');
        snprintf(h->module, sizeof(h->module), "%s", slash ? slash + 1 : path);
        h->module_base = base;
        h->module_offset = off;
    }

    /* 2) FP 链回溯（含命中 pc） */
    h->stack_depth = backtrace_fp(d, pid, h->fp, h->pc, h->stack_pc,
                                  GG_WATCH_STACK_MAX);

    /* 3) 预反汇编 pc±16 条（ARM64 定长 4B: pc-64 .. pc+64 共 32 条） */
    {
        uint8_t buf[128];
        uint64_t start = h->pc > 64 ? h->pc - 64 : 0;
        ssize_t got = driver_read(d, pid, start, buf, sizeof(buf));
        if (got > 0) {
            char mnem[GG_WATCH_DISASM_MAX][64];
            char ops[GG_WATCH_DISASM_MAX][128];
            char bytes[GG_WATCH_DISASM_MAX][32];
            char comment[GG_WATCH_DISASM_MAX][64];
            int n = disasm_a64(buf, (size_t)got, start, mnem, ops, bytes, comment,
                               GG_WATCH_DISASM_MAX);
            for (int i = 0; i < n; i++) {
                h->disasm[i].addr = start + (uint64_t)(i * 4);
                snprintf(h->disasm[i].bytes, sizeof(h->disasm[i].bytes), "%s", bytes[i]);
                snprintf(h->disasm[i].mnemonic, sizeof(h->disasm[i].mnemonic), "%s", mnem[i]);
                snprintf(h->disasm[i].op_str, sizeof(h->disasm[i].op_str), "%s", ops[i]);
                snprintf(h->disasm[i].comment, sizeof(h->disasm[i].comment), "%s", comment[i]);
            }
            h->disasm_count = n;
        }
    }
}

int watch_poll(gg_watch_t *w, gg_driver_t *drv, gg_watch_hit_t *out, int cap) {
    if (!w->active) return 0;
    if (w->use_driver == 2) {
        /* rwproc33 路径: 每个线程断点独立取命中; 模块命中累计不清零且只能从向量头取
         * （无分页/无清除, 上限 1024）, 一次取大窗口并只上报 [consumed, real) 的新命中。 */
        enum { RWPROC_POLL_BATCH = 512 };
        rwproc_hwbp_hit_item_t *items = malloc((size_t)RWPROC_POLL_BATCH *
                                               sizeof(rwproc_hwbp_hit_item_t));
        if (!items) return 0;
        int n = 0;
        for (int b = 0; b < w->rwproc_bp_count && n < cap; b++) {
            int real = 0;
            if (rwproc_hwbp_get_hits(w->drv, w->rwproc_bp_ids[b], items,
                                     RWPROC_POLL_BATCH, &real) != 0) continue;
            uint64_t start = w->rwproc_hit_consumed[b];
            int this_bp = 0;
            for (int i = 0; i < real && n < cap; i++) {
                if ((uint64_t)i < start) continue;   /* 已上报过 */
                out[n].tid = (pid_t)items[i].task_id;
                out[n].pc = items[i].regs_info.pc;
                out[n].addr = items[i].hit_addr;     /* 模块实现里 = 命中指令 pc */
                out[n].r0 = items[i].regs_info.regs[0];
                out[n].r1 = items[i].regs_info.regs[1];
                out[n].r2 = items[i].regs_info.regs[2];
                out[n].r3 = items[i].regs_info.regs[3];
                out[n].r4 = items[i].regs_info.regs[4];
                out[n].fp = items[i].regs_info.regs[29];
                out[n].sp = items[i].regs_info.sp;
                out[n].lr = items[i].regs_info.regs[30];
                enrich_hit(w->drv, w->pid, &out[n]);
                n++;
                this_bp++;
            }
            /* 本 bp 已取窗口标记为消费（cap 截断的剩余部分下次 poll 补齐） */
            w->rwproc_hit_consumed[b] = start + (uint64_t)this_bp;
        }
        free(items);
        return n;
    }
    if (w->use_driver) {
        qx_hwbp_hit_item_t items[64];
        int real = 0;
        if (qx_hwbp_get_hits(w->drv, w->pid, w->bp_addr, items, 64, &real) != 0) return 0;
        if (real > 64) real = 64;
        int n = real < cap ? real : cap;
        for (int i = 0; i < n; i++) {
            out[i].tid = items[i].task_id;
            out[i].pc = items[i].regs.pc;
            out[i].addr = items[i].hit_addr;
            out[i].r0 = items[i].regs.regs[0];
            out[i].r1 = items[i].regs.regs[1];
            out[i].r2 = items[i].regs.regs[2];
            out[i].r3 = items[i].regs.regs[3];
            out[i].r4 = items[i].regs.regs[4];
            out[i].fp = items[i].regs.regs[29];
            out[i].sp = items[i].regs.sp;
            out[i].lr = items[i].regs.regs[30];
            enrich_hit(w->drv, w->pid, &out[i]);
        }
        return n;
    }
    int n = 0;
    pthread_mutex_lock(&w->mtx);
    int take = w->hit_count < cap ? w->hit_count : cap;
    for (int i = 0; i < take; i++) {
        out[n] = w->hits[i];
        enrich_hit(drv, w->pid, &out[n]);
        n++;
    }
    /* cap 截断时剩余命中保留到下次 poll（与 rwproc 路径游标语义一致） */
    if (take < w->hit_count)
        memmove(w->hits, w->hits + take,
                (size_t)(w->hit_count - take) * sizeof(w->hits[0]));
    w->hit_count -= take;
    pthread_mutex_unlock(&w->mtx);
    return n;
}

int watch_inject(gg_watch_t *w, gg_driver_t *drv, pid_t pid,
                 uint64_t pc, uint64_t fp, uint64_t sp, uint64_t lr,
                 uint64_t addr, gg_watch_hit_t *out) {
    memset(out, 0, sizeof(*out));
    out->tid = (pid_t)pid;
    out->pc = pc;
    out->addr = addr;
    out->fp = fp;
    out->sp = sp;
    out->lr = lr;
    out->r0 = 0; out->r1 = 0; out->r2 = 0; out->r3 = 0; out->r4 = 0;
    enrich_hit(drv, pid, out);
    return 0;
}
