/* main.c — 设备端内核守护进程
 *
 * 监听 127.0.0.1:<port>，经 adb forward 供 PC 端 gg-mcp-server 调用。
 * 协议见 shared/wire_protocol.h（20 字节二进制头 + JSON 负载）。
 *
 * 用法: [--port 34127] [--driver ioctl|supercall|auto]
 *                [--ioctl-read 0x7e1a] [--ioctl-write 0x7e1b]
 *                [--supercall 45] [--log /path/log] [--safe]
 */
#include "../shared/wire_protocol.h"
#include "disasm.h"
#include "driver.h"
#include "elfsym.h"
#include "freeze.h"
#include "loader.h"
#include "process.h"
#include "search.h"
#include "version.h"
#include "watch.h"
#include "shell_util.h"   // 安全 shell 纯逻辑 helper（与单元测试共享单一事实源）
#include "engine_internal.h"  // 拆分过渡层：引擎单例/帧API 跨文件共享声明

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "third_party/cJSON.h"   /* 编译时 -I<repo根> 提供 */

/* ---------------- 会话状态 ---------------- */

/* gg_engine_t 定义与 g_eng/g_watch 声明已上移 engine_internal.h（拆分过渡层），
 * main.c 保留唯一定义所有权。 */
gg_engine_t g_eng;
gg_watch_t  g_watch;   /* 硬件断点监视（CE 式"什么访问了这个地址"） */

/* 最近一次响应存档（send_ok/send_err 写入，dispatch 的 cmd_end 事件使用）。
 * X3: 改为线程局部——每连接线程独立，快速通道（PING/KERNEL_STATUS 等无锁
 * 执行）与锁内引擎操作不再共享同一 buffer，消除跨线程数据竞争。 */
static _Thread_local char g_last_resp[8192];
static _Thread_local int  g_last_resp_ok = 0;
static _Thread_local int  g_last_resp_set = 0;

void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_eng.logf) {
        vfprintf(g_eng.logf, fmt, ap);
        fprintf(g_eng.logf, "\n");
        fflush(g_eng.logf);
    }
    va_end(ap);
}

/* ---------------- 帧收发 ---------------- */

int read_full(int fd, void *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, (char *)buf + off, len - off, 0);
        if (n < 0 && errno == EINTR) continue;   /* 信号打断: 重试（对齐 X1, agentd 端补齐） */
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

int send_full(int fd, const void *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, (const char *)buf + off, len - off, 0);
        if (n < 0 && errno == EINTR) continue;   /* 信号打断: 重试 */
        if (n <= 0) {
            log_msg("send failed fd=%d off=%zu len=%zu err=%d %s", fd, off, len,
                    errno, strerror(errno));
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int send_frame(int fd, uint16_t cmd, uint32_t seq, uint32_t flags,
                      char *payload, size_t plen) {
    gg_wire_header_t h;
    h.magic = GGWM_MAGIC;
    h.ver = GGWM_VERSION;
    h.cmd = cmd;
    h.seq = seq;
    /* 负载混淆: 滚动 XOR（防被动嗅探一眼看穿）, 置 GGWF_ENC 让对端解密。
     * payload 原地改写（调用方均为 malloc 缓冲: send_ok/send_err/broadcast_event）。 */
    if (plen && payload) {
        gg_xor_payload((unsigned char *)payload, plen);
        flags |= GGWF_ENC;
    }
    h.flags = flags;
    h.payload_len = (uint32_t)plen;
    if (send_full(fd, &h, sizeof(h)) < 0) return -1;
    if (plen && send_full(fd, payload, plen) < 0) return -1;
    return 0;
}

void send_ok(int fd, uint16_t cmd, uint32_t seq, cJSON *res) {
    char *s = cJSON_PrintUnformatted(res);
    g_last_resp_ok = 1;
    g_last_resp_set = 1;
    snprintf(g_last_resp, sizeof(g_last_resp), "%s", s ? s : "{}");
    send_frame(fd, cmd, seq, GGWF_RESP, s, strlen(s));
    free(s);
}

void send_err(int fd, uint16_t cmd, uint32_t seq, int code, const char *msg) {
    cJSON *root = cJSON_CreateObject();
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg ? msg : "error");
    cJSON_AddItemToObject(root, GG_J_ERR, e);
    char *s = cJSON_PrintUnformatted(root);
    g_last_resp_ok = 0;
    g_last_resp_set = 1;
    snprintf(g_last_resp, sizeof(g_last_resp), "%s", s ? s : "{}");
    send_frame(fd, cmd, seq, GGWF_RESP | GGWF_ERR, s, strlen(s));
    free(s);
    cJSON_Delete(root);
}

/* ---------------- 事件广播（手机悬浮窗实时监控） ----------------
 *
 * 订阅制：客户端需先发 GGCMD_SUBSCRIBE_EVENTS 才会收到 GGWF_EVENT 帧。
 * PC 端 gg-mcp-server 是严格请求-响应，不订阅则行为完全不变；
 * 手机悬浮窗订阅后收到 {cmd_start, cmd_end, watch_hit, search_update,
 * session_change, annotate(PC 中继)} 事件流。
 * 广播 best-effort：发送失败仅从注册表移除（不 close，连接线程自行收尾），
 * 不影响请求方拿结果。
 */

typedef struct gg_client {
    int fd;
    int subscribed;              /* 是否订阅事件流 */
    struct gg_client *next;
} gg_client_t;

static gg_client_t *g_clients = NULL;
static pthread_mutex_t g_clients_mtx = PTHREAD_MUTEX_INITIALIZER;

static void clients_register(int fd) {
    pthread_mutex_lock(&g_clients_mtx);
    gg_client_t *c = malloc(sizeof(*c));
    if (c) {
        c->fd = fd;
        c->subscribed = 0;
        c->next = g_clients;
        g_clients = c;
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

static void clients_unregister(int fd) {
    pthread_mutex_lock(&g_clients_mtx);
    gg_client_t **pp = &g_clients;
    while (*pp) {
        gg_client_t *c = *pp;
        if (c->fd == fd) {
            *pp = c->next;
            free(c);
            break;
        }
        pp = &c->next;
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

static void clients_set_subscribed(int fd, int on) {
    pthread_mutex_lock(&g_clients_mtx);
    gg_client_t *c = g_clients;
    while (c && c->fd != fd) c = c->next;
    if (c) {
        c->subscribed = on;
    } else if (on) {
        /* 广播失败曾移除注册表项，但 fd 仍有效：重新登记 */
        c = malloc(sizeof(*c));
        if (c) {
            c->fd = fd;
            c->subscribed = 1;
            c->next = g_clients;
            g_clients = c;
        }
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 广播一条事件。ev 所有权移交（本函数负责 cJSON_Delete）。 */
static void broadcast_event(cJSON *ev) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, GG_J_EVT, ev);
    char *s = cJSON_PrintUnformatted(root);
    size_t len = s ? strlen(s) : 0;
    if (len) {
        pthread_mutex_lock(&g_clients_mtx);
        gg_client_t **pp = &g_clients;
        while (*pp) {
            gg_client_t *c = *pp;
            if (!c->subscribed) { pp = &c->next; continue; }
            if (send_frame(c->fd, 0, 0, GGWF_EVENT, s, len) < 0) {
                /* best-effort：发送失败移除注册表项（fd 由连接线程关闭） */
                *pp = c->next;
                free(c);
                continue;
            }
            pp = &c->next;
        }
        pthread_mutex_unlock(&g_clients_mtx);
    }
    free(s);
    cJSON_Delete(root);
}

/* 事件构造小助手 */
static void event_simple(const char *type, cJSON *data) {
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", type);
    cJSON_AddNumberToObject(ev, "ts", (double)now_ms());
    cJSON_AddItemToObject(ev, "data", data ? data : cJSON_CreateObject());
    broadcast_event(ev);
}

/* cmd 码 → MCP 风格命令名（日志页展示） */
static const char *cmd_name(uint16_t cmd) {
    switch (cmd) {
    case GGCMD_PING:               return "gg_ping";
    case GGCMD_SELF_TEST:          return "gg_self_test";
    case GGCMD_LIST_PROCESSES:     return "gg_list_processes";
    case GGCMD_ATTACH:             return "gg_attach";
    case GGCMD_DETACH:             return "gg_detach";
    case GGCMD_TARGET_INFO:        return "gg_target_info";
    case GGCMD_MODULE_LIST:        return "gg_module_list";
    case GGCMD_SEARCH:             return "gg_search";
    case GGCMD_SEARCH_UNKNOWN_INIT:return "gg_search_unknown_init";
    case GGCMD_SEARCH_REFINE:      return "gg_search_refine";
    case GGCMD_SEARCH_RANGE:       return "gg_search_range";
    case GGCMD_RESULT_COUNT:       return "gg_result_count";
    case GGCMD_GET_RESULTS:        return "gg_get_results";
    case GGCMD_CLEAR_RESULTS:      return "gg_clear_results";
    case GGCMD_READ:               return "gg_read";
    case GGCMD_READ_BYTES:         return "gg_read_bytes";
    case GGCMD_HEX_DUMP:           return "gg_hex_dump";
    case GGCMD_WRITE:              return "gg_write";
    case GGCMD_SET_RESULTS:        return "gg_set_results";
    case GGCMD_FREEZE:             return "gg_freeze";
    case GGCMD_UNFREEZE:           return "gg_unfreeze";
    case GGCMD_MODULE_BASE:        return "gg_module_base";
    case GGCMD_CALC_OFFSET:        return "gg_calc_offset";
    case GGCMD_POINTER_SCAN:       return "gg_pointer_scan";
    case GGCMD_PAUSE:              return "gg_pause";
    case GGCMD_RESUME:             return "gg_resume";
    case GGCMD_KERNEL_STATUS:      return "gg_kernel_status";
    case GGCMD_LOAD_GG:            return "gg_kernel_load_gg";
    case GGCMD_CORE_VERSIONS:      return "gg_core_versions";
    case GGCMD_SHELL:              return "gg_shell";
    case GGCMD_DEVICE_INFO:        return "gg_device_info";
    case GGCMD_WATCH:              return "gg_watch";
    case GGCMD_DISASSEMBLE:        return "gg_disassemble";
    case GGCMD_VIEW_MEMORY:        return "gg_view_memory";
    case GGCMD_RESOLVE_SYMBOL:     return "gg_resolve_symbol";
    case GGCMD_POINTER_CHAIN:      return "gg_pointer_chain";
    case GGCMD_SUBSCRIBE_EVENTS:   return "gg_subscribe_events";
    case GGCMD_UNSUBSCRIBE_EVENTS: return "gg_unsubscribe_events";
    case GGCMD_RELAY_EVENT:        return "gg_relay_event";
    case GGCMD_GHOST_TAP:          return "gg_ghost_tap";
    case GGCMD_GHOST_SWIPE:        return "gg_ghost_swipe";
    default:                       return "gg_unknown";
    }
}

/* cmd_start: 请求进入（参数快照，>4KB 截断为 {"truncated":true}） */
static void event_cmd_start(uint16_t cmd, cJSON *p) {
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "cmd_start");
    cJSON_AddNumberToObject(ev, "ts", (double)now_ms());
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "cmd", cmd_name(cmd));
    cJSON *params = NULL;
    if (p) {
        char *ps = cJSON_PrintUnformatted(p);
        if (ps) {
            if (strlen(ps) > 4096) {
                params = cJSON_CreateObject();
                cJSON_AddBoolToObject(params, "truncated", 1);
            } else {
                params = cJSON_Parse(ps);
            }
            free(ps);
        }
    }
    cJSON_AddItemToObject(data, "params", params ? params : cJSON_CreateObject());
    cJSON_AddItemToObject(ev, "data", data);
    broadcast_event(ev);
}

/* cmd_end: 请求完成（结果摘要 = 最近一次 send_ok/send_err 的负载） */
static void event_cmd_end(uint16_t cmd, uint64_t t0) {
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "cmd_end");
    cJSON_AddNumberToObject(ev, "ts", (double)now_ms());
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "cmd", cmd_name(cmd));
    cJSON_AddBoolToObject(data, "success", g_last_resp_set ? (g_last_resp_ok ? 1 : 0) : 0);
    if (g_last_resp_set) {
        cJSON *res = cJSON_Parse(g_last_resp);
        if (res) cJSON_AddItemToObject(data, "result", res);
    }
    cJSON_AddNumberToObject(data, "duration_ms", (double)(now_ms() - t0));
    cJSON_AddItemToObject(ev, "data", data);
    broadcast_event(ev);
}

/* session_change: attach/detach */
static void event_session_change(const char *state, pid_t pid, const char *name) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "state", state);
    cJSON_AddNumberToObject(d, "pid", pid);
    cJSON_AddStringToObject(d, "name", name ? name : "");
    event_simple("session_change", d);
}

