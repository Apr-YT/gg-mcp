/* hook.c — libKernelGg.so：GG 核心内核化 hook 库（KO 驱动方案）
 *
 * 外挂圈子加载内核方式（KO 版）：
 *   在 GG 加载 lib5.so（内存引擎）前，通过 LD_PRELOAD 抢先加载本库；
 *   拦截 syscall(2) 中 __NR_process_vm_readv(270)/writev(271)，
 *   解析 iovec 后重定向到 KO 驱动：
 *     - GG_KO_PROTO=qx（Aloha 开源 QX9-10）: /dev 节点 + ioctl 0x801读/0x802写,
 *       COPY_MEMORY{pid,addr,buffer,size}（可选 GG_KO_KEY 做 init_key 0x800）
 *     - GG_KO_DEV 已设置（通用 dev 型 KO）: open /dev 节点 + ioctl 0x7e1a读/0x7e1b写
 *     - 未设置（hook 型 KO）: 直通真实 syscall，由内核 hook 接管（零协议）
 *   驱动不可用时透明回退到真实 syscall（降级链）。
 *
 * 非目标调用通过内联汇编 svc 直通内核，避免递归。
 *
 * 编译: aarch64-linux-androidXX-clang -shared -fPIC -O2 -o libKernelGg.so hook.c
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include "ko_downgrade.h"   /* KO 驱动降级状态机 (C5 修复): 区分单次失败与驱动不可用 */

/* 兜底: 某些头文件不导出这两个宏 */
#ifndef SYS_process_vm_readv
#define SYS_process_vm_readv 270
#endif
#ifndef SYS_process_vm_writev
#define SYS_process_vm_writev 271
#endif

/* ---- 通用 dev 型 KO ioctl 协议（与 agentd/driver.c 的 ko_ioctl_req 一致） ---- */
#define GG_KO_IOCTL_READ_DEFAULT  0x7e1a
#define GG_KO_IOCTL_WRITE_DEFAULT 0x7e1b

typedef struct __attribute__((packed)) ko_ioctl_req {
    uint32_t pid;
    uint32_t pad;
    uint64_t addr;
    uint64_t buf;
    uint64_t size;
} ko_ioctl_req_t;

/* ---- QX 型协议（与 qx-对接.h 一致） ---- */
#define QX_OP_INIT_KEY   0x800
#define QX_OP_READ_MEM   0x801
#define QX_OP_WRITE_MEM  0x802
#define QX_KEY_MAX       0x100

typedef struct qx_copy_memory {
    pid_t     pid;      /* offset 0  */
    uintptr_t addr;     /* offset 8  */
    void     *buffer;   /* offset 16 */
    size_t    size;     /* offset 24 */
} qx_copy_memory_t;

/* ---- 驱动通道状态 ---- */
static pthread_mutex_t s_io_mtx = PTHREAD_MUTEX_INITIALIZER;
static int  s_dev_fd = -1;
static int  s_proto = 0;                /* 0=直通(hook型) 1=dev 2=qx */
static char g_dev_path[128];
static int  s_ioctl_read  = GG_KO_IOCTL_READ_DEFAULT;
static int  s_ioctl_write = GG_KO_IOCTL_WRITE_DEFAULT;
static ko_downgrade_state_t s_ko_state = {0, 0};  /* KO 驱动降级状态机 (见 ko_downgrade.h) */
static int  s_env_parsed = 0;

static void parse_env_once(void) {
    if (s_env_parsed) return;
    s_env_parsed = 1;
    const char *proto = getenv("GG_KO_PROTO");
    if (proto && strcmp(proto, "qx") == 0) {
        s_proto = 2;                     /* QX 协议 */
    }
    const char *dev = getenv("GG_KO_DEV");
    if (dev && *dev) {
        if (s_proto == 0) s_proto = 1;   /* 显式指定节点 → dev 协议 */
        snprintf(g_dev_path, sizeof(g_dev_path), "%s", dev);
    }
    const char *r = getenv("GG_KO_IOCTL_READ");
    if (r && *r) s_ioctl_read = (int)strtol(r, NULL, 0);
    const char *w = getenv("GG_KO_IOCTL_WRITE");
    if (w && *w) s_ioctl_write = (int)strtol(w, NULL, 0);
}

