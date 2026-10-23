/* cmd_status.c — 状态域（自 main.c 拆出，2026-10-23，Phase 2·⑦）
 *
 * 域内命令：GGCMD_KERNEL_STATUS / DEVICE_INFO（SUBSCRIBE/RELAY_EVENT 随事件广播删除）
 *
 * **前两个走"快速通道"（无锁执行）**，这是刻意的：大进程搜索期间
 * g_eng.mtx 被长期持有，如果 PING/KERNEL_STATUS 也要排队，探活就会超时，
 * 从外部看像"agentd 挂了"。快速通道用原子读快照，容忍字符串短暂撕裂
 * （JSON 仍合法），换来的是任何时候都能响应。
 *
 * 改这几个 handler 时注意：
 *   - 新增字段如果要跨字段保持一致（比如 total = used + free），快速通道做不到，
 *     要么放弃一致性（标注为"近似值"），要么把命令挪到锁内（会影响探活）。
 *   - SNAP 宏只做 strncpy + 保证 NUL 结尾，不要往里加 strlen 之类的重活。
 */
#include "engine_internal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* getuid —— device_info 里判断是否 root */

void h_kernel_status(int fd, uint32_t seq, cJSON *p) {
    cJSON *r = cJSON_CreateObject();
    gg_driver_t *d = &g_eng.drv;
    /* X3: 本 handler 经快速通道（无锁）执行, 与锁内引擎操作（搜索/attach 等）
     * 并发 —— 整型字段原子读（ARM64 对齐原子读写零额外开销）,
     * 字符串快照容忍撕裂（仅内容短暂不完整, JSON 仍合法）。 */
    int st = __atomic_load_n(&d->state, __ATOMIC_RELAXED);
    uint64_t reads  = atomic_load_explicit(&d->stat_reads, memory_order_relaxed);
    uint64_t writes = atomic_load_explicit(&d->stat_writes, memory_order_relaxed);
    uint64_t falls  = atomic_load_explicit(&d->stat_fallbacks, memory_order_relaxed);
    int ko_loaded = __atomic_load_n(&d->ko_loaded, __ATOMIC_RELAXED);
    int frozen    = freeze_count(&g_eng.freeze);
    int safe      = __atomic_load_n(&g_eng.safe_mode, __ATOMIC_RELAXED);
    int shell     = __atomic_load_n(&g_eng.shell_enabled, __ATOMIC_RELAXED);
    int s_pid     = __atomic_load_n(&g_eng.session_pid, __ATOMIC_RELAXED);
    char err[256], ko_mod[256], ko_file[512], ko_ver[64], s_name[256];
#define SNAP(dst, src) do { \
        strncpy((dst), (src), sizeof(dst) - 1); \
        (dst)[sizeof(dst) - 1] = 0; \
    } while (0)
    SNAP(err, d->last_error);   SNAP(ko_mod, d->ko_module);
    SNAP(ko_file, d->ko_file);  SNAP(ko_ver, d->ko_version);
    SNAP(s_name, g_eng.session_name);
#undef SNAP
    int w_active = __atomic_load_n(&g_watch.active, __ATOMIC_RELAXED);
    int w_pid    = __atomic_load_n(&g_watch.pid, __ATOMIC_RELAXED);
    int w_hits   = __atomic_load_n(&g_watch.hit_count, __ATOMIC_RELAXED);
    int w_mode   = __atomic_load_n(&g_watch.use_driver, __ATOMIC_RELAXED);

    cJSON_AddNumberToObject(r, "state", st);
    cJSON_AddStringToObject(r, "state_str", driver_state_str(st));
    cJSON_AddNumberToObject(r, "reads", (double)reads);
    cJSON_AddNumberToObject(r, "writes", (double)writes);
    cJSON_AddNumberToObject(r, "fallbacks", (double)falls);
    cJSON_AddStringToObject(r, "error", err);
    cJSON_AddBoolToObject(r, "ko_loaded", ko_loaded ? 1 : 0);
    cJSON_AddStringToObject(r, "ko_module", ko_mod);
    cJSON_AddStringToObject(r, "ko_file", ko_file);
    cJSON_AddStringToObject(r, "ko_version", ko_ver);
    cJSON_AddNumberToObject(r, "frozen", (double)frozen);
    cJSON_AddBoolToObject(r, "safe_mode", safe ? 1 : 0);
    /* ---- O2 可观测性扩展：只读暴露已有字段, 零行为变化 ----
     * 这些字段经 gg_status 的 kernel 子对象透传到 PC 端,
     * 无需改 PC 端 local_status()（它整对象透传 ks）。 */
    cJSON_AddBoolToObject(r, "shell_enabled", shell ? 1 : 0);
    cJSON_AddNumberToObject(r, "session_pid", (double)s_pid);
    cJSON_AddStringToObject(r, "session_name", s_name);
    cJSON_AddBoolToObject(r, "watch_active", w_active ? 1 : 0);
    cJSON_AddNumberToObject(r, "watch_pid", (double)w_pid);
    cJSON_AddNumberToObject(r, "watch_hits", (double)w_hits);
    /* 0=ptrace 1=QX驱动 2=rwproc33驱动（见 watch.h use_driver 注释） */
    cJSON_AddNumberToObject(r, "watch_mode", (double)w_mode);
    send_ok(fd, GGCMD_KERNEL_STATUS, seq, r);
    cJSON_Delete(r);
}

/* cmd_view.c（拆分外迁） */
void h_view_memory(int fd, uint32_t seq, cJSON *p);
void h_resolve_symbol(int fd, uint32_t seq, cJSON *p);
void h_pointer_chain(int fd, uint32_t seq, cJSON *p);

/* ---------------- 安全 shell（白名单 argv 直执行, 不经 sh, 防注入） ---------------- */
/* shell_split_argv / shell_argv_allowed 见 shell_util.h（与单元测试共享单一事实源）。 */

void h_device_info(int fd, uint32_t seq, cJSON *p) {
    cJSON *r = cJSON_CreateObject();
    /* X3: 快速通道无锁执行, state 原子读（attach/self_test 锁内写） */
    int st = __atomic_load_n(&g_eng.drv.state, __ATOMIC_RELAXED);
    cJSON_AddStringToObject(r, "agentd", "1.0.0");
    cJSON_AddStringToObject(r, "arch", "arm64");
    cJSON_AddNumberToObject(r, "root", getuid() == 0 ? 1 : 0);
    cJSON_AddNumberToObject(r, "driver_state", st);
    cJSON_AddStringToObject(r, "driver_state_str", driver_state_str(st));
    send_ok(fd, GGCMD_DEVICE_INFO, seq, r);
    cJSON_Delete(r);
}

/* cmd_shell.c（拆分外迁） */
void h_shell(int fd, uint32_t seq, cJSON *p);