/* search_update: 搜索结果数变化 */
static void event_search_update(long prev_count, long cur_count) {
    if (prev_count == cur_count) return;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "count", cur_count);
    cJSON_AddNumberToObject(d, "prev_count", prev_count);
    event_simple("search_update", d);
}

/* results_update: AI 搜索/refine/冻结后向订阅端(手机悬浮窗)同步结果快照。
 * 带前 RESULTS_SYNC_MAX 条（冻结项优先），app 搜索页据此实时渲染，
 * 与 PC 端 AI 看到的数据保持一致（GG 式"数据同步"）。 */
#define RESULTS_SYNC_MAX 100
static void event_results_update(void) {
    gg_results_t *r = &g_eng.results;
    size_t n = r->count;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "count", (double)n);
    cJSON_AddNumberToObject(d, "frozen_count", (double)freeze_count(&g_eng.freeze));
    cJSON *arr = cJSON_AddArrayToObject(d, "results");
    if (n > 0 && r->addrs) {
        /* 冻结项优先 */
        int emitted = 0;
        for (size_t pass = 0; pass < 2 && emitted < RESULTS_SYNC_MAX; pass++) {
            for (size_t i = 0; i < n && emitted < RESULTS_SYNC_MAX; i++) {
                int frozen = 0;
                gg_freeze_entry_t *fe = g_eng.freeze.entries;
                for (size_t k = 0; k < g_eng.freeze.count; k++) {
                    if (fe[k].active && fe[k].addr == r->addrs[i]) { frozen = 1; break; }
                }
                if (pass == 0 && !frozen) continue;   /* 第一遍只发冻结项 */
                if (pass == 1 && frozen) continue;    /* 第二遍发其余 */
                cJSON *o = cJSON_CreateObject();
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)r->addrs[i]);
                cJSON_AddStringToObject(o, "addr", tmp);
                cJSON_AddNumberToObject(o, "index", (double)i);
                /* 当前值: 逐地址读（仅前 50 条, 避免事件过大） */
                uint64_t v = 0;
                if (emitted < 50 && g_eng.session_pid > 0) {
                    uint8_t buf[8] = {0};
                    int sz = vt_size(r->exact_vt > 0 ? r->exact_vt : VT_DWORD);
                    if (driver_read(&g_eng.drv, g_eng.session_pid, r->addrs[i], buf, (size_t)sz) == sz) {
                        v = read_bits(buf, r->exact_vt > 0 ? r->exact_vt : VT_DWORD);
                    }
                }
                cJSON_AddNumberToObject(o, "value", (double)v);
                snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)v);
                cJSON_AddStringToObject(o, "value_hex", tmp);
                cJSON_AddBoolToObject(o, "frozen", frozen ? 1 : 0);
                cJSON_AddItemToArray(arr, o);
                emitted++;
            }
        }
    }
    event_simple("results_update", d);
}

/* 断点命中 → JSON（响应数组元素与 watch_hit 事件共用） */
static cJSON *watch_hit_json(const gg_watch_hit_t *h) {
    cJSON *o = cJSON_CreateObject();
    char tmp[32];
    cJSON_AddNumberToObject(o, "tid", h->tid);
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->pc);
    cJSON_AddStringToObject(o, "pc", tmp);
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->addr);
    cJSON_AddStringToObject(o, "addr", tmp);
    char rx[5][32];
    for (int k = 0; k < 5; k++) snprintf(rx[k], sizeof(rx[k]), "0x%llx",
        (unsigned long long)(k == 0 ? h->r0 : k == 1 ? h->r1 :
         k == 2 ? h->r2 : k == 3 ? h->r3 : h->r4));
    cJSON_AddStringToObject(o, "r0", rx[0]);
    cJSON_AddStringToObject(o, "r1", rx[1]);
    cJSON_AddStringToObject(o, "r2", rx[2]);
    cJSON_AddStringToObject(o, "r3", rx[3]);
    cJSON_AddStringToObject(o, "r4", rx[4]);
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->fp);
    cJSON_AddStringToObject(o, "fp", tmp);
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->sp);
    cJSON_AddStringToObject(o, "sp", tmp);
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->lr);
    cJSON_AddStringToObject(o, "lr", tmp);
    if (h->module[0]) {
        cJSON_AddStringToObject(o, "module", h->module);
        cJSON_AddStringToObject(o, "module_path", h->module_path);
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->module_base);
        cJSON_AddStringToObject(o, "module_base", tmp);
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->module_offset);
        cJSON_AddStringToObject(o, "module_offset", tmp);
    }
    cJSON_AddNumberToObject(o, "stack_depth", h->stack_depth);
    cJSON *stk = cJSON_AddArrayToObject(o, "stack");
    for (int s = 0; s < h->stack_depth; s++) {
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->stack_pc[s]);
        cJSON_AddItemToArray(stk, cJSON_CreateString(tmp));
    }
    cJSON_AddNumberToObject(o, "disasm_count", h->disasm_count);
    cJSON *dis = cJSON_AddArrayToObject(o, "disasm");
    for (int k = 0; k < h->disasm_count; k++) {
        cJSON *di = cJSON_CreateObject();
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)h->disasm[k].addr);
        cJSON_AddStringToObject(di, "addr", tmp);
        cJSON_AddStringToObject(di, "bytes", h->disasm[k].bytes);
        cJSON_AddStringToObject(di, "mnemonic", h->disasm[k].mnemonic);
        cJSON_AddStringToObject(di, "op_str", h->disasm[k].op_str);
        cJSON_AddStringToObject(di, "comment", h->disasm[k].comment);
        cJSON_AddItemToArray(dis, di);
    }
    return o;
}

/* watch_hit 事件（poll/inject 产出的每条命中） */
static void event_watch_hit(const gg_watch_hit_t *h) {
    event_simple("watch_hit", watch_hit_json(h));
}

/* ---------------- 参数解析 ---------------- */

static uint64_t parse_value_str(int vtype, const char *s, int *ok) {
    *ok = 0;
    if (!s || !*s) return 0;
    char *end = NULL;
    if (vtype == VT_FLOAT || vtype == VT_DOUBLE) {
        double d = strtod(s, &end);
        if (end == s) return 0;
        uint64_t bits = 0;
        if (vtype == VT_FLOAT) {
            float f = (float)d;
            memcpy(&bits, &f, 4);
        } else {
            double dv = d;
            memcpy(&bits, &dv, 8);
        }
        *ok = 1;
        return bits;
    }
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s || errno == ERANGE) return 0;
    *ok = 1;
    return (uint64_t)v;
}

/* ---------------- 各命令处理 ---------------- */

static void h_ping(int fd, uint32_t seq, cJSON *p) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "pong", 1);
    cJSON_AddStringToObject(r, "agentd", "1.0.0");
    send_ok(fd, GGCMD_PING, seq, r);
    cJSON_Delete(r);
}

static void h_self_test(int fd, uint32_t seq, cJSON *p) {
    int st = driver_self_test(&g_eng.drv);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "state", g_eng.drv.state);
    cJSON_AddStringToObject(r, "state_str", driver_state_str(g_eng.drv.state));
    cJSON_AddNumberToObject(r, "result", st);
    cJSON_AddNumberToObject(r, "reads", (double)g_eng.drv.stat_reads);
    cJSON_AddNumberToObject(r, "writes", (double)g_eng.drv.stat_writes);
    cJSON_AddNumberToObject(r, "fallbacks", (double)g_eng.drv.stat_fallbacks);
    cJSON_AddStringToObject(r, "error", g_eng.drv.last_error);
    send_ok(fd, GGCMD_SELF_TEST, seq, r);
    cJSON_Delete(r);
}

static void h_list_processes(int fd, uint32_t seq, cJSON *p) {
    const char *filter = cJSON_GetObjectItem(p, "filter") ?
        cJSON_GetObjectItem(p, "filter")->valuestring : NULL;
    size_t n = 0;
    gg_proc_entry_t *arr = process_list(&n, filter);
    cJSON *r = cJSON_CreateObject();
    cJSON *procs = cJSON_AddArrayToObject(r, "processes");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "pid", arr[i].pid);
        cJSON_AddStringToObject(o, "name", arr[i].name);
        cJSON_AddStringToObject(o, "comm", arr[i].comm);
        cJSON_AddNumberToObject(o, "uid", arr[i].uid);
        cJSON_AddItemToArray(procs, o);
    }
    free(arr);
    send_ok(fd, GGCMD_LIST_PROCESSES, seq, r);
    cJSON_Delete(r);
}

static pid_t resolve_pid(cJSON *p) {
    cJSON *pidj = cJSON_GetObjectItem(p, "pid");
    cJSON *namej = cJSON_GetObjectItem(p, "name");
    if (pidj && pidj->valuedouble > 0) return (pid_t)pidj->valuedouble;
    if (namej && namej->valuestring && *namej->valuestring) {
        size_t n = 0;
        gg_proc_entry_t *arr = process_list(&n, NULL);
        pid_t found = -1;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(arr[i].name, namej->valuestring) == 0 ||
                strcmp(arr[i].comm, namej->valuestring) == 0) {
                found = arr[i].pid;
                break;
            }
        }
        free(arr);
        return found;
    }
    return -1;
}

static void h_attach(int fd, uint32_t seq, cJSON *p) {
    pid_t pid = resolve_pid(p);
    if (pid <= 0 || !process_exists(pid)) {
        send_err(fd, GGCMD_ATTACH, seq, GGERR_NOT_FOUND, "process not found");
        return;
    }
    if (process_is_64bit(pid) == 0) {
        send_err(fd, GGCMD_ATTACH, seq, GGERR_ARCH_UNSUPPORTED,
                 "32-bit target not supported (driver struct is 64-bit)");
        return;
    }
    /* 清理旧会话 */
    watch_stop(&g_watch);
    freeze_stop(&g_eng.freeze);
    results_clear(&g_eng.results);
    results_init(&g_eng.results, g_eng.result_cap);
    maps_cache_invalidate();   /* maps 会话缓存随会话切换失效 */

    g_eng.session_pid = pid;
    snprintf(g_eng.session_name, sizeof(g_eng.session_name), "%d", (int)pid);
    size_t n = 0;
    gg_proc_entry_t *arr = process_list(&n, NULL);
    for (size_t i = 0; i < n; i++) {
        if (arr[i].pid == pid) {
            snprintf(g_eng.session_name, sizeof(g_eng.session_name), "%s", arr[i].name);
            break;
        }
    }
    free(arr);
    freeze_start(&g_eng.freeze, pid);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "pid", pid);
    cJSON_AddStringToObject(r, "name", g_eng.session_name);
    cJSON_AddNumberToObject(r, "arch", 64);
    cJSON_AddNumberToObject(r, "driver_state", g_eng.drv.state);
    send_ok(fd, GGCMD_ATTACH, seq, r);
    cJSON_Delete(r);
    event_session_change("attached", pid, g_eng.session_name);
}

static void h_detach(int fd, uint32_t seq, cJSON *p) {
    watch_stop(&g_watch);
    freeze_stop(&g_eng.freeze);
    results_clear(&g_eng.results);
    maps_cache_invalidate();   /* maps 会话缓存随会话切换失效 */
    pid_t old_pid = g_eng.session_pid;
    char old_name[256];
    snprintf(old_name, sizeof(old_name), "%s", g_eng.session_name);
    g_eng.session_pid = -1;
    g_eng.session_name[0] = 0;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    send_ok(fd, GGCMD_DETACH, seq, r);
    cJSON_Delete(r);
    event_session_change("detached", old_pid, old_name);
}

/* 模块名 = path basename（供客户端按 name 索引, 免解析 path） */
static const char *mod_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void h_target_info(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_TARGET_INFO, seq, GGERR_NO_SESSION, "no session"); return; }
    size_t nr = 0;
    gg_map_region_t *regs = process_maps(g_eng.session_pid, &nr);
    size_t nm = 0;
    gg_module_t *mods = process_modules(g_eng.session_pid, &nm);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "pid", g_eng.session_pid);
    cJSON_AddStringToObject(r, "name", g_eng.session_name);
    cJSON *regions = cJSON_AddArrayToObject(r, "regions");
    if (regs) {
        for (size_t i = 0; i < nr; i++) {
            cJSON *o = cJSON_CreateObject();
            char start[32], end[32];
            snprintf(start, sizeof(start), "0x%llx", (unsigned long long)regs[i].start);
            snprintf(end, sizeof(end), "0x%llx", (unsigned long long)regs[i].end);
            cJSON_AddStringToObject(o, "start", start);
            cJSON_AddStringToObject(o, "end", end);
            cJSON_AddStringToObject(o, "perms", regs[i].perms);
            cJSON_AddStringToObject(o, "path", regs[i].path);
            cJSON_AddItemToArray(regions, o);
        }
        free(regs);
    }
    cJSON *modules = cJSON_AddArrayToObject(r, "modules");
    if (mods) {
        for (size_t i = 0; i < nm; i++) {
            cJSON *o = cJSON_CreateObject();
            char base[32];
            snprintf(base, sizeof(base), "0x%llx", (unsigned long long)mods[i].base);
            cJSON_AddStringToObject(o, "path", mods[i].path);
            cJSON_AddStringToObject(o, "name", mod_basename(mods[i].path));
            cJSON_AddStringToObject(o, "base", base);
            cJSON_AddNumberToObject(o, "size", (double)mods[i].size);
            cJSON_AddItemToArray(modules, o);
        }
        free(mods);
    }
    send_ok(fd, GGCMD_TARGET_INFO, seq, r);
    cJSON_Delete(r);
}

