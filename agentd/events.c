/* events.c — 客户端注册表 + 事件广播（自 main.c 拆出，2026-10-23，Phase 1·②）
 *
 * 这是 agentd 里最大的独立子系统：谁连着、谁订阅了什么、事件怎么扇出去。
 * 与业务 handler 无关，只依赖帧收发层（wire_io.c 的 send_frame）。
 *
 * 三条不能动的约束（每条都对应过一次事故）：
 *
 *   1. **broadcast_event 的 dup() + 锁外发送顺序别改。** 现在的结构是：
 *      锁内把订阅者 fd 列表 dup() 出来 → 解锁 → 逐个发。目的是防止一个僵死
 *      的订阅者（不读数据、send 阻塞）在持锁期间把全局卡死。改成锁内发送
 *      会让某个手机端掉线拖死整个 agentd。
 *
 *   2. **g_clients_mtx 只保护注册表，不是发送锁。** 真正防交叉的是
 *      wire_io.c 里的 g_send_mtx（出站帧全局串行）。
 *
 *   3. **event_cmd_end 读的是 _Thread_local g_last_resp。** 每连接线程独立，
 *      所以"上一个命令的响应摘要"不会串到别的连接上。声明在
 *      engine_internal.h 里，必须保留 _Thread_local。
 *
 * cmd_name 是"cmd 码 → MCP 风格命令名"的映射，dispatch 的日志和 cmd_end
 * 事件都用它，所以它跟着一起搬过来了。
 */
#include "engine_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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
    int frag_ok;                 /* AUTH 时声明能重组分片响应（"frag":1） */
    struct gg_client *next;
} gg_client_t;

static gg_client_t *g_clients = NULL;
static pthread_mutex_t g_clients_mtx = PTHREAD_MUTEX_INITIALIZER;

/* 客户端 socket 的发送超时。默认无超时：对端不读（客户端被 SIGSTOP、进程僵死、
 * 只 connect 不 recv 的扫描器）时 send() 会在内核缓冲满后**永久阻塞**。
 * 由于 send_frame 持 g_send_mtx（全局串行），一个这样的连接就能让所有响应与
 * 事件广播一起停住。1s 对正常客户端（响应通常 <10ms）完全无感，只在僵死时兜底。 */
#define GG_SOCK_SEND_TIMEOUT_MS 1000

