/* main.c — 设备端内核守护进程
 *
 * 监听 127.0.0.1:<port>，经 adb forward 供 PC 端 gg-mcp-server 调用。
 * 协议见 shared/wire_protocol.h（20 字节二进制头 + JSON 负载）。
 *
 * 用法: [--port 34127] [--driver ioctl|supercall|auto]
 *                [--supercall 45] [--log /path/log] [--safe]
 */
#include "../shared/wire_protocol.h"
#include "disasm.h"
#include "driver.h"
#include "elfsym.h"
#include "freeze.h"
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
#include <netinet/tcp.h>   /* TCP_NODELAY（NDK 的 arpa/inet.h 不带） */
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
#include <sys/time.h>
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

/* 硬件断点：默认由运行时探测把关，不再无条件关闭。
 *
 * 历史：afei 的 hwBreakpointProc prebuilt 有两处致命缺陷 —— 命令码从 0 起始
 * （driver.h 按 1 起始发，OPEN_PROCESS(1) 会落到 CLOSE_PROCESS →
 * put_pid((struct pid*)1) 解引用野指针），命中回调用 mutex 而非 raw_spinlock
 * （调试异常上下文里睡眠 → scheduling while atomic）。实测一调
 * rwproc_hwbp_start 就整机重启。
 *
 * 现在用 scripts/build_hwbp_ko.sh 自编译并修好四条缺陷（含执行断点命中后
 * 不挪位导致的无限重复触发），deploy.ps1 会自动用它覆盖 prebuilt。
 * 探测见下方 g_hwbp_probe。 */
int g_hwbp_allowed = 0;   /* --hwbp：跳过探测强制启用（自担风险） */
/* 模块命令码基准探测结果：-1 未探测、0 不安全、1 可用。
 * 自编译模块 GET_NUM_BRPS 返回 6；afei 的 0 起始 prebuilt 返回 4（那里
 * cmd=3 其实是 GET_NUM_WRPS）。只有探到 6 才放行。 */
int g_hwbp_probe = -1;
/* 探测的原始返回值，只用于把错误信息说准：-1 是「节点没开/开错」，
 * 4 才是「0 起始枚举的旧模块」。两者混在一句话里会把排查方向带偏
 * （AGENTS 第 25 条就是这么误判成模块命令码不一致的）。 */
int g_hwbp_probe_rc = -1;

/* ---------------- 握手鉴权 + 并发闸门 ---------------- */


/* 并发连接上限：accept 后无限 pthread_create，本地进程可用几千连接耗尽
 * 线程栈与 fd 把 agentd 拖死（listen 的 backlog 只限制未 accept 的队列）。 */
#define GG_MAX_CLIENTS 8
static atomic_int g_active_clients;



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

/* 严格解析地址字符串；失败返回 0。详见 engine_internal.h 的说明。 */
int parse_u64_strict(const char *s, uint64_t *out) {
    if (!s || !*s) return 0;
    /* strtoull 会把前导 '-' 按模运算吃掉（"-1" → 0xFFFFFFFFFFFFFFFF）且不设 ERANGE，
     * 不拦就把写错的负地址当成一个巨大的合法地址。 */
    const char *q = s;
    while (*q == ' ' || *q == '\t') q++;
    if (*q == '-') return 0;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE) return 0;
    if (out) *out = (uint64_t)v;
    return 1;
}

/* force 参数: 1 = 允许驱动临时改 PTE 强制读（rwproc33 的 is_force_read）。
 * 只接受 0/1，不把任意整数塞给驱动（那是未定义语义）。
 * 背景：PC 端 schema 与 handler 一直在传 force，agentd 早期把它丢掉了，
 * 表现为"读未驻留页失败，加 force 也没用"。 */