static void h_module_list(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_MODULE_LIST, seq, GGERR_NO_SESSION, "no session"); return; }
    size_t nm = 0;
    gg_module_t *mods = process_modules(g_eng.session_pid, &nm);
    cJSON *r = cJSON_CreateObject();
    cJSON *modules = cJSON_AddArrayToObject(r, "modules");
    for (size_t i = 0; i < nm; i++) {
        cJSON *o = cJSON_CreateObject();
        char base[32];
        snprintf(base, sizeof(base), "0x%llx", (unsigned long long)mods[i].base);
        cJSON_AddStringToObject(o, "path", mods[i].path);
        cJSON_AddStringToObject(o, "name", mod_basename(mods[i].path));
        cJSON_AddStringToObject(o, "base", base);
        cJSON_AddNumberToObject(o, "size", (double)mods[i].size);
        cJSON_AddItemToArray(modules, o);
    }
    free(mods);
    send_ok(fd, GGCMD_MODULE_LIST, seq, r);
    cJSON_Delete(r);
}

/* ---- type/region 参数兼容解析（2026-08-19） ----
 * MCP 层（PC 端 session.cpp）会把字符串转 int 再转发，但直连 agentd 的
 * 客户端（gg_direct.py 等）传字符串会拿到 valueint=0 → 静默失败/走默认。
 * 统一在此兼容两种形式：数字直接取，字符串按名字映射（与 PC 端
 * norm_type/norm_region 同一套名字）。 */
static int parse_vt(const cJSON *p, int def) {
    const cJSON *j = cJSON_GetObjectItem(p, "type");
    if (!j) return def;
    if (j->type == cJSON_Number) {
        int v = j->valueint;
        if (v >= VT_BYTE && v <= VT_BYTES) return v;
        return -1;   /* 越界数字 → 调用方报错（替代静默走默认） */
    }
    if (j->type == cJSON_String) {
        const char *s = j->valuestring;
        if (s && *s) {
            if (!strcmp(s, "byte") || !strcmp(s, "u8")) return VT_BYTE;
            if (!strcmp(s, "word") || !strcmp(s, "u16")) return VT_WORD;
            if (!strcmp(s, "dword") || !strcmp(s, "u32") || !strcmp(s, "int")) return VT_DWORD;
            if (!strcmp(s, "qword") || !strcmp(s, "u64") || !strcmp(s, "long")) return VT_QWORD;
            if (!strcmp(s, "float") || !strcmp(s, "f32")) return VT_FLOAT;
            if (!strcmp(s, "double") || !strcmp(s, "f64")) return VT_DOUBLE;
            if (!strcmp(s, "bytes") || !strcmp(s, "aob")) return VT_BYTES;
        }
        return -1;   /* 字符串不识别 → 调用方报错 */
    }
    return def;
}
static int parse_region(const cJSON *p, int def) {
    const cJSON *j = cJSON_GetObjectItem(p, "region");
    if (!j) return def;
    if (j->type == cJSON_Number) {
        int v = j->valueint;
        if (v >= REGION_ANONYMOUS && v <= REGION_ALL) return v;
        return -1;
    }
    if (j->type == cJSON_String) {
        const char *s = j->valuestring;
        if (s && *s) {
            if (!strcmp(s, "anonymous") || !strcmp(s, "anon")) return REGION_ANONYMOUS;
            if (!strcmp(s, "writable")) return REGION_WRITABLE;
            if (!strcmp(s, "all")) return REGION_ALL;
        }
        return -1;
    }
    return def;
}

/* type/region 无效值统一报错文案（省得每个命令重复写） */
#define VT_HINT "type: 1-7 或 byte/word/dword/qword/float/double/bytes"
#define REGION_HINT "region: 1-3 或 anonymous/writable/all"

static void h_search(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_NO_SESSION, "no session"); return; }
    int type = parse_vt(p, VT_DWORD);
    int region = parse_region(p, REGION_ANONYMOUS);
    if (type < 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_BAD_REQUEST, "invalid " VT_HINT); return; }
    if (region < 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_BAD_REQUEST, "invalid " REGION_HINT); return; }
    const char *vs = cJSON_GetObjectItem(p, "value") ? cJSON_GetObjectItem(p, "value")->valuestring : NULL;
    int64_t prev = (int64_t)g_eng.results.count;
    int64_t found = 0;

    if (type == VT_BYTES) {
        /* AoB 特征码搜索: value = hex 字符串 "48 65 4C 4C" */
        uint8_t pat[256];
        int plen = parse_hex_pattern(vs, pat, sizeof(pat));
        if (plen <= 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_BAD_REQUEST, "bad aob pattern"); return; }
        found = search_aob(&g_eng.drv, g_eng.session_pid, &g_eng.results, pat, (size_t)plen, region);
        if (found < 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_INTERNAL, "aob search failed"); return; }
    } else {
        int ok = 0;
        uint64_t value = parse_value_str(type, vs, &ok);
        if (!ok) { send_err(fd, GGCMD_SEARCH, seq, GGERR_BAD_REQUEST, "bad value"); return; }
        found = search_exact(&g_eng.drv, g_eng.session_pid, &g_eng.results, type, value, region);
        if (found < 0) { send_err(fd, GGCMD_SEARCH, seq, GGERR_INTERNAL, "search failed"); return; }
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "count", (double)found);
    cJSON_AddBoolToObject(r, "truncated", found >= (int64_t)g_eng.results.cap);
    send_ok(fd, GGCMD_SEARCH, seq, r);
    cJSON_Delete(r);
    event_search_update(prev, (long)found);
    event_results_update();
}

static void h_search_range(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_SEARCH_RANGE, seq, GGERR_NO_SESSION, "no session"); return; }
    int type = parse_vt(p, VT_DWORD);
    int region = parse_region(p, REGION_ANONYMOUS);
    if (type < 0) { send_err(fd, GGCMD_SEARCH_RANGE, seq, GGERR_BAD_REQUEST, "invalid " VT_HINT); return; }
    if (region < 0) { send_err(fd, GGCMD_SEARCH_RANGE, seq, GGERR_BAD_REQUEST, "invalid " REGION_HINT); return; }
    double vmin = cJSON_GetObjectItem(p, "min") ? cJSON_GetObjectItem(p, "min")->valuedouble : 0;
    double vmax = cJSON_GetObjectItem(p, "max") ? cJSON_GetObjectItem(p, "max")->valuedouble : 0;
    int64_t prev = (int64_t)g_eng.results.count;
    int64_t found = search_range(&g_eng.drv, g_eng.session_pid, &g_eng.results, type, vmin, vmax, region);
    if (found < 0) { send_err(fd, GGCMD_SEARCH_RANGE, seq, GGERR_INTERNAL, "range search failed"); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "count", (double)found);
    cJSON_AddBoolToObject(r, "truncated", found >= (int64_t)g_eng.results.cap);
    send_ok(fd, GGCMD_SEARCH_RANGE, seq, r);
    cJSON_Delete(r);
    event_search_update(prev, (long)found);
    event_results_update();
}

static void h_unknown_init(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_SEARCH_UNKNOWN_INIT, seq, GGERR_NO_SESSION, "no session"); return; }
    int type = parse_vt(p, VT_DWORD);
    int region = parse_region(p, REGION_ANONYMOUS);
    if (type < 0) { send_err(fd, GGCMD_SEARCH_UNKNOWN_INIT, seq, GGERR_BAD_REQUEST, "invalid " VT_HINT); return; }
    if (region < 0) { send_err(fd, GGCMD_SEARCH_UNKNOWN_INIT, seq, GGERR_BAD_REQUEST, "invalid " REGION_HINT); return; }
    int64_t prev = (int64_t)g_eng.results.count;
    int64_t elems = search_unknown_init(&g_eng.drv, g_eng.session_pid, &g_eng.results, type, region);
    if (elems < 0) { send_err(fd, GGCMD_SEARCH_UNKNOWN_INIT, seq, GGERR_INTERNAL, "unknown-init failed"); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "elements", (double)elems);
    send_ok(fd, GGCMD_SEARCH_UNKNOWN_INIT, seq, r);
    cJSON_Delete(r);
    event_search_update(prev, 0);   /* 模糊初始化后具体命中数未知, 广播清 0 */
}

static void h_refine(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_SEARCH_REFINE, seq, GGERR_NO_SESSION, "no session"); return; }
    int op = cJSON_GetObjectItem(p, "op") ? cJSON_GetObjectItem(p, "op")->valueint : REFINE_CHANGED;
    int64_t prev = (int64_t)g_eng.results.count;
    int64_t rem;
    if (g_eng.results.fuzzy_active) {
        rem = search_refine(&g_eng.drv, g_eng.session_pid, &g_eng.results, op);
    } else {
        /* 精确/区间搜索结果: 按"当前值 vs 搜索时快照" refine（GG 标准流程） */
        rem = search_refine_exact(&g_eng.drv, g_eng.session_pid, &g_eng.results, op);
    }
    if (rem < 0) { send_err(fd, GGCMD_SEARCH_REFINE, seq, GGERR_INTERNAL, "refine failed (search first?)"); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "remaining", (double)rem);
    send_ok(fd, GGCMD_SEARCH_REFINE, seq, r);
    cJSON_Delete(r);
    event_search_update(prev, (long)rem);
    event_results_update();
}

/* 反汇编目标进程内存（Capstone ARM64）: {addr, length} → {insns:[{addr,bytes,mnemonic,op_str,comment}]} */
static void h_disassemble(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) {
        send_err(fd, GGCMD_DISASSEMBLE, seq, GGERR_NO_SESSION, "no session");
        return;
    }
    const char *as = cJSON_GetObjectItem(p, "addr") ?
        cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (!as) { send_err(fd, GGCMD_DISASSEMBLE, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    uint64_t addr = strtoull(as, NULL, 0);
    int length = cJSON_GetObjectItem(p, "length") ?
        cJSON_GetObjectItem(p, "length")->valueint : 64;
    if (length <= 0) length = 64;
    if (length > 4096) length = 4096;   /* 上限 4KB */

    uint8_t *buf = malloc((size_t)length);
    if (!buf) { send_err(fd, GGCMD_DISASSEMBLE, seq, GGERR_INTERNAL, "oom"); return; }
    ssize_t got = driver_read(&g_eng.drv, g_eng.session_pid, addr, buf, (size_t)length);
    if (got <= 0) {
        free(buf);
        send_err(fd, GGCMD_DISASSEMBLE, seq, GGERR_INTERNAL, "read failed");
        return;
    }

    char mnem[GG_DISASM_MAX_INSNS][64];
    char ops[GG_DISASM_MAX_INSNS][128];
    char bytes[GG_DISASM_MAX_INSNS][32];
    char comment[GG_DISASM_MAX_INSNS][64];
    int n = disasm_a64(buf, (size_t)got, addr, mnem, ops, bytes, comment,
                       GG_DISASM_MAX_INSNS);
    free(buf);
    if (n < 0) {
        send_err(fd, GGCMD_DISASSEMBLE, seq, GGERR_INTERNAL, "capstone init failed");
        return;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "arch", "arm64");
    cJSON_AddStringToObject(r, "start", as);
    cJSON_AddNumberToObject(r, "bytes_read", (double)got);
    cJSON_AddNumberToObject(r, "count", (double)n);
    cJSON *arr = cJSON_AddArrayToObject(r, "insns");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        char ab[32];
        snprintf(ab, sizeof(ab), "0x%llx", (unsigned long long)(addr + (uint64_t)(i * 4)));
        cJSON_AddStringToObject(o, "addr", ab);
        cJSON_AddStringToObject(o, "bytes", bytes[i]);
        cJSON_AddStringToObject(o, "mnemonic", mnem[i]);
        cJSON_AddStringToObject(o, "op_str", ops[i]);
        cJSON_AddStringToObject(o, "comment", comment[i]);
        cJSON_AddItemToArray(arr, o);
    }
    send_ok(fd, GGCMD_DISASSEMBLE, seq, r);
    cJSON_Delete(r);
}