void client_set_send_timeout(int fd) {
    struct timeval tv;
    tv.tv_sec  = GG_SOCK_SEND_TIMEOUT_MS / 1000;
    tv.tv_usec = (GG_SOCK_SEND_TIMEOUT_MS % 1000) * 1000;
    /* 非 socket（单元测试用管道）会失败，忽略即可 */
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void clients_register(int fd) {
    client_set_send_timeout(fd);
    pthread_mutex_lock(&g_clients_mtx);
    gg_client_t *c = malloc(sizeof(*c));
    if (c) {
        c->fd = fd;
        c->subscribed = 0;
        c->frag_ok = 0;
        c->next = g_clients;
        g_clients = c;
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

void clients_unregister(int fd) {
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

/* 标记/查询「该连接能重组分片响应」。查询只在 send_ok 的超长分支用。 */
void clients_set_frag(int fd, int on) {
    pthread_mutex_lock(&g_clients_mtx);
    for (gg_client_t *c = g_clients; c; c = c->next)
        if (c->fd == fd) { c->frag_ok = on ? 1 : 0; break; }
    pthread_mutex_unlock(&g_clients_mtx);
}

int client_supports_frag(int fd) {
    int ok = 0;
    pthread_mutex_lock(&g_clients_mtx);
    for (gg_client_t *c = g_clients; c; c = c->next)
        if (c->fd == fd) { ok = c->frag_ok; break; }
    pthread_mutex_unlock(&g_clients_mtx);
    return ok;
}

void clients_set_subscribed(int fd, int on) {
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
            c->frag_ok = 0;
            c->next = g_clients;
            g_clients = c;
        }
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 发送失败的订阅者从注册表摘除（fd 由连接线程关闭）。 */
void clients_drop(int fd) {
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

#define GG_EVENT_MAX_SUBS 32

/* 广播一条事件。ev 所有权移交（本函数负责 cJSON_Delete）。
 *
 * ⚠ 发送必须在 g_clients_mtx **之外**。send_frame 是阻塞写：订阅者不读
 * （悬浮窗被冻结、手机端进程僵死、只订阅不读的调试客户端）时，写会卡在
 * 内核缓冲满上。持注册表锁卡住 = clients_register / subscribe 全部堵死；
 * 而本函数又被 dispatch 调用（那里持 g_eng.mtx），连锁起来就是整个守护进程
 * "假死"——进程在、端口在、所有请求无响应，且从外部看不出是订阅者拖的。
 *
 * 做法：锁内只做快照 + dup，锁外逐条发送。
 * dup 是必需的：原 fd 归连接线程所有，广播期间可能被 close，而 fd 号会立刻
 * 被复用——拿裸 fd 去写就可能写到别的连接上（帧内容串线）。dup 出来的句柄
 * 指向同一 socket，原 fd 关闭不影响它，写完 close 掉即可。
 * 发送超时在 clients_register 里统一设过（SO_SNDTIMEO），dup 副本共享该 socket
 * 选项，所以慢订阅者最多拖 GG_SOCK_SEND_TIMEOUT_MS 就被判失败并摘除。 */
void broadcast_event(cJSON *ev) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, GG_J_EVT, ev);
    char *s = cJSON_PrintUnformatted(root);
    size_t len = s ? strlen(s) : 0;
    if (!len) {
        free(s);
        cJSON_Delete(root);
        return;
    }

    struct { int orig; int dupfd; } subs[GG_EVENT_MAX_SUBS];
    size_t n = 0;
    int truncated = 0;
    pthread_mutex_lock(&g_clients_mtx);
    for (gg_client_t *c = g_clients; c; c = c->next) {
        if (!c->subscribed) continue;
        if (n >= GG_EVENT_MAX_SUBS) { truncated = 1; break; }
        int d = dup(c->fd);
        if (d < 0) continue;          /* 已被关闭：跳过，下次广播自然清掉 */
        subs[n].orig  = c->fd;
        subs[n].dupfd = d;
        n++;
    }
    pthread_mutex_unlock(&g_clients_mtx);

    if (truncated)
        log_msg("broadcast: more than %d subscribers, some event copies dropped",
                GG_EVENT_MAX_SUBS);

    for (size_t i = 0; i < n; i++) {
        if (send_frame(subs[i].dupfd, 0, 0, GGWF_EVENT, s, len) < 0) {
            /* best-effort：摘掉该项（发送失败多半是连接已断或对端不读）。
             * 按原 fd 摘除而不是 dupfd —— 二者号不同，且新连接初始未订阅，
             * 误删也只会让它下次 subscribe 时重新登记。 */
            clients_drop(subs[i].orig);
        }
        close(subs[i].dupfd);
    }
    free(s);
    cJSON_Delete(root);
}

/* 事件构造小助手 */
void event_simple(const char *type, cJSON *data) {
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", type);
    cJSON_AddNumberToObject(ev, "ts", (double)now_ms());
    cJSON_AddItemToObject(ev, "data", data ? data : cJSON_CreateObject());
    broadcast_event(ev);
}

/* cmd 码 → MCP 风格命令名（日志页展示） */
const char *cmd_name(uint16_t cmd) {
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
    /* 命令码保留（wire ABI 不可回收），实现已删除 */
    case GGCMD_LOAD_GG:            return "gg_kernel_load_gg (removed)";
    case GGCMD_CORE_VERSIONS:      return "gg_core_versions (removed)";
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
    case GGCMD_AUTH:               return "gg_auth";
    default:                       return "gg_unknown";
    }
}

/* cmd_start: 请求进入（参数快照，>4KB 截断为 {"truncated":true}） */
void event_cmd_start(uint16_t cmd, cJSON *p) {
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
void event_cmd_end(uint16_t cmd, uint64_t t0) {
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "cmd_end");
    cJSON_AddNumberToObject(ev, "ts", (double)now_ms());
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "cmd", cmd_name(cmd));
    cJSON_AddBoolToObject(data, "success", g_last_resp.set ? (g_last_resp.ok ? 1 : 0) : 0);
    if (g_last_resp.set) {
        if (!g_last_resp.truncated) {
            cJSON *res = cJSON_Parse(g_last_resp.buf);
            if (res) cJSON_AddItemToObject(data, "result", res);
            else cJSON_AddStringToObject(data, "result_raw", g_last_resp.buf);
        } else {
            /* 超长响应（典型：gg_get_results 的 610–740KB）。存档里只有前缀，
             * 硬 parse 会失败并让 result 静默消失 —— 那正是原缺陷。这里改成
             * 显式告知：多大、前面长什么样，订阅端据此决定显示摘要还是去拉详情。 */
            cJSON_AddBoolToObject(data, "result_truncated", 1);
            cJSON_AddNumberToObject(data, "result_bytes", (double)g_last_resp.full_len);
            cJSON_AddStringToObject(data, "result_preview", g_last_resp.buf);
        }
    }
    cJSON_AddNumberToObject(data, "duration_ms", (double)(now_ms() - t0));
    cJSON_AddItemToObject(ev, "data", data);
    broadcast_event(ev);
}

/* session_change: attach/detach */
void event_session_change(const char *state, pid_t pid, const char *name) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "state", state);
    cJSON_AddNumberToObject(d, "pid", pid);
    cJSON_AddStringToObject(d, "name", name ? name : "");
    event_simple("session_change", d);
}

/* search_update: 搜索结果数变化 */
void event_search_update(long prev_count, long cur_count) {
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
void event_results_update(void) {
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
cJSON *watch_hit_json(const gg_watch_hit_t *h) {
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
void event_watch_hit(const gg_watch_hit_t *h) {
    event_simple("watch_hit", watch_hit_json(h));
}