static int ensure_channel(void) {
    if (s_proto == 0) return -1;         /* hook 型 KO: 内核接管, 无需通道 */
    if (s_ko_state.downgraded) return -1;/* 已判驱动不可用 → 短路 */
    if (s_dev_fd >= 0) return s_dev_fd;
    int fd = open(g_dev_path, O_RDWR);
    if (fd < 0) return -1;
    /* qx: 先做 init_key 认证 */
    if (s_proto == 2) {
        const char *key = getenv("GG_KO_KEY");
        if (key && *key) {
            char kb[QX_KEY_MAX];
            memset(kb, 0, sizeof(kb));
            strncpy(kb, key, sizeof(kb) - 1);
            if (ioctl(fd, QX_OP_INIT_KEY, kb) != 0) {
                close(fd);
                ko_downgrade_hard(&s_ko_state);  /* init_key 失败 = 结构性错误 → 永久降级 */
                return -1;
            }
        }
    }
    s_dev_fd = fd;
    return fd;
}

/* 返回 >0 成功 / -1 驱动不可用或单次请求失败（调用方本次回退真实 syscall）。
 * 单次请求失败（如该次地址不可读/权限不足）不立即永久降级驱动：
 *   仅当连续失败 GG_KO_FAIL_THRESHOLD 次才判驱动整体不可用（见 ko_downgrade.h），
 *   避免偶发错误（大进程扫描中极常见）误杀正常 KO 驱动。 */
static long driver_io(int is_write, pid_t pid, uint64_t addr, void *buf, size_t len) {
    parse_env_once();
    int fd = ensure_channel();
    if (fd < 0) return -1;               /* 通道结构性不可用 → 本次回退真实 syscall */

    if (s_proto == 2) {
        qx_copy_memory_t cm;
        cm.pid = pid;
        cm.addr = (uintptr_t)addr;
        cm.buffer = buf;
        cm.size = len;
        errno = 0;
        long r = ioctl(fd, is_write ? QX_OP_WRITE_MEM : QX_OP_READ_MEM, &cm);
        if (r != 0) { ko_downgrade_record(&s_ko_state, 0); return -1; }  /* errno 由 ioctl 保留 */
        ko_downgrade_record(&s_ko_state, 1);
        return (long)len;
    }

    ko_ioctl_req_t req;
    memset(&req, 0, sizeof(req));
    req.pid = (uint32_t)pid;
    req.addr = addr;
    req.buf = (uint64_t)(uintptr_t)buf;
    req.size = (uint64_t)len;
    errno = 0;
    long r = ioctl(fd, is_write ? s_ioctl_write : s_ioctl_read, &req);
    if (r < 0) { ko_downgrade_record(&s_ko_state, 0); return -1; }
    ko_downgrade_record(&s_ko_state, 1);
    return r;
}

/* ---- 真实 syscall（aarch64 内联汇编直通, 避免递归） ---- */
#if defined(__aarch64__)
static long raw_syscall6(long number, long a1, long a2, long a3,
                         long a4, long a5, long a6) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    register long x3 __asm__("x3") = a4;
    register long x4 __asm__("x4") = a5;
    register long x5 __asm__("x5") = a6;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory", "cc");
    return x0;
}
#else
#error "hook.c supports aarch64 only"
#endif

/* ---- 拦截 process_vm_readv/writev ---- */
static long hook_process_vm(long number, long pid,
                            struct iovec *local, long liovcnt,
                            struct iovec *remote, long riovcnt) {
    int is_write = (number == SYS_process_vm_writev);
    long total = 0;
    long n = liovcnt < riovcnt ? liovcnt : riovcnt;
    for (long i = 0; i < n; i++) {
        size_t len = local[i].iov_len;
        if (remote[i].iov_len < len) len = remote[i].iov_len;
        if (len == 0) continue;
        long got = driver_io(is_write, (pid_t)pid,
                             (uint64_t)(uintptr_t)remote[i].iov_base,
                             local[i].iov_base, len);
        if (got < 0) return -1;               /* 驱动失败 → 调用方回退 */
        total += got;
        if ((size_t)got < len) break;
    }
    return total;
}

/* ---- 对外符号: 覆盖 libc 的 syscall() ---- */
long syscall(long number, ...) {
    va_list ap;
    va_start(ap, number);
    long a1 = va_arg(ap, long);
    long a2 = va_arg(ap, long);
    long a3 = va_arg(ap, long);
    long a4 = va_arg(ap, long);
    long a5 = va_arg(ap, long);
    long a6 = va_arg(ap, long);
    va_end(ap);

    if (number == SYS_process_vm_readv || number == SYS_process_vm_writev) {
        pthread_mutex_lock(&s_io_mtx);
        long r = hook_process_vm(number, a1, (struct iovec *)a2, a3,
                                 (struct iovec *)a4, a5);
        pthread_mutex_unlock(&s_io_mtx);
        if (r >= 0) return r;
        /* 驱动不可用 → 落到真实 syscall（降级链） */
    }

    long r = raw_syscall6(number, a1, a2, a3, a4, a5, a6);
    if (r < 0) {
        errno = (int)(-r);
        return -1;
    }
    return r;
}