static void h_watch(int fd, uint32_t seq, cJSON *p) {
    const char *op = cJSON_GetObjectItem(p, "op") ?
        cJSON_GetObjectItem(p, "op")->valuestring : "set";
    cJSON *r = cJSON_CreateObject();
    if (strcmp(op, "set") == 0) {
        if (g_eng.session_pid <= 0) {
            cJSON_Delete(r);
            send_err(fd, GGCMD_WATCH, seq, GGERR_NO_SESSION, "no session");
            return;
        }
        const char *as = cJSON_GetObjectItem(p, "addr") ?
            cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
        if (!as) { cJSON_Delete(r); send_err(fd, GGCMD_WATCH, seq, GGERR_BAD_REQUEST, "addr required"); return; }
        uint64_t addr = strtoull(as, NULL, 0);
        int btype = cJSON_GetObjectItem(p, "type") ?
            cJSON_GetObjectItem(p, "type")->valueint : HW_BP_TYPE_W;
        int blen = cJSON_GetObjectItem(p, "len") ?
            cJSON_GetObjectItem(p, "len")->valueint : 4;
        if (g_watch.active) watch_stop(&g_watch);   /* 重复 set 前先清旧的, 防断点槽位泄漏 */
        int rc = watch_start(&g_watch, &g_eng.drv, g_eng.session_pid, addr, btype, blen);
        cJSON_AddBoolToObject(r, "active", rc == 0 ? 1 : 0);
        cJSON_AddNumberToObject(r, "threads", g_watch.ntids);
        cJSON_AddStringToObject(r, "info", g_watch.last_error);
    } else if (strcmp(op, "clear") == 0) {
        watch_stop(&g_watch);
        cJSON_AddBoolToObject(r, "active", 0);
    } else if (strcmp(op, "poll") == 0) {
        gg_watch_hit_t hits[64];
        int n = watch_poll(&g_watch, &g_eng.drv, hits, 64);
        cJSON_AddNumberToObject(r, "hits", n);
        cJSON_AddBoolToObject(r, "active", g_watch.active ? 1 : 0);
        cJSON *arr = cJSON_AddArrayToObject(r, "list");
        for (int i = 0; i < n; i++) {
            cJSON_AddItemToArray(arr, watch_hit_json(&hits[i]));
            event_watch_hit(&hits[i]);   /* 实时推送给订阅端（手机悬浮窗） */
        }
    } else if (strcmp(op, "inject") == 0) {
        /* 调试: 手动构造命中并走完整 enrich 管道（断点不可用环境验证日志） */
        if (g_eng.session_pid <= 0) {
            cJSON_Delete(r);
            send_err(fd, GGCMD_WATCH, seq, GGERR_NO_SESSION, "no session");
            return;
        }
        uint64_t pc = cJSON_GetObjectItem(p, "pc") ?
            strtoull(cJSON_GetObjectItem(p, "pc")->valuestring, NULL, 0) : 0;
        uint64_t fp = cJSON_GetObjectItem(p, "fp") ?
            strtoull(cJSON_GetObjectItem(p, "fp")->valuestring, NULL, 0) : 0;
        uint64_t sp = cJSON_GetObjectItem(p, "sp") ?
            strtoull(cJSON_GetObjectItem(p, "sp")->valuestring, NULL, 0) : 0;
        uint64_t lr = cJSON_GetObjectItem(p, "lr") ?
            strtoull(cJSON_GetObjectItem(p, "lr")->valuestring, NULL, 0) : 0;
        gg_watch_hit_t hit;
        watch_inject(&g_watch, &g_eng.drv, g_eng.session_pid, pc, fp, sp, lr, 0, &hit);
        cJSON_AddNumberToObject(r, "hits", 1);
        cJSON_AddItemToObject(r, "hit", watch_hit_json(&hit));
        event_watch_hit(&hit);
    } else {
        cJSON_Delete(r);
        send_err(fd, GGCMD_WATCH, seq, GGERR_BAD_REQUEST, "op must be set|clear|poll");
        return;
    }
    send_ok(fd, GGCMD_WATCH, seq, r);
    cJSON_Delete(r);
}

static void h_result_count(int fd, uint32_t seq, cJSON *p) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "count", (double)g_eng.results.count);
    cJSON_AddNumberToObject(r, "cap", (double)g_eng.results.cap);
    cJSON_AddBoolToObject(r, "fuzzy_active", g_eng.results.fuzzy_active ? 1 : 0);
    if (g_eng.results.fuzzy_active) {
        cJSON_AddNumberToObject(r, "fuzzy_candidates",
            (double)results_fuzzy_count(&g_eng.results));
    }
    send_ok(fd, GGCMD_RESULT_COUNT, seq, r);
    cJSON_Delete(r);
}

static void h_get_results(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_GET_RESULTS, seq, GGERR_NO_SESSION, "no session"); return; }
    size_t offset = cJSON_GetObjectItem(p, "offset") ? (size_t)cJSON_GetObjectItem(p, "offset")->valuedouble : 0;
    size_t limit = cJSON_GetObjectItem(p, "limit") ? (size_t)cJSON_GetObjectItem(p, "limit")->valuedouble : 100;
    /* 值类型以结果集自身为准（精确搜索 exact_vt / 模糊物化 fuzzy_vt），调用方 type 仅兜底：
       避免类型不匹配读错字节数/错值（2026-08-18 CF 实测：带 type 偶发串旧 buffer） */
    int type = VT_DWORD;
    if (g_eng.results.exact_vt > 0) type = g_eng.results.exact_vt;
    else if (g_eng.results.fuzzy_vt > 0) type = g_eng.results.fuzzy_vt;
    if (limit > 10000) limit = 10000;

    uint64_t *addrs = malloc(limit * sizeof(uint64_t));
    uint64_t *vals = malloc(limit * sizeof(uint64_t));
    size_t got = 0;
    if (addrs && vals) {
        results_get_page(&g_eng.drv, g_eng.session_pid, &g_eng.results,
                         offset, limit, addrs, vals, type, &got);
    }
    cJSON *r = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(r, "results");
    for (size_t i = 0; i < got; i++) {
        cJSON *o = cJSON_CreateObject();
        char addr[32], val[32], vhex[24];
        snprintf(addr, sizeof(addr), "0x%llx", (unsigned long long)addrs[i]);
        snprintf(val, sizeof(val), "%llu", (unsigned long long)vals[i]);
        snprintf(vhex, sizeof(vhex), "0x%llx", (unsigned long long)vals[i]);
        cJSON_AddStringToObject(o, "addr", addr);
        cJSON_AddStringToObject(o, "value", val);      /* 十进制字符串（u64 精确） */
        cJSON_AddStringToObject(o, "value_hex", vhex);
        cJSON_AddItemToArray(items, o);
    }
    cJSON_AddNumberToObject(r, "total", (double)g_eng.results.count);
    free(addrs);
    free(vals);
    send_ok(fd, GGCMD_GET_RESULTS, seq, r);
    cJSON_Delete(r);
}

static void h_clear_results(int fd, uint32_t seq, cJSON *p) {
    int64_t prev = (int64_t)g_eng.results.count;
    results_clear(&g_eng.results);
    results_init(&g_eng.results, g_eng.result_cap);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    send_ok(fd, GGCMD_CLEAR_RESULTS, seq, r);
    cJSON_Delete(r);
    event_search_update(prev, 0);
    event_results_update();
}

static void h_read(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_READ, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    int type = parse_vt(p, VT_DWORD);
    if (type < 0) { send_err(fd, GGCMD_READ, seq, GGERR_BAD_REQUEST, "invalid " VT_HINT); return; }
    if (!as) { send_err(fd, GGCMD_READ, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    uint64_t addr = strtoull(as, NULL, 0);
    int sz = vt_size(type);
    uint8_t buf[8] = {0};
    ssize_t got = driver_read(&g_eng.drv, g_eng.session_pid, addr, buf, (size_t)sz);
    if (got < 0) { send_err(fd, GGCMD_READ, seq, GGERR_PERM, g_eng.drv.last_error); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "value", (double)read_bits(buf, type));
    cJSON_AddNumberToObject(r, "bytes", (double)got);
    send_ok(fd, GGCMD_READ, seq, r);
    cJSON_Delete(r);
}

static void h_read_bytes(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_READ_BYTES, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    size_t len = cJSON_GetObjectItem(p, "len") ? (size_t)cJSON_GetObjectItem(p, "len")->valuedouble : 64;
    if (!as) { send_err(fd, GGCMD_READ_BYTES, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    if (len > GG_SEARCH_CHUNK) len = GG_SEARCH_CHUNK;   /* 单次读上限 256KB（= RWPROC_MAX_IO） */
    uint64_t addr = strtoull(as, NULL, 0);
    uint8_t *buf = malloc(len);
    ssize_t got = driver_read(&g_eng.drv, g_eng.session_pid, addr, buf, len);
    cJSON *r = cJSON_CreateObject();
    if (got < 0) {
        cJSON_AddStringToObject(r, "hex", "");
        cJSON_AddNumberToObject(r, "bytes", 0);
        cJSON_AddStringToObject(r, "error", g_eng.drv.last_error);
    } else {
        char *hex = malloc((size_t)got * 2 + 1);
        for (ssize_t i = 0; i < got; i++) snprintf(hex + i * 2, 3, "%02x", buf[i]);
        cJSON_AddStringToObject(r, "hex", hex);
        cJSON_AddNumberToObject(r, "bytes", (double)got);
        free(hex);
    }
    free(buf);
    send_ok(fd, GGCMD_READ_BYTES, seq, r);
    cJSON_Delete(r);
}

/* ---------------- 结构化内存查看（gg_view_memory） ---------------- */

/* 元素解释类型: 1=int32 2=float32 3=pointer(arm64, 8B) */
static int parse_view_etype(const char *s) {
    if (!s || !*s) return 1;
    if (strcmp(s, "int32") == 0 || strcmp(s, "i32") == 0 || strcmp(s, "int") == 0) return 1;
    if (strcmp(s, "float32") == 0 || strcmp(s, "f32") == 0 || strcmp(s, "float") == 0) return 2;
    if (strcmp(s, "pointer") == 0 || strcmp(s, "ptr") == 0) return 3;
    return -1;
}

/* /proc/<pid>/maps 按地址升序 → 二分查找 addr 所在映射 */
static const gg_map_region_t *map_find(const gg_map_region_t *maps, size_t n, uint64_t addr) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (maps[mid].start <= addr) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return NULL;
    const gg_map_region_t *r = &maps[lo - 1];
    return (addr >= r->start && addr < r->end) ? r : NULL;
}

static void h_view_memory(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_VIEW_MEMORY, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (!as) { send_err(fd, GGCMD_VIEW_MEMORY, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    const char *ets = cJSON_GetObjectItem(p, "element_type") ?
        cJSON_GetObjectItem(p, "element_type")->valuestring : "int32";
    int etype = parse_view_etype(ets);
    if (etype < 0) {
        send_err(fd, GGCMD_VIEW_MEMORY, seq, GGERR_BAD_REQUEST,
                 "element_type must be int32|float32|pointer");
        return;
    }
    long long range = cJSON_GetObjectItem(p, "range") ?
        (long long)cJSON_GetObjectItem(p, "range")->valuedouble : 8;
    if (range < 1) range = 1;
    if (range > 512) range = 512;

    int esz = (etype == 3) ? 8 : 4;
    uint64_t addr = strtoull(as, NULL, 0);
    size_t nbytes = (size_t)range * (size_t)esz;
    uint8_t *buf = malloc(nbytes);
    if (!buf) { send_err(fd, GGCMD_VIEW_MEMORY, seq, GGERR_INTERNAL, "oom"); return; }
    ssize_t got = driver_read(&g_eng.drv, g_eng.session_pid, addr, buf, nbytes);

    /* 一次取 maps：顶层标注基址所在区域，pointer 元素标注目标是否映射/所在模块 */
    size_t nreg = 0;
    gg_map_region_t *maps = process_maps(g_eng.session_pid, &nreg);

    cJSON *r = cJSON_CreateObject();
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)addr);
    cJSON_AddStringToObject(r, "addr", tmp);
    cJSON_AddStringToObject(r, "element_type",
                            etype == 1 ? "int32" : (etype == 2 ? "float32" : "pointer"));
    cJSON_AddNumberToObject(r, "element_size", esz);
    cJSON_AddNumberToObject(r, "range", (double)range);
    cJSON_AddBoolToObject(r, "base_mapped", map_find(maps, nreg, addr) ? 1 : 0);

    if (got < 0) {
        cJSON_AddNumberToObject(r, "count", 0);
        cJSON_AddNumberToObject(r, "bytes_read", 0);
        cJSON_AddStringToObject(r, "error", g_eng.drv.last_error);
    } else {
        size_t count = (size_t)got / (size_t)esz;   /* 只解析完整元素，尾部残缺丢弃 */
        cJSON_AddNumberToObject(r, "count", (double)count);
        cJSON_AddNumberToObject(r, "bytes_read", (double)got);
        cJSON *els = cJSON_AddArrayToObject(r, "elements");
        for (size_t i = 0; i < count; i++) {
            const uint8_t *ep = buf + i * (size_t)esz;
            uint64_t eaddr = addr + i * (size_t)esz;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "index", (double)i);
            cJSON_AddNumberToObject(o, "offset", (double)(i * (size_t)esz));
            snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)eaddr);
            cJSON_AddStringToObject(o, "addr", tmp);
            if (etype == 1) {          /* int32: 有符号十进制 + 原始位 */
                int32_t v;
                memcpy(&v, ep, 4);
                cJSON_AddNumberToObject(o, "value", (double)v);
                snprintf(tmp, sizeof(tmp), "0x%08x", (unsigned)(uint32_t)v);
                cJSON_AddStringToObject(o, "hex", tmp);
            } else if (etype == 2) {   /* float32: 浮点 + 原始位 */
                float f;
                uint32_t bits;
                memcpy(&f, ep, 4);
                memcpy(&bits, ep, 4);
                cJSON_AddNumberToObject(o, "value", (double)f);
                snprintf(tmp, sizeof(tmp), "0x%08x", (unsigned)bits);
                cJSON_AddStringToObject(o, "hex", tmp);
            } else {                   /* pointer (arm64, 8B): 指针值 + 映射标注 */
                uint64_t ptr;
                memcpy(&ptr, ep, 8);
                snprintf(tmp, sizeof(tmp), "0x%016llx", (unsigned long long)ptr);
                cJSON_AddStringToObject(o, "pointer", tmp);
                const gg_map_region_t *tr = map_find(maps, nreg, ptr);
                cJSON_AddBoolToObject(o, "mapped", tr ? 1 : 0);
                cJSON_AddStringToObject(o, "region",
                                        tr ? (tr->path[0] ? tr->path : "(anonymous)") : "");
                if (tr) cJSON_AddStringToObject(o, "region_perms", tr->perms);
            }
            cJSON_AddItemToArray(els, o);
        }
    }
    free(buf);
    free(maps);
    send_ok(fd, GGCMD_VIEW_MEMORY, seq, r);
    cJSON_Delete(r);
}

