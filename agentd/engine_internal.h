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
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "third_party/cJSON.h"
#include "driver.h"
#include "freeze.h"
#include "search.h"
#include "watch.h"
#include "../shared/wire_protocol.h"
#include "last_resp.h"

/* ---------------- 引擎单例（定义在 main.c） ---------------- */

typedef struct gg_engine {
    gg_driver_t  drv;
    gg_freeze_t  freeze;
    pid_t        session_pid;      /* -1 = 未附加 */
    /* attach 时目标的 starttime。每个请求分发前比对，用来识别 pid 复用：
     * 目标退出后 pid 会被回收再分配，session_pid 仍"有效"却已指向别的进程。 */
    uint64_t     session_starttime;
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

/* ---------------- 帧收发 ----------------
 * read_full / send_full 在 main.c（读写原始字节，与 socket 生命周期绑定）；
 * send_frame / send_ok / send_err 已搬到 wire_io.c（2026-10-23）。 */

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

/* ---------------- 参数解析 helper（跨域共用，实现仍在 main.c） ----------------
 *
 * 这几个原本是 main.c 里的 static，cmd_search.c 拆出去后必须共享：
 *   parse_vt        值类型（数字 1-7 或 byte/word/dword/... 字符串），被搜索、
 *                   读、写、冻结、指针扫描五个域用到，是跨域最广的一个。
 *   j_scalar_str    cJSON 标量（数字/字符串/布尔）→ 字符串。写类命令的 value
 *                   既可能是字符串也可能是数字，必须统一入口，否则各处各写一套。
 *   parse_value_str 值字符串 → u64（带 endptr 校验）。
 *   j_addr_u64      地址节点 → u64，同时吃 "0x..." 字符串与 JSON 数字。
 *                   数组元素里的地址走不到 dispatch 的顶层地址闸门，只能靠它。 */
int parse_vt(const cJSON *p, int def);
const char *j_scalar_str(cJSON *node, char *buf, size_t bufsz);
uint64_t parse_value_str(int vtype, const char *s, int *ok);
int j_addr_u64(cJSON *node, uint64_t *out);

/* type/region 无效值的统一报错文案（搜索/读/写/冻结共用，避免各处措辞漂移） */
#define VT_HINT "type: 1-7 或 byte/word/dword/qword/float/double/bytes"
#define REGION_HINT "region: 1-3 或 anonymous/writable/all"

/* ---------------- 事件广播（实现在 main.c，cmd_*.c 域内要用） ---------------- */

void event_search_update(long prev_count, long cur_count);
void event_results_update(void);

/* ---------------- 外迁命令 handler（实现见各自的 cmd_*.c） ---------------- */

/* cmd_search.c — 搜索与结果集 */
void h_search(int fd, uint32_t seq, cJSON *p);
void h_search_range(int fd, uint32_t seq, cJSON *p);
void h_unknown_init(int fd, uint32_t seq, cJSON *p);
void h_refine(int fd, uint32_t seq, cJSON *p);
void h_result_count(int fd, uint32_t seq, cJSON *p);
void h_get_results(int fd, uint32_t seq, cJSON *p);
void h_clear_results(int fd, uint32_t seq, cJSON *p);
void h_set_results(int fd, uint32_t seq, cJSON *p);

/* cmd_mem.c — 内存读写 / 冻结 / 暂停恢复 */
void h_read(int fd, uint32_t seq, cJSON *p);
void h_read_bytes(int fd, uint16_t cmd, uint32_t seq, cJSON *p);  /* READ_BYTES 与 HEX_DUMP 共用 */
void h_write(int fd, uint32_t seq, cJSON *p);
void h_freeze(int fd, uint32_t seq, cJSON *p);
void h_unfreeze(int fd, uint32_t seq, cJSON *p);
void h_pause_resume(int fd, uint32_t seq, cJSON *p, int pause);

/* cmd_view.c — 内存视图 / 符号解析 / 指针链
 * （这三条本来以“前置声明”形式写在 main.c 的 h_kernel_status 之后，
 *   Phase 2 拆分时随区间一起被删过一次，现在收到这里统一管理。） */
void h_view_memory(int fd, uint32_t seq, cJSON *p);
void h_resolve_symbol(int fd, uint32_t seq, cJSON *p);
void h_pointer_chain(int fd, uint32_t seq, cJSON *p);

/* cmd_shell.c — 安全 shell（白名单 argv 直执行，不经 sh）
 * 实现里自己去解析 argv 白名单，dispatch 只负责鉴权/降级判断。
 * 注意：这条只在 g_eng.shell_enabled 时才会被 dispatch 放行。 */
void h_shell(int fd, uint32_t seq, cJSON *p);


/* ---------------- 出站帧（实现在 wire_io.c） ---------------- */

/* 出站帧全局串行锁：事件广播与各连接线程共用。定义在 wire_io.c，
 * events.c（Phase 1·② 之后）直接用它往订阅者发事件。 */
extern pthread_mutex_t g_send_mtx;

/* 分片发送：客户端没声明 frag 能力时不要调（旧客户端看不懂 GGWF_MORE）。 */
int  send_frame_fragmented(int fd, uint16_t cmd, uint32_t seq, uint32_t flags,
                           const char *payload, size_t plen);

/* ---------------- 响应存档（定义在 main.c） ---------------- */

/* 线程局部：每连接线程独立，快速通道（无锁 handler）与锁内引擎操作不共享
 * 同一 buffer。send_ok/send_err 写，event_cmd_end 读。 */
extern _Thread_local gg_last_resp_t g_last_resp;

/* 客户端是否声明过能重组分片（AUTH 带 "frag":1）。
 * 定义在 main.c 的事件区（Phase 1·② 之后搬到 events.c）。 */
int client_supports_frag(int fd);


/* ---------------- 事件系统（实现在 events.c） ---------------- */

/* 客户端注册表。register/unregister 由 conn_thread（main.c）调；
 * 其余由 dispatch 和 cmd_status.c 调。 */
void clients_register(int fd);
void clients_unregister(int fd);
void clients_set_frag(int fd, int on);
void clients_set_subscribed(int fd, int on);
void clients_drop(int fd);
void client_set_send_timeout(int fd);

/* 事件广播。broadcast_event 内部是"锁内 dup 列表 → 锁外发送"，
 * 改之前先读 events.c 顶部注释。 */
void broadcast_event(cJSON *ev);
void event_simple(const char *type, cJSON *data);
void event_cmd_start(uint16_t cmd, cJSON *p);
void event_cmd_end(uint16_t cmd, uint64_t t0);
void event_session_change(const char *state, pid_t pid, const char *name);
void event_watch_hit(const gg_watch_hit_t *h);

/* cmd 码 -> 命令名（dispatch 日志用） */
const char *cmd_name(uint16_t cmd);

/* 把一次断点命中转成事件 JSON（h_watch 也会直接用它构造响应） */
cJSON *watch_hit_json(const gg_watch_hit_t *h);

/* 单调毫秒时钟（事件时间戳 / dispatch 计时） */
uint64_t now_ms(void);


/* ---------------- wire 鉴权（实现在 wire_auth.c） ---------------- */

/* 临时密钥落盘位置与长度。main() 启动时打印这个路径，所以要放在头里。 */
#define GG_EPHEMERAL_KEY_PATH "/data/adb/GG-Ghost/wire.key.ephemeral"
#define GG_WIRE_KEY_BYTES     16   /* 128 位，hex 后 32 字符 */

/* 共享密钥（hex）。定义在 wire_auth.c，main() 启动时写入。 */
extern char g_wire_key[65];
/* 1 = 本次用的是自动生成的临时 key（会出现在 AUTH 响应和启动日志里）。 */
extern int  g_wire_key_ephemeral;

int  read_key_file(const char *path, char *out, size_t outsz);
int  load_or_create_ephemeral_key(char *out, size_t outsz);

/* 握手：成功后把 *authed 置 1，dispatch 才放行其余命令。 */
void h_auth(int fd, uint32_t seq, cJSON *p, int *authed);


/* ---------------- 进程/会话域（实现在 cmd_proc.c） ---------------- */

/* session_guard 由 dispatch 在 g_eng.mtx 锁内、switch 之前调用；
 * session_drop 由 session_guard 与 attach/detach 调用。
 * 调用顺序见 cmd_proc.c 顶部注释。 */
pid_t resolve_pid(cJSON *p);
int   session_guard(void);
void  session_drop(const char *reason);

void h_ping(int fd, uint32_t seq, cJSON *p);
void h_self_test(int fd, uint32_t seq, cJSON *p);
void h_list_processes(int fd, uint32_t seq, cJSON *p);
void h_attach(int fd, uint32_t seq, cJSON *p);
void h_detach(int fd, uint32_t seq, cJSON *p);
void h_target_info(int fd, uint32_t seq, cJSON *p);
void h_module_list(int fd, uint32_t seq, cJSON *p);


/* ---------------- 分析域（实现在 cmd_analyze.c） ---------------- */

/* hwbp 运行时开关：定义在 main.c（main() 的 --hwbp 参数解析写），
 * h_watch 读。g_hwbp_probe: -1 未探测 / 0 不安全 / 1 可用。
 * 探测有副作用（会往断点槽注册），一次进程生命周期只探一次，别每请求重探。 */
extern int g_hwbp_allowed;
extern int g_hwbp_probe;
extern int g_hwbp_probe_rc;

void h_disassemble(int fd, uint32_t seq, cJSON *p);
void h_watch(int fd, uint32_t seq, cJSON *p);
void h_module_base(int fd, uint32_t seq, cJSON *p);
void h_calc_offset(int fd, uint32_t seq, cJSON *p);
void h_pointer_scan(int fd, uint32_t seq, cJSON *p);


/* ---------------- 虚拟触摸（实现在 cmd_touch.c） ---------------- */

/* 不依赖 attach：触摸是系统级输入，走 driver 的内核注入通道。 */
void h_ghost_tap(int fd, uint32_t seq, cJSON *p);
void h_ghost_swipe(int fd, uint32_t seq, cJSON *p);

/* ---------------- 状态域（实现在 cmd_status.c） ---------------- */

/* h_kernel_status / h_device_info 走无锁快速通道（见 cmd_status.c 顶部说明）。 */
void h_kernel_status(int fd, uint32_t seq, cJSON *p);
void h_device_info(int fd, uint32_t seq, cJSON *p);
void h_subscribe(int fd, uint32_t seq, cJSON *p, int on);
void h_relay_event(int fd, uint32_t seq, cJSON *p);

#endif /* GG_ENGINE_INTERNAL_H */