int parse_force(const cJSON *p) {
    const cJSON *j = cJSON_GetObjectItem(p, "force");
    if (!j) return 0;
    if (cJSON_IsBool(j)) return cJSON_IsTrue(j) ? 1 : 0;
    if (cJSON_IsNumber(j)) return j->valueint ? 1 : 0;
    if (cJSON_IsString(j) && j->valuestring) return atoi(j->valuestring) ? 1 : 0;
    return 0;
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

/* ---------------- 参数解析 ---------------- */

uint64_t parse_value_str(int vtype, const char *s, int *ok) {
    *ok = 0;
    if (!s || !*s) return 0;
    char *end = NULL;
    if (vtype == VT_FLOAT || vtype == VT_DOUBLE) {
        double d = strtod(s, &end);
        if (end == s || *end != '\0') return 0;
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
    /* endptr 必须吃到结尾：只判 end==s 时 "123abc" / "1.5x" 会被当成 123 / 1
     * 当「合法值」写进目标进程，调用方永远看不出自己参数写错了。 */
    if (end == s || *end != '\0' || errno == ERANGE) return 0;
    *ok = 1;
    return (uint64_t)v;
}

/* 取 string / number / bool 形式的标量参数，统一转成字符串。
 * 存在原因：cJSON 数字节点的 valuestring 恒为 NULL。AI 客户端发
 * {"value":100} 是常态，直接取 valuestring 会得到 NULL —— 轻则误报
 * "value required"，重则把它交给 strtoull 直接段错误。 */
const char *j_scalar_str(cJSON *node, char *buf, size_t bufsz) {
    if (!node) return NULL;
    if (cJSON_IsString(node)) return node->valuestring;
    if (cJSON_IsNumber(node)) {
        /* %.17g 保证 double 可无损回读（int 类型下 "1.5" 会被严格解析拒绝，
         * 这正确：整数型不该接受小数）。 */
        snprintf(buf, bufsz, "%.17g", node->valuedouble);
        return buf;
    }
    if (cJSON_IsBool(node)) return cJSON_IsTrue(node) ? "1" : "0";
    return NULL;
}

/* 从 cJSON 节点取地址，接受 "0x..." 字符串与 JSON 数字两种形态。
 * 嵌套在数组里的地址（set_results 的 values[].addr）走不到 dispatch 的
 * 顶层地址闸门，必须在这里单独兜住，否则同样是 strtoull(NULL)。 */
int j_addr_u64(cJSON *node, uint64_t *out) {
    if (!node) return 0;
    if (cJSON_IsString(node)) return parse_u64_strict(node->valuestring, out);
    if (cJSON_IsNumber(node)) {
        double dv = node->valuedouble;
        if (!(dv >= 0) || dv >= 18446744073709551616.0) return 0;   /* 2^64 */
        if (out) *out = (uint64_t)dv;
        return 1;
    }
    return 0;
}

/* ---------------- 各命令处理（已全部外迁） ----------------
 * proc / analyze / touch / status / search / mem / view / shell 各域 handler
 * 都在对应的 cmd_*.c 里（声明见 engine_internal.h）。本文件只剩 dispatch
 * 与连接生命周期。 */


static void dispatch(int fd, gg_wire_header_t *h, const char *payload, int *authed) {
    uint32_t seq = h->seq;
    cJSON *root = cJSON_Parse(payload);
    cJSON *p = NULL;
    if (root) {
        cJSON *pj = cJSON_GetObjectItem(root, GG_J_REQ);
        if (pj) p = pj;
    }
    if (!p) { send_err(fd, h->cmd, seq, GGERR_BAD_REQUEST, "payload must be {\"p\":{...}}"); if (root) cJSON_Delete(root); return; }

    /* 握手门：未认证的连接只放行 AUTH 与 PING（PING 无副作用，用于探活与部署自检）。
     * 位置在 payload 解析之后、safe_mode 之前 —— AUTH 需要读 {"p":{"key":...}}，
     * 且认证失败时不该暴露 safe_mode / 驱动状态等引擎信息。
     * 这一层堵的是「本地任意 App 连上回环端口即获得 root 能力」的面。 */
    if (h->cmd == GGCMD_AUTH) {
        h_auth(fd, seq, p, authed);
        cJSON_Delete(root);
        return;
    }
    if (!*authed && h->cmd != GGCMD_PING) {
        log_msg("gate: cmd %u rejected (unauthenticated)", (unsigned)h->cmd);
        send_err(fd, h->cmd, seq, GGERR_PERM, "wire auth required");
        cJSON_Delete(root);
        return;
    }

    /* 地址类参数统一校验：agentd 内有 17 处裸 strtoull(s, NULL, 0)，
     * 非法输入会静默得到 0，随后拿着地址 0 去读内存 —— 调用方看到的错误是
     * 「读失败」而不是「地址写错了」，排查成本极高。
     * 集中在这里拦一次，比在 17 个 handler 里各写一遍可靠，也不会漏。
     * PC 端已统一下发 "0x..." 字符串；这里同时容忍 JSON 数字形式以兼容旧客户端。 */
    {
        static const char *const addr_keys[] = {"addr", "base", "pc", "fp", "sp", "lr", NULL};
        for (int i = 0; addr_keys[i]; i++) {
            cJSON *a = cJSON_GetObjectItem(p, addr_keys[i]);
            if (!a) continue;
            int ok = 0;
            if (cJSON_IsString(a)) {
                ok = parse_u64_strict(a->valuestring, NULL);
            } else if (cJSON_IsNumber(a)) {
                /* 数字形式必须就地规范化为 "0x..." 字符串，不能只「放行」：
                 * 下游 handler 一律按 ->valuestring 取地址，而 cJSON 数字节点的
                 * valuestring 恒为 NULL —— 放行而不转换等于把 NULL 交给 strtoull
                 * （gg_watch_inject 曾因此让 agentd 直接段错误）。
                 * double 只有 53 位整数精度：更大的地址本就该用字符串传，
                 * 这里按客户端实际给出的值规范化，不替它猜。 */
                double dv = a->valuedouble;
                if (dv >= 0 && dv < 18446744073709551616.0) {
                    char hexbuf[32];
                    snprintf(hexbuf, sizeof(hexbuf), "0x%llx", (unsigned long long)dv);
                    ok = cJSON_ReplaceItemInObjectCaseSensitive(
                             p, addr_keys[i], cJSON_CreateString(hexbuf)) ? 1 : 0;
                }
            }
            if (!ok) {
                log_msg("gate: cmd %u bad address arg '%s'", (unsigned)h->cmd, addr_keys[i]);
                send_err(fd, h->cmd, seq, GGERR_BAD_REQUEST,
                         "invalid address argument (expected 0x-prefixed string)");
                cJSON_Delete(root);
                return;
            }
        }
    }

    /* safe_mode 双保险: 单一破坏性命令清单, 在分发前统一拦截。
     * 单一事实源, 消除各 handler 散落检查导致的漏拦
     * (unfreeze/pause/resume/load_gg/watch 原 handler 未查 safe_mode)。
     * 位于锁与事件之外, 拦截时直接返回, 不泄漏引擎状态。 */
    static const uint16_t g_destructive_cmds[] = {
        GGCMD_WRITE, GGCMD_SET_RESULTS, GGCMD_FREEZE, GGCMD_UNFREEZE,
        GGCMD_SHELL, GGCMD_PAUSE, GGCMD_RESUME, GGCMD_WATCH,
        /* 虚拟触摸会操作系统 UI（可替用户点掉授权弹窗、走完应用内确认），
         * 属「改变外部状态」而非只读。旧清单只按「写内存类」列举，漏了这两个，
         * 于是 --safe 自称只读却仍能注入输入。 */
        GGCMD_GHOST_TAP, GGCMD_GHOST_SWIPE
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

    /* X3 快速通道：只读健康命令不持引擎锁——大进程搜索（可数十秒）期间
     * 仍可服务 PING/KERNEL_STATUS/LIST_PROCESSES/DEVICE_INFO，客户端/AI
     * 能随时确认存活与搜索进度（此前全部阻塞在搜索后面, 表现为"假死"）。
     * 写操作与其余命令仍走 g_eng.mtx 全串行（帧原子性不变）。
     * 并发安全：stat_* 为 _Atomic（driver.c 原子累加）；整型状态字段
     * __atomic_load 读；last_error 快照容忍撕裂（内容短暂不完整, JSON 合法）。 */
    switch (h->cmd) {
    case GGCMD_PING:           h_ping(fd, seq, p); goto fast_done;
    case GGCMD_KERNEL_STATUS:  h_kernel_status(fd, seq, p); goto fast_done;
    case GGCMD_LIST_PROCESSES: h_list_processes(fd, seq, p); goto fast_done;
    case GGCMD_DEVICE_INFO:    h_device_info(fd, seq, p); goto fast_done;
    default: break;
    }

    pthread_mutex_lock(&g_eng.mtx);   /* 串行化全部引擎操作（含静态搜索缓冲） */
    /* 会话自愈：目标已退出 / pid 被复用就在这里清掉会话（见 session_guard）。
     * 放在锁内是因为它要改 session_* 并停 freeze 线程，与 attach/detach 同临界区；
     * 放在分发前是为了让所有依赖会话的 handler 都受保护，而不是逐个加检查。
     * 返回值刻意不用：清掉之后 handler 自己会回 GG_NO_SESSION，走的是"从未
     * attach"的同一条路径，客户端不需要区分两种情况。 */
    session_guard();
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
    case GGCMD_READ_BYTES:         h_read_bytes(fd, h->cmd, seq, p); break;
    case GGCMD_HEX_DUMP:           h_read_bytes(fd, h->cmd, seq, p); break;  /* 同 READ_BYTES，仅 cmd 回填不同 */
    case GGCMD_WRITE:              h_write(fd, seq, p); break;
    case GGCMD_SET_RESULTS:        h_set_results(fd, seq, p); break;
    case GGCMD_FREEZE:             h_freeze(fd, seq, p); break;
    case GGCMD_UNFREEZE:           h_unfreeze(fd, seq, p); break;
    case GGCMD_FREEZE_RESULTS:     h_freeze_results(fd, seq, p); break;
    case GGCMD_FILTER_RESULTS:     h_filter_results(fd, seq, p); break;
    case GGCMD_SCREENSHOT_ROI:     h_screenshot_roi(fd, seq, p); break;
    case GGCMD_MODULE_BASE:        h_module_base(fd, seq, p); break;
    case GGCMD_CALC_OFFSET:        h_calc_offset(fd, seq, p); break;
    case GGCMD_POINTER_SCAN:       h_pointer_scan(fd, seq, p); break;
    case GGCMD_PAUSE:              h_pause_resume(fd, seq, p, 1); break;
    case GGCMD_RESUME:             h_pause_resume(fd, seq, p, 0); break;
    case GGCMD_KERNEL_STATUS:      h_kernel_status(fd, seq, p); break;
    /* 命令码 28/29 保留占位：GG 核心注入链路（lib5_stub + libKernelGg.so）
     * 已于 2026-10-21 整条删除 —— 该方案实测从未成功，且会改写用户的 lib5.so。
     * 保留枚举值只为不改变后续命令码（wire ABI），旧客户端调用得到明确错误。 */
    case GGCMD_LOAD_GG:
    case GGCMD_CORE_VERSIONS:
        send_err(fd, h->cmd, seq, GGERR_BAD_REQUEST,
                 "command removed: GG core injection (lib5_stub/libKernelGg) was deleted");
        break;
    case GGCMD_SHELL:              h_shell(fd, seq, p); break;
    case GGCMD_DEVICE_INFO:        h_device_info(fd, seq, p); break;
    case GGCMD_WATCH:              h_watch(fd, seq, p); break;
    case GGCMD_DISASSEMBLE:        h_disassemble(fd, seq, p); break;
    case GGCMD_AOB_RESOLVE:        h_aob_resolve(fd, seq, p); break;
    case GGCMD_VIEW_MEMORY:        h_view_memory(fd, seq, p); break;
    case GGCMD_RESOLVE_SYMBOL:     h_resolve_symbol(fd, seq, p); break;
    case GGCMD_POINTER_CHAIN:      h_pointer_chain(fd, seq, p); break;
    case GGCMD_SUBSCRIBE_EVENTS:
    case GGCMD_UNSUBSCRIBE_EVENTS:
    case GGCMD_RELAY_EVENT:
        send_err(fd, h->cmd, seq, GGERR_BAD_REQUEST,
                 "command removed: events subsystem was deleted in AI-first mode");
        break;
    case GGCMD_GHOST_TAP:          h_ghost_tap(fd, seq, p); break;
    case GGCMD_GHOST_SWIPE:        h_ghost_swipe(fd, seq, p); break;
    /* AUTH 已在 dispatch 的握手门里提前处理并 return；这里保留 case 只为
     * 消除 -Wswitch 告警并让命令表保持完整。 */
    case GGCMD_AUTH:               h_auth(fd, seq, p, authed); break;
    default:
        send_err(fd, h->cmd, seq, GGERR_BAD_REQUEST, "unknown command");
        break;
    }
    pthread_mutex_unlock(&g_eng.mtx);

    if (root) cJSON_Delete(root);
    return;

fast_done:
    if (root) cJSON_Delete(root);
}

/* ---------------- 连接处理 ---------------- */

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    int authed = 0;   /* 本连接是否已通过 GGCMD_AUTH；未认证只放行 PING/AUTH */
    struct timeval tv = { .tv_sec = 300, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 5;   /* 发送超时收紧: 事件广播 best-effort, 慢客户端最多拖 5s */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    /* Nagle 必须关：wire 是请求-响应小帧协议，一个响应只有几十字节时，若前一个
     * 小包还没被确认，内核会把后续小包压到对端 delayed ACK（40ms）。实测单点
     * gg_read 稳定 40.0ms/次，就是这 40ms（见 wire_io.c send_hdr_payload 注释）。
     * 与 send_hdr_payload 的"合并 header+payload"是一套组合拳，缺一个仍会退化。 */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    clients_register(fd);   /* 事件广播注册表（默认未订阅） */

    for (;;) {
        gg_wire_header_t h;
        if (read_full(fd, &h, sizeof(h)) < 0) break;
        /* 不再逐请求打印 conn 行（原实现每次命令一条，UI 每秒轮询 result_count
         * 就能把日志刷到一万多行，把启动期的 self-test 结论挤出尾部窗口，
         * 隐藏性自检因此误报"日志里没有 channel OK"；顺带省掉每请求一次
         * flash 写）。协议异常的诊断信息在下面各分支里保留。 */
        if (h.magic != GGWM_MAGIC || h.ver != GGWM_VERSION) {
            log_msg("conn: bad header magic=%08x ver=%u (expect %08x/%u), drop",
                    (unsigned)h.magic, (unsigned)h.ver,
                    (unsigned)GGWM_MAGIC, (unsigned)GGWM_VERSION);
            break;
        }
        if (h.payload_len > GGWM_MAX_PAYLOAD) {   /* 单帧负载上限 1MiB（见 wire_protocol.h） */
            log_msg("conn: payload_len %u exceeds max %u, drop",
                    (unsigned)h.payload_len, (unsigned)GGWM_MAX_PAYLOAD);
            break;
        }
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
        dispatch(fd, &h, payload, &authed);
        free(payload);
    }
    clients_unregister(fd);
    close(fd);
    atomic_fetch_sub(&g_active_clients, 1);
    return NULL;
}

/* ---------------- 主入口 ---------------- */

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "设备端内核守护（KO 驱动方案）\n"
        "用法: %s [--port 34127]\n"
        "           [--driver rwproc]             # 驱动后端（当前仅 rwproc33）\n"
        "           # KO 的加载与版本选择由 scripts/install_ko.sh 负责，agentd 不参与\n"
        "           [--rwproc-key <key>]          # rwproc33 内存模块 proc 节点 auth key\n"
        "           [--rwproc-bp-key <key>]       # rwproc33 断点模块 proc 节点 auth key\n"
        "           [--wire-key <hex>]            # wire 握手密钥（PC 端须配置同一值）\n"
        "           [--wire-key-file <path>]      # 从文件读握手密钥（推荐：0600 权限，仅 root 可读）\n"
        "           [--hwbp]                      # 启用硬件断点（当前 KO 的模块会崩内核，勿开）\n"
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
    static char dtype_buf[16];
    static char rwproc_key_buf[128], rwproc_bp_key_buf[128], logpath_buf[256];
    static char wire_key_buf[65], wire_key_file_buf[256];
    const char *dtype = dtype_buf;
    const char *rwproc_key = rwproc_key_buf, *rwproc_bp_key = rwproc_bp_key_buf;
    const char *logpath = logpath_buf;
    int port = 34127;            /* 非常见高位端口，避免 19100 等默认值被扫 */
    snprintf(dtype_buf, sizeof(dtype_buf), "%s", "hook");
    rwproc_key_buf[0] = rwproc_bp_key_buf[0] = logpath_buf[0] = 0;
    wire_key_buf[0] = wire_key_file_buf[0] = 0;

    /* 【2026-10-22 修复】引擎状态必须在**参数解析之前**清零。
     * 原顺序是「先解析参数、后 memset(&g_eng)」，而 --shell / --safe / --hwbp
     * 这三个开关是直接写 g_eng 字段的 —— 解析完立刻被 memset 抹掉：
     *   --shell → shell_enabled 恒 0，gg_shell 永远返回 "shell disabled"
     *   --safe  → safe_mode 恒 0，**只读保护形同虚设**（这个最严重）
     *   --hwbp  → hwbp_allowed 恒 0
     * 表现是「参数照收、行为不变」，而且 cmdline 被 hide_process() 清空后
     * 从外部也看不出参数到底有没有生效。 */
    memset(&g_eng, 0, sizeof(g_eng));
    g_eng.session_pid = -1;
    g_eng.result_cap = GG_RESULT_CAP_DEFAULT;
    pthread_mutex_init(&g_eng.mtx, NULL);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            /* atoi 对 "70000"/"abc" 分别给出 70000/0，随后被 (uint16_t) 静默截断成
             * 4464/0 —— 端口 0 表示随机端口，adb forward 必然连不上，而日志里看不出
             * 任何异常。这里显式校验范围，把配置错误变成启动失败。 */
            uint64_t pv = 0;
            if (!parse_u64_strict(argv[++i], &pv) || pv == 0 || pv > 65535) {
                fprintf(stderr, "gg-agentd: --port must be 1..65535 (got '%s')\n", argv[i]);
                return 2;
            }
            port = (int)pv;
        }
        else if (strcmp(argv[i], "--driver") == 0 && i + 1 < argc)
            snprintf(dtype_buf, sizeof(dtype_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--rwproc-key") == 0 && i + 1 < argc)
            snprintf(rwproc_key_buf, sizeof(rwproc_key_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--rwproc-bp-key") == 0 && i + 1 < argc)
            snprintf(rwproc_bp_key_buf, sizeof(rwproc_bp_key_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--wire-key") == 0 && i + 1 < argc)
            snprintf(wire_key_buf, sizeof(wire_key_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--wire-key-file") == 0 && i + 1 < argc)
            snprintf(wire_key_file_buf, sizeof(wire_key_file_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            snprintf(logpath_buf, sizeof(logpath_buf), "%s", argv[++i]);
        else if (strcmp(argv[i], "--safe") == 0) g_eng.safe_mode = 1;
        else if (strcmp(argv[i], "--shell") == 0) g_eng.shell_enabled = 1;
        else if (strcmp(argv[i], "--hwbp") == 0) g_hwbp_allowed = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* ---- 握手密钥装载：--wire-key 优先，其次 --wire-key-file，都没有则无鉴权 ----
     * 为什么必须有：agentd 以 root 运行且监听回环端口，设备上任何拿到 INTERNET
     * 权限的 App 都能连上来调 GGCMD_SHELL / WRITE / LOAD_GG。密钥文件应放在
     * 只有 root 可读的目录（如 /data/adb/...，权限 0600），普通 App 拿不到。
     * 必须在 hide_process() 之前完成：argv 稍后会被清零。 */
    if (wire_key_buf[0]) {
        snprintf(g_wire_key, sizeof(g_wire_key), "%s", wire_key_buf);
    } else if (wire_key_file_buf[0]) {
        if (!read_key_file(wire_key_file_buf, g_wire_key, sizeof(g_wire_key))) {
            /* 明确指定了文件却读不出内容，属配置错误：直接失败，不要悄悄
             * 退化成"临时 key"（那样客户端用的是另一份 key，报的是鉴权失败，
             * 真正的原因在启动参数里）。 */
            fprintf(stderr, "[FATAL] --wire-key-file 无法读取或内容为空: %s\n",
                    wire_key_file_buf);
            return 1;
        }
    } else {
        /* 没指定密钥：生成/复用临时 key，而不是放行所有本地连接。 */
        if (load_or_create_ephemeral_key(g_wire_key, sizeof(g_wire_key)) != 0) {
            fprintf(stderr,
                    "[FATAL] 未指定 --wire-key/--wire-key-file，且无法生成临时密钥"
                    "（/dev/urandom 不可读），拒绝启动。\n");
            return 1;
        }
        g_wire_key_ephemeral = 1;
        fprintf(stderr,
                "[gg-agentd][WARN] 未指定 --wire-key/--wire-key-file，"
                "已使用临时密钥（非本次新生成的会复用 %s）：\n"
                "    %s\n"
                "  客户端 AUTH 需携带该值。正式部署请显式配置（deploy.ps1 会自动生成）。\n",
                GG_EPHEMERAL_KEY_PATH, g_wire_key);
        log_msg("wire auth: EPHEMERAL key in use (no --wire-key given)");
    }
    fprintf(stderr, "[gg-agentd] wire auth: ENABLED (key %zu chars%s)\n",
            strlen(g_wire_key), g_wire_key_ephemeral ? ", ephemeral" : "");

    hide_process(argc, argv);   /* 清 cmdline 参数与环境（防 ACE 扫 /proc/PID/cmdline） */

    /* 注意：g_eng 的清零与锁初始化已前移到参数解析之前（见 main() 开头），
     * 否则 --shell / --safe / --hwbp 会被这里的 memset 抹掉。 */
    if (logpath) g_eng.logf = fopen(logpath, "a");
    log_msg("starting (port=%d driver=%s safe=%d)",
            port, dtype, g_eng.safe_mode);

    driver_init_rwproc(&g_eng.drv, rwproc_key, rwproc_bp_key);
    freeze_init(&g_eng.freeze, &g_eng.drv);
    results_init(&g_eng.results, g_eng.result_cap);

    /* 对接 rwproc33：模块由 install_ko.sh 预先 insmod，这里只做节点发现 + 打开。
     * 不在 agentd 里 insmod —— 该模块 hide_by_default，重复 insmod 会 EEXIST。 */
    if (rwproc_attach(&g_eng.drv) != 0) {
        log_msg("rwproc_attach failed: %s", g_eng.drv.last_error);
    } else {
        log_msg("rwproc attached: %s", g_eng.drv.ko_file);
    }

    int st = driver_self_test(&g_eng.drv);
    log_msg("driver self-test: %s (%s)",
            st > 0 ? "channel OK" : "UNAVAILABLE",
            driver_state_str(g_eng.drv.state));
    if (st < 0) {
        log_msg("WARNING: rwproc channel unavailable; attach/search will fail");
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
        /* 连接上限：先原子占位再判，避免「判完未占位」的竞态导致瞬时超限 */
        int now = atomic_fetch_add(&g_active_clients, 1) + 1;
        if (now > GG_MAX_CLIENTS) {
            atomic_fetch_sub(&g_active_clients, 1);
            log_msg("accept: too many clients (max %d), rejected", GG_MAX_CLIENTS);
            close(cfd);
            continue;
        }
        pthread_t th;
        if (pthread_create(&th, NULL, conn_thread, (void *)(intptr_t)cfd) != 0) {
            atomic_fetch_sub(&g_active_clients, 1);
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