/* ---------------- 地址解析（gg_resolve_symbol 本地静态层） ---------------- */

static const char *path_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* 同路径映射的最低 start = 模块加载基址（.so 多段映射的首段） */
static uint64_t map_load_base(const gg_map_region_t *maps, size_t n,
                              const gg_map_region_t *r) {
    if (!r->path[0]) return r->start;
    uint64_t lo = r->start;
    for (size_t i = 0; i < n; i++)
        if (maps[i].path[0] && strcmp(maps[i].path, r->path) == 0 && maps[i].start < lo)
            lo = maps[i].start;
    return lo;
}

static void h_resolve_symbol(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_RESOLVE_SYMBOL, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (!as) { send_err(fd, GGCMD_RESOLVE_SYMBOL, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    uint64_t addr = strtoull(as, NULL, 0);

    size_t nreg = 0;
    gg_map_region_t *maps = process_maps(g_eng.session_pid, &nreg);

    cJSON *r = cJSON_CreateObject();
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)addr);
    cJSON_AddStringToObject(r, "addr", tmp);

    const gg_map_region_t *rg = map_find(maps, nreg, addr);
    if (!rg) {
        cJSON_AddBoolToObject(r, "mapped", 0);
    } else {
        cJSON_AddBoolToObject(r, "mapped", 1);
        cJSON_AddStringToObject(r, "region", rg->path[0] ? rg->path : "");
        cJSON_AddStringToObject(r, "perms", rg->perms);
        if (rg->path[0]) {
            uint64_t base = map_load_base(maps, nreg, rg);
            uint64_t off = addr - base;
            cJSON_AddStringToObject(r, "module", path_basename(rg->path));
            snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)base);
            cJSON_AddStringToObject(r, "module_base", tmp);
            snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)off);
            cJSON_AddStringToObject(r, "offset_in_module", tmp);
            /* 纯静态层：ELF .dynsym/.symtab 最近导出符号（无 IDA 也能拿到函数名） */
            char sym[256];
            uint64_t sval = 0, sdelta = 0;
            int skind = 0;
            int rc = elfsym_nearest(rg->path, off, sym, sizeof(sym), &sval, &sdelta, &skind);
            if (rc == 1 && sym[0]) {
                cJSON_AddStringToObject(r, "symbol", sym);
                cJSON_AddNumberToObject(r, "symbol_delta", (double)sdelta);
                cJSON_AddStringToObject(r, "symbol_kind",
                                        skind == ELFSYM_FUNC ? "func" :
                                        (skind == ELFSYM_OBJECT ? "object" : "notype"));
            } else if (rc == -1) {
                cJSON_AddStringToObject(r, "elf_error", "unreadable or not ELF64");
            }
        }
    }
    free(maps);
    send_ok(fd, GGCMD_RESOLVE_SYMBOL, seq, r);
    cJSON_Delete(r);
}

/* ---------------- 多级指针追踪（gg_pointer_chain） ---------------- */

#define GG_PC_MAX_DEPTH 32

static void h_pointer_chain(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_POINTER_CHAIN, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (!as) { send_err(fd, GGCMD_POINTER_CHAIN, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    long long max_depth = cJSON_GetObjectItem(p, "max_depth") ?
        (long long)cJSON_GetObjectItem(p, "max_depth")->valuedouble : 8;
    if (max_depth < 1) max_depth = 1;
    if (max_depth > GG_PC_MAX_DEPTH) max_depth = GG_PC_MAX_DEPTH;

    uint64_t addr = strtoull(as, NULL, 0);
    size_t nreg = 0;
    gg_map_region_t *maps = process_maps(g_eng.session_pid, &nreg);

    cJSON *r = cJSON_CreateObject();
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)addr);
    cJSON_AddStringToObject(r, "start", tmp);
    cJSON_AddNumberToObject(r, "max_depth", (double)max_depth);
    cJSON *levels = cJSON_AddArrayToObject(r, "levels");

    /* 记录每级读到的指针值，防环 */
    uint64_t visited[GG_PC_MAX_DEPTH];
    int nvisited = 0;
    const char *stopped = "ok";
    uint64_t cur = addr;

    /* 最后一个有效映射指针（链的"基址"） */
    int have_base = 0;
    uint64_t base_addr = 0, base_ptr = 0;
    const gg_map_region_t *base_rg = NULL;
    uint64_t base_depth = 0;

    for (int d = 0; d < (int)max_depth; d++) {
        uint8_t buf[8];
        ssize_t got = driver_read(&g_eng.drv, g_eng.session_pid, cur, buf, 8);
        if (got < 8) { stopped = "read_failed"; break; }
        uint64_t ptr;
        memcpy(&ptr, buf, 8);

        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "depth", d);
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)cur);
        cJSON_AddStringToObject(o, "addr", tmp);
        snprintf(tmp, sizeof(tmp), "0x%016llx", (unsigned long long)ptr);
        cJSON_AddStringToObject(o, "pointer", tmp);

        const gg_map_region_t *rg = map_find(maps, nreg, ptr);
        cJSON_AddBoolToObject(o, "mapped", rg ? 1 : 0);
        if (rg) {
            cJSON_AddStringToObject(o, "region", rg->path[0] ? rg->path : "(anonymous)");
            cJSON_AddStringToObject(o, "perms", rg->perms);
            cJSON_AddBoolToObject(o, "at_base", rg->start == ptr ? 1 : 0);
            if (rg->path[0]) {
                uint64_t lb = map_load_base(maps, nreg, rg);
                cJSON_AddStringToObject(o, "module", path_basename(rg->path));
                snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)(ptr - lb));
                cJSON_AddStringToObject(o, "module_offset", tmp);
            } else {
                cJSON_AddStringToObject(o, "module", "");
                cJSON_AddStringToObject(o, "module_offset", "");
            }
        } else {
            cJSON_AddStringToObject(o, "region", "");
            cJSON_AddStringToObject(o, "perms", "");
            cJSON_AddStringToObject(o, "module", "");
            cJSON_AddStringToObject(o, "module_offset", "");
            cJSON_AddBoolToObject(o, "at_base", 0);
        }
        cJSON_AddItemToArray(levels, o);

        if (ptr == 0) { stopped = "null_pointer"; break; }
        if (!rg) { stopped = "unmapped"; break; }

        /* 环检测 */
        int dup = 0;
        for (int i = 0; i < nvisited; i++) if (visited[i] == ptr) { dup = 1; break; }
        if (dup) { stopped = "cycle"; break; }
        visited[nvisited++] = ptr;

        /* 记录链基址（本级指针是有效映射） */
        have_base = 1;
        base_addr = cur;
        base_ptr = ptr;
        base_rg = rg;
        base_depth = (uint64_t)d;

        cur = ptr;
        if (d == (int)max_depth - 1) stopped = "max_depth";
    }

    cJSON_AddStringToObject(r, "stopped", stopped);
    if (have_base && base_rg) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "depth", (double)base_depth);
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)base_addr);
        cJSON_AddStringToObject(b, "addr", tmp);
        snprintf(tmp, sizeof(tmp), "0x%016llx", (unsigned long long)base_ptr);
        cJSON_AddStringToObject(b, "pointer", tmp);
        cJSON_AddStringToObject(b, "region", base_rg->path[0] ? base_rg->path : "(anonymous)");
        cJSON_AddStringToObject(b, "perms", base_rg->perms);
        if (base_rg->path[0]) {
            uint64_t lb = map_load_base(maps, nreg, base_rg);
            cJSON_AddStringToObject(b, "module", path_basename(base_rg->path));
            snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)(base_ptr - lb));
            cJSON_AddStringToObject(b, "module_offset", tmp);
        }
        cJSON_AddItemToObject(r, "base", b);
    }
    free(maps);
    send_ok(fd, GGCMD_POINTER_CHAIN, seq, r);
    cJSON_Delete(r);
}

static void h_write(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.safe_mode) { send_err(fd, GGCMD_WRITE, seq, GGERR_SAFE_MODE, "safe mode: write denied"); return; }
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_WRITE, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    int type = parse_vt(p, VT_DWORD);
    const char *vs = cJSON_GetObjectItem(p, "value") ? cJSON_GetObjectItem(p, "value")->valuestring : NULL;
    if (!as || !vs) { send_err(fd, GGCMD_WRITE, seq, GGERR_BAD_REQUEST, "addr+value required"); return; }
    int ok = 0;
    uint64_t value = parse_value_str(type, vs, &ok);
    if (!ok) { send_err(fd, GGCMD_WRITE, seq, GGERR_BAD_REQUEST, "bad value"); return; }
    uint64_t addr = strtoull(as, NULL, 0);
    uint8_t buf[8];
    encode_value_local(type, value, buf);
    ssize_t w = driver_write(&g_eng.drv, g_eng.session_pid, addr, buf, (size_t)vt_size(type));
    if (w < 0) { send_err(fd, GGCMD_WRITE, seq, GGERR_PERM, g_eng.drv.last_error); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "bytes", (double)w);
    send_ok(fd, GGCMD_WRITE, seq, r);
    cJSON_Delete(r);
}

static void h_set_results(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.safe_mode) { send_err(fd, GGCMD_SET_RESULTS, seq, GGERR_SAFE_MODE, "safe mode: write denied"); return; }
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_SET_RESULTS, seq, GGERR_NO_SESSION, "no session"); return; }
    int type = parse_vt(p, VT_DWORD);
    cJSON *vals = cJSON_GetObjectItem(p, "values");
    if (!cJSON_IsArray(vals)) { send_err(fd, GGCMD_SET_RESULTS, seq, GGERR_BAD_REQUEST, "values array required"); return; }
    if (g_eng.results.fuzzy_active) results_materialize(&g_eng.results);

    int n = cJSON_GetArraySize(vals);
    if (n > 10000) n = 10000;

    /* 逐条: 按 index 或 addr 解析目标地址, 按 value 写入 */
    size_t written = 0;
    int m = 0;
    for (int i = 0; i < n && m < n; i++) {
        cJSON *o = cJSON_GetArrayItem(vals, i);
        cJSON *idxj = cJSON_GetObjectItem(o, "index");
        cJSON *addj = cJSON_GetObjectItem(o, "addr");
        cJSON *vj = cJSON_GetObjectItem(o, "value");
        if (!vj) continue;
        int ok = 0;
        uint64_t value = parse_value_str(type, vj->valuestring, &ok);
        if (!ok) continue;
        uint64_t addr = 0;
        if (addj && addj->valuestring) {
            addr = strtoull(addj->valuestring, NULL, 0);
        } else if (idxj && idxj->valuedouble >= 0 && (size_t)idxj->valuedouble < g_eng.results.count) {
            addr = g_eng.results.addrs[(size_t)idxj->valuedouble];
        } else continue;
        uint8_t buf[8];
        encode_value_local(type, value, buf);
        if (driver_write(&g_eng.drv, g_eng.session_pid, addr, buf, (size_t)vt_size(type)) == (ssize_t)vt_size(type)) {
            written++;
        }
        m++;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "written", (double)written);
    send_ok(fd, GGCMD_SET_RESULTS, seq, r);
    cJSON_Delete(r);
    event_results_update();
}

