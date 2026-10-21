/* engine_internal.h — agentd 内部共享声明（main.c 拆分过渡层）
 *
 * 背景：main.c 2205 行巨型文件按域拆分（cmd_shell.c / cmd_view.c / …），
 * 但各 handler 都紧耦合 g_eng 引擎单例与 send_ok/send_err 帧 API。
 * 本头文件把这些"拆分边界上的共享物"收敛为单一声明源：
 *   - gg_engine_t 结构（会话状态单例）
 *   - g_eng / g_watch 外部链接声明（定义仍在 main.c）
 *   - 帧收发 API（read_full/send_full/send_frame/send_ok/send_err）
 *   - log_msg（driver.c 已在用）
 *
 * 拆分纪律：
 *   - 只放"确需跨翻译单元共享"的声明，不放实现（纯头文件，无静态定义）。
 *   - main.c 保留定义与所有权；外迁的 cmd_*.c 只消费。
 *   - check_kernel_only.py / check_dispatch_consistency.py 护栏不受影响
 *     （dispatch 仍在 main.c，本头文件不引入用户态回退路径）。
 */
#ifndef GG_ENGINE_INTERNAL_H
#define GG_ENGINE_INTERNAL_H

#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>

#include "third_party/cJSON.h"
#include "driver.h"
#include "freeze.h"
#include "search.h"
#include "watch.h"
#include "../shared/wire_protocol.h"

/* ---------------- 引擎单例（定义在 main.c） ---------------- */

typedef struct gg_engine {
    gg_driver_t  drv;
    gg_freeze_t  freeze;
    pid_t        session_pid;      /* -1 = 未附加 */
    char         session_name[256];
    gg_results_t results;
    size_t       result_cap;
    int          safe_mode;        /* 只读会话 */
    int          shell_enabled;
    pthread_mutex_t mtx;           /* 串行化所有 handler */
    FILE        *logf;
} gg_engine_t;

extern gg_engine_t g_eng;
extern gg_watch_t  g_watch;      /* 硬件断点监视（CE 式"什么访问了这个地址"） */

/* ---------------- 帧收发（实现位于 main.c） ---------------- */

int  read_full(int fd, void *buf, size_t len);
int  send_full(int fd, const void *buf, size_t len);
int  send_frame(int fd, uint16_t cmd, uint32_t seq, uint32_t flags,
                char *payload, size_t plen);
void send_ok(int fd, uint16_t cmd, uint32_t seq, cJSON *res);
void send_err(int fd, uint16_t cmd, uint32_t seq, int code, const char *msg);

/* ---------------- 日志（实现在 main.c，driver.c 已在用） ---------------- */

void log_msg(const char *fmt, ...);

/* ---------------- 参数解析 helper（实现位于 main.c） ---------------- */

/* 严格解析地址字符串（"0x..." / 十进制）。返回 1 成功、0 非法；out 可为 NULL（只校验）。
 *
 * 为什么需要：agentd 里原本有 17 处裸 strtoull(s, NULL, 0)，非法输入一律得到 0，
 * 然后拿着地址 0 去读内存。调用方看到的错误是"读失败"，而不是"地址写错了" ——
 * 排查成本极高。dispatch 在分发前统一用本函数校验 addr/base/pc/fp/sp/lr。 */
int parse_u64_strict(const char *s, uint64_t *out);

/* force 布尔参数解析（只接受 0/1）。实现在 main.c。 */
int parse_force(const cJSON *p);

#endif /* GG_ENGINE_INTERNAL_H */