static void h_freeze(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.safe_mode) { send_err(fd, GGCMD_FREEZE, seq, GGERR_SAFE_MODE, "safe mode: freeze denied"); return; }
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_FREEZE, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    int type = parse_vt(p, VT_DWORD);
    const char *vs = cJSON_GetObjectItem(p, "value") ? cJSON_GetObjectItem(p, "value")->valuestring : NULL;
    if (!as || !vs) { send_err(fd, GGCMD_FREEZE, seq, GGERR_BAD_REQUEST, "addr+value required"); return; }
    int ok = 0;
    uint64_t value = parse_value_str(type, vs, &ok);
    if (!ok) { send_err(fd, GGCMD_FREEZE, seq, GGERR_BAD_REQUEST, "bad value"); return; }
    uint8_t buf[8];
    encode_value_local(type, value, buf);
    if (freeze_add(&g_eng.freeze, strtoull(as, NULL, 0), buf, (size_t)vt_size(type)) < 0) {
        send_err(fd, GGCMD_FREEZE, seq, GGERR_INTERNAL, "freeze table full");
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "frozen", (double)freeze_count(&g_eng.freeze));
    send_ok(fd, GGCMD_FREEZE, seq, r);
    cJSON_Delete(r);
    event_results_update();
}

static void h_unfreeze(int fd, uint32_t seq, cJSON *p) {
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (as) freeze_remove(&g_eng.freeze, strtoull(as, NULL, 0));
    else freeze_clear(&g_eng.freeze);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "frozen", (double)freeze_count(&g_eng.freeze));
    send_ok(fd, GGCMD_UNFREEZE, seq, r);
    cJSON_Delete(r);
    event_results_update();
}

static void h_module_base(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_MODULE_BASE, seq, GGERR_NO_SESSION, "no session"); return; }
    const char *name = cJSON_GetObjectItem(p, "name") ? cJSON_GetObjectItem(p, "name")->valuestring : NULL;
    if (!name) { send_err(fd, GGCMD_MODULE_BASE, seq, GGERR_BAD_REQUEST, "name required"); return; }
    size_t nm = 0;
    gg_module_t *mods = process_modules(g_eng.session_pid, &nm);
    cJSON *r = cJSON_CreateObject();
    int found = 0;
    for (size_t i = 0; i < nm; i++) {
        if (strstr(mods[i].path, name)) {
            char base[32];
            snprintf(base, sizeof(base), "0x%llx", (unsigned long long)mods[i].base);
            cJSON_AddStringToObject(r, "path", mods[i].path);
            cJSON_AddStringToObject(r, "base", base);
            cJSON_AddNumberToObject(r, "size", (double)mods[i].size);
            found = 1;
            break;
        }
    }
    free(mods);
    if (!found) { send_err(fd, GGCMD_MODULE_BASE, seq, GGERR_NOT_FOUND, "module not found"); return; }
    send_ok(fd, GGCMD_MODULE_BASE, seq, r);
    cJSON_Delete(r);
}

static void h_calc_offset(int fd, uint32_t seq, cJSON *p) {
    const char *as = cJSON_GetObjectItem(p, "addr") ? cJSON_GetObjectItem(p, "addr")->valuestring : NULL;
    if (!as) { send_err(fd, GGCMD_CALC_OFFSET, seq, GGERR_BAD_REQUEST, "addr required"); return; }
    uint64_t addr = strtoull(as, NULL, 0);
    const char *modname = cJSON_GetObjectItem(p, "module") ?
        cJSON_GetObjectItem(p, "module")->valuestring : NULL;
    if (modname && g_eng.session_pid > 0) {
        size_t nm = 0;
        gg_module_t *mods = process_modules(g_eng.session_pid, &nm);
        for (size_t i = 0; i < nm; i++) {
            if (strstr(mods[i].path, modname)) {
                cJSON *r = cJSON_CreateObject();
                char off[32];
                snprintf(off, sizeof(off), "0x%llx", (unsigned long long)(addr - mods[i].base));
                cJSON_AddStringToObject(r, "offset", off);
                cJSON_AddStringToObject(r, "base", mods[i].path);
                free(mods);
                send_ok(fd, GGCMD_CALC_OFFSET, seq, r);
                cJSON_Delete(r);
                return;
            }
        }
        free(mods);
        send_err(fd, GGCMD_CALC_OFFSET, seq, GGERR_NOT_FOUND, "module not found");
        return;
    }
    /* 无模块: 相对匿名堆基址（当前结果首地址近似） */
    cJSON *r = cJSON_CreateObject();
    char off[32];
    snprintf(off, sizeof(off), "0x%llx", (unsigned long long)addr);
    cJSON_AddStringToObject(r, "offset", off);
    cJSON_AddStringToObject(r, "note", "no module given; raw address returned");
    send_ok(fd, GGCMD_CALC_OFFSET, seq, r);
    cJSON_Delete(r);
}

static void h_pointer_scan(int fd, uint32_t seq, cJSON *p) {
    if (g_eng.session_pid <= 0) { send_err(fd, GGCMD_POINTER_SCAN, seq, GGERR_NO_SESSION, "no session"); return; }
    int type = parse_vt(p, VT_QWORD);
    size_t cap = cJSON_GetObjectItem(p, "cap") ? (size_t)cJSON_GetObjectItem(p, "cap")->valuedouble : 5000;
    if (g_eng.results.fuzzy_active) results_materialize(&g_eng.results);

    gg_pointer_hit_t *hits = malloc(cap * sizeof(gg_pointer_hit_t));
    size_t n = pointer_scan(&g_eng.drv, g_eng.session_pid,
                            g_eng.results.addrs, g_eng.results.count, type, hits, cap);
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "hits");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        char c[32], t[32];
        snprintf(c, sizeof(c), "0x%llx", (unsigned long long)hits[i].container);
        snprintf(t, sizeof(t), "0x%llx", (unsigned long long)hits[i].target);
        cJSON_AddStringToObject(o, "container", c);
        cJSON_AddStringToObject(o, "target", t);
        cJSON_AddNumberToObject(o, "ptype", hits[i].ptype);
        cJSON_AddItemToArray(arr, o);
    }
    free(hits);
    send_ok(fd, GGCMD_POINTER_SCAN, seq, r);
    cJSON_Delete(r);
}

static void h_pause_resume(int fd, uint32_t seq, cJSON *p, int pause) {
    if (g_eng.session_pid <= 0) {
        send_err(fd, pause ? GGCMD_PAUSE : GGCMD_RESUME, seq, GGERR_NO_SESSION, "no session");
        return;
    }
    int sig = pause ? SIGSTOP : SIGCONT;
    int rc = kill(g_eng.session_pid, sig);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "signal", sig);
    cJSON_AddNumberToObject(r, "rc", rc);
    send_ok(fd, pause ? GGCMD_PAUSE : GGCMD_RESUME, seq, r);
    cJSON_Delete(r);
}

/* GG-TOUCH 内核虚拟触摸注入（rwproc33 驱动 CMD_GGTOUCH_TAP/SWIPE）。
 * 不依赖 attach（触摸是系统级输入, 与目标进程无关）。 */
static void h_ghost_tap(int fd, uint32_t seq, cJSON *p) {
    cJSON *jx = cJSON_GetObjectItem(p, "x");
    cJSON *jy = cJSON_GetObjectItem(p, "y");
    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
        send_err(fd, GGCMD_GHOST_TAP, seq, GGERR_BAD_REQUEST, "need x,y (display coords)");
        return;
    }
    uint32_t x = (uint32_t)jx->valuedouble;
    uint32_t y = (uint32_t)jy->valuedouble;
    uint32_t hold = 120;
    cJSON *jh = cJSON_GetObjectItem(p, "hold_ms");
    if (cJSON_IsNumber(jh)) hold = (uint32_t)jh->valuedouble;

    if (driver_touch_tap(&g_eng.drv, x, y, hold) != 0) {
        send_err(fd, GGCMD_GHOST_TAP, seq, GGERR_DEGRADED, g_eng.drv.last_error);
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "ok", 1);
    cJSON_AddNumberToObject(r, "x", x);
    cJSON_AddNumberToObject(r, "y", y);
    cJSON_AddNumberToObject(r, "hold_ms", hold);
    send_ok(fd, GGCMD_GHOST_TAP, seq, r);
    cJSON_Delete(r);
}

static void h_ghost_swipe(int fd, uint32_t seq, cJSON *p) {
    cJSON *p1 = cJSON_GetObjectItem(p, "x1");
    cJSON *p2 = cJSON_GetObjectItem(p, "y1");
    cJSON *p3 = cJSON_GetObjectItem(p, "x2");
    cJSON *p4 = cJSON_GetObjectItem(p, "y2");
    if (!cJSON_IsNumber(p1) || !cJSON_IsNumber(p2) ||
        !cJSON_IsNumber(p3) || !cJSON_IsNumber(p4)) {
        send_err(fd, GGCMD_GHOST_SWIPE, seq, GGERR_BAD_REQUEST, "need x1,y1,x2,y2");
        return;
    }
    uint32_t ms = 300;
    cJSON *jm = cJSON_GetObjectItem(p, "ms");
    if (cJSON_IsNumber(jm)) ms = (uint32_t)jm->valuedouble;

    if (driver_touch_swipe(&g_eng.drv,
                           (uint32_t)p1->valuedouble, (uint32_t)p2->valuedouble,
                           (uint32_t)p3->valuedouble, (uint32_t)p4->valuedouble,
                           ms) != 0) {
        send_err(fd, GGCMD_GHOST_SWIPE, seq, GGERR_DEGRADED, g_eng.drv.last_error);
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "ok", 1);
    send_ok(fd, GGCMD_GHOST_SWIPE, seq, r);
    cJSON_Delete(r);
}

static void h_kernel_status(int fd, uint32_t seq, cJSON *p) {
    cJSON *r = cJSON_CreateObject();
    gg_driver_t *d = &g_eng.drv;
    /* X3: 本 handler 经快速通道（无锁）执行, 与锁内引擎操作（搜索/attach 等）
     * 并发 —— 整型字段原子读（ARM64 对齐原子读写零额外开销）,
     * 字符串快照容忍撕裂（仅内容短暂不完整, JSON 仍合法）。 */
    int st = __atomic_load_n(&d->state, __ATOMIC_RELAXED);
    uint64_t reads  = atomic_load_explicit(&d->stat_reads, memory_order_relaxed);
    uint64_t writes = atomic_load_explicit(&d->stat_writes, memory_order_relaxed);
    uint64_t falls  = atomic_load_explicit(&d->stat_fallbacks, memory_order_relaxed);
    int ko_type   = __atomic_load_n(&d->ko_type, __ATOMIC_RELAXED);
    int ko_loaded = __atomic_load_n(&d->ko_loaded, __ATOMIC_RELAXED);
    int qx_key    = __atomic_load_n(&d->qx_have_key, __ATOMIC_RELAXED);
    int ioctl_r   = __atomic_load_n(&d->ioctl_read, __ATOMIC_RELAXED);
    int ioctl_w   = __atomic_load_n(&d->ioctl_write, __ATOMIC_RELAXED);
    int frozen    = freeze_count(&g_eng.freeze);
    int safe      = __atomic_load_n(&g_eng.safe_mode, __ATOMIC_RELAXED);
    int shell     = __atomic_load_n(&g_eng.shell_enabled, __ATOMIC_RELAXED);
    int s_pid     = __atomic_load_n(&g_eng.session_pid, __ATOMIC_RELAXED);
    char err[256], ko_mod[256], ko_file[512], ko_ver[64], dev_p[128], s_name[256];
#define SNAP(dst, src) do { \
        strncpy((dst), (src), sizeof(dst) - 1); \
        (dst)[sizeof(dst) - 1] = 0; \
    } while (0)
    SNAP(err, d->last_error);   SNAP(ko_mod, d->ko_module);
    SNAP(ko_file, d->ko_file);  SNAP(ko_ver, d->ko_version);
    SNAP(dev_p, d->dev_path);   SNAP(s_name, g_eng.session_name);
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
    cJSON_AddNumberToObject(r, "ko_type", ko_type);
    cJSON_AddBoolToObject(r, "ko_loaded", ko_loaded ? 1 : 0);
    cJSON_AddStringToObject(r, "ko_module", ko_mod);
    cJSON_AddStringToObject(r, "ko_file", ko_file);
    cJSON_AddStringToObject(r, "ko_version", ko_ver);
    cJSON_AddStringToObject(r, "dev_path", dev_p);
    cJSON_AddBoolToObject(r, "qx_key_set", qx_key ? 1 : 0);
    cJSON_AddNumberToObject(r, "ioctl_read", ioctl_r);
    cJSON_AddNumberToObject(r, "ioctl_write", ioctl_w);
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

static void h_load_gg(int fd, uint32_t seq, cJSON *p) {
    const char *pattern = cJSON_GetObjectItem(p, "pattern") ?
        cJSON_GetObjectItem(p, "pattern")->valuestring : "101.*";
    int strict = cJSON_GetObjectItem(p, "strict") ?
        (cJSON_GetObjectItem(p, "strict")->valueint != 0) : 1;
    const char *want_dir = cJSON_GetObjectItem(p, "dir") ?
        cJSON_GetObjectItem(p, "dir")->valuestring : NULL;

    size_t n = 0;
    gg_ggdir_t *dirs = loader_find_dirs(&n, pattern);
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "dirs");
    int loaded = 0;
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "dir", dirs[i].dir);
        cJSON_AddStringToObject(o, "pkg", dirs[i].pkg);
        cJSON_AddStringToObject(o, "version", dirs[i].version);
        cJSON_AddBoolToObject(o, "family_match", dirs[i].family_match ? 1 : 0);
        cJSON_AddBoolToObject(o, "stub_installed", dirs[i].stub_installed ? 1 : 0);
        cJSON_AddBoolToObject(o, "hook_loaded", dirs[i].hook_loaded ? 1 : 0);
        int do_load = 0;
        if (!loaded && (!want_dir || strcmp(want_dir, dirs[i].dir) == 0)) do_load = 1;
        if (do_load) {
            int rc = loader_load(&dirs[i], pattern, strict);
            if (rc == 0) {
                cJSON_AddBoolToObject(o, "loaded", 1);
                cJSON_AddStringToObject(o, "load_error", "");
                loaded = 1;
            } else {
                cJSON_AddBoolToObject(o, "loaded", 0);
                const char *msg = "load failed";
                switch (-rc) {
                case GGERR_VERSION_MISMATCH: msg = "version family mismatch (pattern: X.Y.* required)"; break;
                case GGERR_NOT_FOUND: msg = "lib5.so not found"; break;
                case GGERR_PERM: msg = "permission denied (need root, /data/adb/vendor_security assets)"; break;
                default: msg = "internal error"; break;
                }
                cJSON_AddStringToObject(o, "load_error", msg);
                if (want_dir) {
                    /* 指定目录失败 → 直接返回错误 */
                    send_err(fd, GGCMD_LOAD_GG, seq, -rc, msg);
                    cJSON_Delete(r);
                    free(dirs);
                    return;
                }
            }
        }
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddBoolToObject(r, "loaded_any", loaded ? 1 : 0);
    free(dirs);
    send_ok(fd, GGCMD_LOAD_GG, seq, r);
    cJSON_Delete(r);
}

static void h_core_versions(int fd, uint32_t seq, cJSON *p) {
    const char *pattern = cJSON_GetObjectItem(p, "pattern") ?
        cJSON_GetObjectItem(p, "pattern")->valuestring : "101.*";
    size_t n = 0;
    gg_ggdir_t *dirs = loader_find_dirs(&n, pattern);
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "dirs");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "dir", dirs[i].dir);
        cJSON_AddStringToObject(o, "pkg", dirs[i].pkg);
        cJSON_AddStringToObject(o, "version", dirs[i].version);
        cJSON_AddBoolToObject(o, "family_match", dirs[i].family_match ? 1 : 0);
        cJSON_AddBoolToObject(o, "stub_installed", dirs[i].stub_installed ? 1 : 0);
        cJSON_AddBoolToObject(o, "hook_loaded", dirs[i].hook_loaded ? 1 : 0);
        cJSON_AddItemToArray(arr, o);
    }
    free(dirs);
    send_ok(fd, GGCMD_CORE_VERSIONS, seq, r);
    cJSON_Delete(r);
}

/* ---------------- 安全 shell（白名单 argv 直执行, 不经 sh, 防注入） ---------------- */
/* shell_split_argv / shell_argv_allowed 见 shell_util.h（与单元测试共享单一事实源）。 */

static void h_device_info(int fd, uint32_t seq, cJSON *p) {
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

/* ---------------- 分发 ---------------- */

/* 订阅事件流（手机悬浮窗）；未订阅客户端不收 GGWF_EVENT 帧 */
static void h_subscribe(int fd, uint32_t seq, cJSON *p, int on) {
    clients_set_subscribed(fd, on);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "subscribed", on ? 1 : 0);
    send_ok(fd, on ? GGCMD_SUBSCRIBE_EVENTS : GGCMD_UNSUBSCRIBE_EVENTS, seq, r);
    cJSON_Delete(r);
}

/* PC 端转发事件（标注等 PC 本地动作）：{"p":{"type":"annotate","data":{...}}}
 * → 事件 {"ev":{"type":"annotate","ts":...,"data":{...}}}（仅订阅端可见） */
static void h_relay_event(int fd, uint32_t seq, cJSON *p) {
    const char *type = cJSON_GetObjectItem(p, "type") ?
        cJSON_GetObjectItem(p, "type")->valuestring : "relay";
    cJSON *data = cJSON_GetObjectItem(p, "data");
    event_simple(type, data ? cJSON_Duplicate(data, 1) : NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    send_ok(fd, GGCMD_RELAY_EVENT, seq, r);
    cJSON_Delete(r);
}

static void dispatch(int fd, gg_wire_header_t *h, const char *payload) {
    uint32_t seq = h->seq;
    cJSON *root = cJSON_Parse(payload);
    cJSON *p = NULL;
    if (root) {
        cJSON *pj = cJSON_GetObjectItem(root, GG_J_REQ);
        if (pj) p = pj;
    }
    if (!p) { send_err(fd, h->cmd, seq, GGERR_BAD_REQUEST, "payload must be {\"p\":{...}}"); if (root) cJSON_Delete(root); return; }

    /* safe_mode 双保险: 单一破坏性命令清单, 在分发前统一拦截。
     * 单一事实源, 消除各 handler 散落检查导致的漏拦
     * (unfreeze/pause/resume/load_gg/watch 原 handler 未查 safe_mode)。
     * 位于锁与事件之外, 拦截时直接返回, 不泄漏引擎状态。 */
    static const uint16_t g_destructive_cmds[] = {
        GGCMD_WRITE, GGCMD_SET_RESULTS, GGCMD_FREEZE, GGCMD_UNFREEZE,
        GGCMD_SHELL, GGCMD_LOAD_GG, GGCMD_PAUSE, GGCMD_RESUME, GGCMD_WATCH
    };
    if (g_eng.safe_mode) {
        for (size_t i = 0; i < sizeof(g_destructive_cmds) / sizeof(g_destructive_cmds[0]); i++) {
            if (h->cmd != g_destructive_cmds[i]) continue;
            /* WATCH 的 poll 是只读查询(取断点命中), 不在 safe_mode 限制内 */
            if (h->cmd == GGCMD_WATCH) {
                const char *op = cJSON_GetObjectItem(p, "op") ? cJSON_GetObjectItem(p, "op")->valuestring : "";
                if (op && strcmp(op, "poll") == 0) break;
            }
            send_err(fd, h->cmd, seq, GGERR_SAFE_MODE, "safe mode: denied");
            if (root) cJSON_Delete(root);
            return;
        }
    }

    uint64_t t0 = now_ms();
    g_last_resp_set = 0;
    event_cmd_start(h->cmd, p);

    /* X3 快速通道：只读健康命令不持引擎锁——大进程搜索（可数十秒）期间
     * 仍可服务 PING/KERNEL_STATUS/LIST_PROCESSES/DEVICE_INFO，客户端/AI
     * 能随时确认存活与搜索进度（此前全部阻塞在搜索后面, 表现为"假死"）。
     * 写操作与其余命令仍走 g_eng.mtx 全串行（帧原子性不变）。
     * 并发安全：stat_* 为 _Atomic（driver.c 原子累加）；整型状态字段
     * __atomic_load 读；last_error 快照容忍撕裂（内容短暂不完整, JSON 合法）；
     * g_last_resp 为线程局部（本线程 handler 写、本线程 cmd_end 读, 无跨线程竞争）。 */
    switch (h->cmd) {
    case GGCMD_PING:           h_ping(fd, seq, p); goto fast_done;
    case GGCMD_KERNEL_STATUS:  h_kernel_status(fd, seq, p); goto fast_done;
    case GGCMD_LIST_PROCESSES: h_list_processes(fd, seq, p); goto fast_done;
    case GGCMD_DEVICE_INFO:    h_device_info(fd, seq, p); goto fast_done;
    default: break;
    }

    pthread_mutex_lock(&g_eng.mtx);   /* 串行化全部引擎操作（含静态搜索缓冲） */
    switch (h->cmd) {
    case GGCMD_PING:               h_ping(fd, seq, p); break;
    case GGCMD_SELF_TEST:          h_self_test(fd, seq, p); break;
    case GGCMD_LIST_PROCESSES:     h_list_processes(fd, seq, p); break;
    case GGCMD_ATTACH:             h_attach(fd, seq, p); break;
    case GGCMD_DETACH:             h_detach(fd, seq, p); break;
    case GGCMD_TARGET_INFO:        h_target_info(fd, seq, p); break;
    case GGCMD_MODULE_LIST:        h_module_list(fd, seq, p); break;
    case GGCMD_SEARCH:             h_search(fd, seq, p); break;
    case GGCMD_SEARCH_UNKNOWN_INIT:h_unknown_init(fd, seq, p); break;
    case GGCMD_SEARCH_REFINE:      h_refine(fd, seq, p); break;
    case GGCMD_SEARCH_RANGE:       h_search_range(fd, seq, p); break;
    case GGCMD_RESULT_COUNT:       h_result_count(fd, seq, p); break;
    case GGCMD_GET_RESULTS:        h_get_results(fd, seq, p); break;
    case GGCMD_CLEAR_RESULTS:      h_clear_results(fd, seq, p); break;
    case GGCMD_READ:               h_read(fd, seq, p); break;
    case GGCMD_READ_BYTES:         h_read_bytes(fd, seq, p); break;
    case GGCMD_HEX_DUMP:           h_read_bytes(fd, seq, p); break;  /* 同 READ_BYTES */
    case GGCMD_WRITE:              h_write(fd, seq, p); break;
    case GGCMD_SET_RESULTS:        h_set_results(fd, seq, p); break;
    case GGCMD_FREEZE:             h_freeze(fd, seq, p); break;
    case GGCMD_UNFREEZE:           h_unfreeze(fd, seq, p); break;
    case GGCMD_MODULE_BASE:        h_module_base(fd, seq, p); break;
    case GGCMD_CALC_OFFSET:        h_calc_offset(fd, seq, p); break;
    case GGCMD_POINTER_SCAN:       h_pointer_scan(fd, seq, p); break;
    case GGCMD_PAUSE:              h_pause_resume(fd, seq, p, 1); break;
    case GGCMD_RESUME:             h_pause_resume(fd, seq, p, 0); break;
    case GGCMD_KERNEL_STATUS:      h_kernel_status(fd, seq, p); break;
    case GGCMD_LOAD_GG:            h_load_gg(fd, seq, p); break;
    case GGCMD_CORE_VERSIONS:      h_core_versions(fd, seq, p); break;
    case GGCMD_SHELL:              h_shell(fd, seq, p); break;
    case GGCMD_DEVICE_INFO:        h_device_info(fd, seq, p); break;
    case GGCMD_WATCH:              h_watch(fd, seq, p); break;
    case GGCMD_DISASSEMBLE:        h_disassemble(fd, seq, p); break;
    case GGCMD_VIEW_MEMORY:        h_view_memory(fd, seq, p); break;
    case GGCMD_RESOLVE_SYMBOL:     h_resolve_symbol(fd, seq, p); break;
    case GGCMD_POINTER_CHAIN:      h_pointer_chain(fd, seq, p); break;
    case GGCMD_SUBSCRIBE_EVENTS:   h_subscribe(fd, seq, p, 1); break;
    case GGCMD_UNSUBSCRIBE_EVENTS: h_subscribe(fd, seq, p, 0); break;
    case GGCMD_RELAY_EVENT:        h_relay_event(fd, seq, p); break;
    case GGCMD_GHOST_TAP:          h_ghost_tap(fd, seq, p); break;
    case GGCMD_GHOST_SWIPE:        h_ghost_swipe(fd, seq, p); break;
    default:
        send_err(fd, h->cmd, seq, GGERR_BAD_REQUEST, "unknown command");
        break;
    }
    /* cmd_end 在锁内发出（线程局部 g_last_resp 与 handler 同线程, 无跨线程竞争） */
    event_cmd_end(h->cmd, t0);
    pthread_mutex_unlock(&g_eng.mtx);

    if (root) cJSON_Delete(root);
    return;

fast_done:
    event_cmd_end(h->cmd, t0);   /* 读本线程 handler 写入的 g_last_resp */
    if (root) cJSON_Delete(root);
}

/* ---------------- 连接处理 ---------------- */

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    struct timeval tv = { .tv_sec = 300, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 5;   /* 发送超时收紧: 事件广播 best-effort, 慢客户端最多拖 5s */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    clients_register(fd);   /* 事件广播注册表（默认未订阅） */

    for (;;) {
        gg_wire_header_t h;
        if (read_full(fd, &h, sizeof(h)) < 0) break;
        log_msg("conn: magic=%08x ver=%u cmd=%u flags=%u len=%u",
                (unsigned)h.magic, (unsigned)h.ver, (unsigned)h.cmd,
                (unsigned)h.flags, (unsigned)h.payload_len);
        if (h.magic != GGWM_MAGIC || h.ver != GGWM_VERSION) break;
        if (h.payload_len > GGWM_MAX_PAYLOAD) break;   /* 单帧负载上限 1MiB（见 wire_protocol.h） */
        char *payload = malloc(h.payload_len + 1);
        if (!payload) break;
        if (h.payload_len && read_full(fd, payload, h.payload_len) < 0) {
            log_msg("conn: payload read failed (len=%u err=%d %s)",
                    (unsigned)h.payload_len, errno, strerror(errno));
            free(payload);
            break;
        }
        payload[h.payload_len] = 0;
        /* 请求负载解密（客户端若开 GGWF_ENC 则 XOR; 未开保持明文, 向后兼容） */
        if ((h.flags & GGWF_ENC) && h.payload_len)
            gg_xor_payload((unsigned char *)payload, h.payload_len);
        dispatch(fd, &h, payload);
        free(payload);
    }
    clients_unregister(fd);
    close(fd);
    return NULL;
}

/* ---------------- 主入口 ---------------- */

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "设备端内核守护（KO 驱动方案）\n"
        "用法: %s [--port 34127]\n"
        "           [--driver hook|dev|qx|rwproc] # KO 后端类型（默认 hook）\n"
        "           [--ko-dir /data/adb/vendor_security/ko]\n"
        "           [--ko-file name.ko]           # 指定 KO（空=按版本族自动选）\n"
        "           [--ko-pattern 6.12.*]         # KO 版本族模式（同族可加载）\n"
        "           [--ko-strict 0|1]             # 族外拒绝（默认 1）\n"
        "           [--dev /dev/xxx]              # dev 型 KO 的设备节点\n"
        "           [--ioctl-read 0x7e1a] [--ioctl-write 0x7e1b]\n"
        "           [--qx-key <key>]              # qx 型驱动 init_key 密钥（可选）\n"
        "           [--rwproc-key <key>]          # rwproc33 内存模块 proc 节点 auth key\n"
        "           [--rwproc-bp-key <key>]       # rwproc33 断点模块 proc 节点 auth key\n"
        "           [--log /path/log] [--safe] [--shell]\n",
        argv0);
}

/* ---- 进程伪装：prctl 改名 + 重写 cmdline（argv 区） ----
 * ACE 类反作弊扫描 /proc/<pid>/comm 与 /proc/<pid>/cmdline：
 *   二进制名=dspbridge（厂商风格），comm 与之一致最自然（不 prctl 改名）。
 *   cmdline 清空全部参数（--driver rwproc/--rwproc-key 等是硬特征）与
 *   环境变量区，只保留 argv[0]（程序名），/proc/PID/cmdline 显示 "dspbridge"。
 * 必须在参数解析完成后调用（会清空 argv[1..]，之后不再引用 argv）。 */
static void hide_process(int argc, char **argv) {
    extern char **environ;
    for (int i = 1; i < argc; i++) {
        if (argv[i]) memset(argv[i], 0, strlen(argv[i]));
    }
    for (int i = 0; environ && environ[i]; i++)
        memset(environ[i], 0, strlen(environ[i]));
}

int main(int argc, char **argv) {
    /* 参数值必须拷贝到独立 buffer：hide_process() 会清空 argv[] 内存，
       dtype 等若直接指向 argv[i]，被 memset 后变空串 → strcmp 全失败 →
       误走默认 hook 型（用户态 process_vm_readv，违反内核通道纪律）。
       修复（2026-08-19）：解析时一律 snprintf 到本地 buffer。 */
    static char dtype_buf[16], ko_dir_buf[256], ko_file_buf[512];
    static char dev_path_buf[128], qx_key_buf[0x100];
    static char rwproc_key_buf[128], rwproc_bp_key_buf[128], logpath_buf[256];
    static char ko_pattern_buf[64];
    const char *dtype = dtype_buf, *ko_dir = ko_dir_buf, *ko_file = ko_file_buf;
    const char *dev_path = dev_path_buf, *qx_key = qx_key_buf;
    const char *rwproc_key = rwproc_key_buf, *rwproc_bp_key = rwproc_bp_key_buf;
    const char *logpath = logpath_buf, *ko_pattern = ko_pattern_buf;
    int port = 34127;            /* 非常见高位端口，避免 19100 等默认值被扫 */
    snprintf(dtype_buf, sizeof(dtype_buf), "%s", "hook");
    snprintf(ko_dir_buf, sizeof(ko_dir_buf), "%s",
             "/data/adb/vendor_security/ko");  /* 避开 /data/local/tmp（ACE 重点扫描） */
    snprintf(ko_pattern_buf, sizeof(ko_pattern_buf), "%s", "6.12.*");
    ko_file_buf[0] = dev_path_buf[0] = qx_key_buf[0] = 0;
    rwproc_key_buf[0] = rwproc_bp_key_buf[0] = logpath_buf[0] = 0;
    int ko_strict = 1;
    int ioctl_read = 0x7e1a, ioctl_write = 0x7e1b;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--driver") == 0 && i + 1 < argc)
            snprintf(dtype_buf, sizeof(dtype_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--ko-dir") == 0 && i + 1 < argc)
            snprintf(ko_dir_buf, sizeof(ko_dir_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--ko-file") == 0 && i + 1 < argc)
            snprintf(ko_file_buf, sizeof(ko_file_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--ko-pattern") == 0 && i + 1 < argc)
            snprintf(ko_pattern_buf, sizeof(ko_pattern_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--ko-strict") == 0 && i + 1 < argc) ko_strict = atoi(argv[++i]);
        else if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc)
            snprintf(dev_path_buf, sizeof(dev_path_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--ioctl-read") == 0 && i + 1 < argc) ioctl_read = (int)strtol(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--ioctl-write") == 0 && i + 1 < argc) ioctl_write = (int)strtol(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--qx-key") == 0 && i + 1 < argc)
            snprintf(qx_key_buf, sizeof(qx_key_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--rwproc-key") == 0 && i + 1 < argc)
            snprintf(rwproc_key_buf, sizeof(rwproc_key_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--rwproc-bp-key") == 0 && i + 1 < argc)
            snprintf(rwproc_bp_key_buf, sizeof(rwproc_bp_key_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            snprintf(logpath_buf, sizeof(logpath_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--safe") == 0) g_eng.safe_mode = 1;
        else if (strcmp(argv[i], "--shell") == 0) g_eng.shell_enabled = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    hide_process(argc, argv);   /* 清 cmdline 参数与环境（防 ACE 扫 /proc/PID/cmdline） */

    memset(&g_eng, 0, sizeof(g_eng));
    g_eng.session_pid = -1;
    g_eng.result_cap = GG_RESULT_CAP_DEFAULT;
    pthread_mutex_init(&g_eng.mtx, NULL);
    if (logpath) g_eng.logf = fopen(logpath, "a");
    log_msg("starting (port=%d driver=%s ko_dir=%s pattern=%s strict=%d safe=%d)",
            port, dtype, ko_dir, ko_pattern, ko_strict, g_eng.safe_mode);

    if (strcmp(dtype, "qx") == 0) {
        driver_init_qx(&g_eng.drv, qx_key);
    } else if (strcmp(dtype, "rwproc") == 0) {
        driver_init_rwproc(&g_eng.drv, rwproc_key, rwproc_bp_key);
    } else {
        driver_init(&g_eng.drv, dtype, dev_path, ioctl_read, ioctl_write);
    }
    freeze_init(&g_eng.freeze, &g_eng.drv);
    results_init(&g_eng.results, g_eng.result_cap);

    /* 加载/对接驱动（qx/rwproc 型为发现+对接；其余为 insmod） */
    int lrc;
    if (g_eng.drv.ko_type == KO_TYPE_QX) {
        lrc = qx_attach(&g_eng.drv);
        if (lrc != 0) {
            log_msg("qx_attach failed: %s", g_eng.drv.last_error);
        } else {
            log_msg("qx attached: %s", g_eng.drv.ko_file);
        }
    } else if (g_eng.drv.ko_type == KO_TYPE_RWPROC) {
        lrc = rwproc_attach(&g_eng.drv);
        if (lrc != 0) {
            log_msg("rwproc_attach failed: %s", g_eng.drv.last_error);
        } else {
            log_msg("rwproc attached: %s", g_eng.drv.ko_file);
        }
    } else {
        lrc = ko_load(&g_eng.drv, ko_dir, ko_file, ko_pattern, ko_strict);
        if (lrc != 0) {
            log_msg("ko_load failed: %s", g_eng.drv.last_error);
        } else {
            log_msg("ko loaded: %s (module=%s version=%s type=%s)",
                    g_eng.drv.ko_file, g_eng.drv.ko_module, g_eng.drv.ko_version,
                    g_eng.drv.ko_type == KO_TYPE_DEV ? "dev" : "hook");
        }
    }

    int st = driver_self_test(&g_eng.drv);
    log_msg("driver self-test: %s (%s)",
            st > 0 ? "channel OK" : (st == 0 ? "degraded" : "UNAVAILABLE"),
            driver_state_str(g_eng.drv.state));
    if (st < 0) {
        log_msg("WARNING: all memory channels unavailable; attach/search will fail");
    }

    /* 同步 GG hook 的驱动协议环境（loader_load 时写入 lib5_hook.conf）。
     * rwproc33 后端暂不接入 GG 核心 hook（libKernelGg.so 仅支持 dev/qx），
     * 此时 GG 核心加载回退 process_vm_readv，agentd 自身引擎不受影响。 */
    {
        const char *proto = g_eng.drv.ko_type == KO_TYPE_QX ? "qx" :
                            g_eng.drv.ko_type == KO_TYPE_DEV ? "dev" : "hook";
        loader_set_hook_env(proto, g_eng.drv.dev_path,
                            g_eng.drv.qx_have_key ? g_eng.drv.qx_key : NULL);
    }

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }
    /* 关键: 客户端断开后写响应会触发 SIGPIPE, 默认行为是杀死整个进程。
     * 服务端必须忽略 SIGPIPE, 让 send/recv 返回 EPIPE 错误即可。 */
    signal(SIGPIPE, SIG_IGN);
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    if (listen(sfd, 8) < 0) { perror("listen"); return 1; }
    log_msg("listening on 127.0.0.1:%d", port);

    /* 进程伪装：所有 argv 字符串（dtype/keys/logpath/ko_dir 等）已消费完毕，
     * 此时清 argv 区不再影响任何指针。⚠ 必须放在参数解析与驱动初始化之后：
     * 提前清空会导致 dtype/key 失效 → 静默降级 hook 型 ko_load + 日志 fopen 失败。 */
    hide_process(argc, argv);

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            /* 永不退出: 偶发错误(EMFILE 等)休眠后继续 */
            if (errno == EINTR) continue;
            log_msg("accept failed: %s (retrying)", strerror(errno));
            usleep(100000);
            continue;
        }
        pthread_t th;
        if (pthread_create(&th, NULL, conn_thread, (void *)(intptr_t)cfd) != 0) {
            close(cfd);
            continue;
        }
        pthread_detach(th);
    }
    close(sfd);
    freeze_stop(&g_eng.freeze);
    results_clear(&g_eng.results);
    driver_close(&g_eng.drv);
    return 0;
}
