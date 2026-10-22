/* driver.c — 内核内存驱动抽象层实现（KO 驱动方案）
 *
 * 只加载 KO 内核驱动（外挂圈子方式：insmod，不依赖 KPM/KPatch-Next）。
 *
 * 后端：
 *   1) hook 型 KO: 内核 hook process_vm_readv/writev → 用户态直接走
 *      process_vm_readv（内核已接管）→ /proc/<pid>/mem 兜底。
 *   2) dev 型 KO: open /dev 节点 + ioctl(fd, 0x7e1a读/0x7e1b写,
 *      {u32 pid; u32 pad; u64 addr; u64 buf; u64 size})（packed）。
 *   3) qx 型（Aloha 开源 QX9-10）: 隐藏进程持有已删除的设备节点，
 *      经 /proc/<pid>/fd/3 的 st_rdev 重建节点 → open →
 *      ioctl 0x800 init_key / 0x801 读 / 0x802 写 / 0x803 模块基址，
 *      COPY_MEMORY{pid,addr,buffer,size} 自然对齐（非 packed，与 qx-对接.h 一致）。
 *
 * KO 加载：insmod + setenforce 0（若需）；dev 型自动按 /proc/misc 建节点。
 * 版本族容错：文件名版本与模式（如 6.12.*）族匹配，同族补丁任意可加载。
 */
#define _GNU_SOURCE

#include "driver.h"

/* 日志（main.c 定义，非 static；写 --log 文件，失败时静默） */
void log_msg(const char *fmt, ...);
#include "version.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#endif

/* ---- dev 型 ioctl 请求结构（packed, 与 KO 驱动约定一致） ---- */
typedef struct __attribute__((packed)) ko_ioctl_req {
    uint32_t pid;
    uint32_t pad;
    uint64_t addr;
    uint64_t buf;
    uint64_t size;
} ko_ioctl_req_t;

/* ---- qx 型请求结构（自然对齐, 与 qx-对接.h 的 COPY_MEMORY/MODULE_BASE 一致） ---- */
typedef struct qx_copy_memory {
    pid_t     pid;      /* offset 0  */
    uintptr_t addr;     /* offset 8  */
    void     *buffer;   /* offset 16 */
    size_t    size;     /* offset 24 */
} qx_copy_memory_t;     /* sizeof 32 */

typedef struct qx_module_base {
    pid_t     pid;      /* offset 0  */
    char     *name;     /* offset 8  */
    uintptr_t base;     /* offset 16 */
} qx_module_base_t;     /* sizeof 24 */

static void set_err(gg_driver_t *d, const char *msg) {
    snprintf(d->last_error, sizeof(d->last_error), "%s", msg ? msg : "");
}


/* ---------- rwproc33 型后端（abcz316/rwProcMem33, proc read 33B 头协议） ----------
 * 协议（见 docs/engineering/RWPROC33_ADAPTER.md + 上游源码）:
 *   read(fd, buf, 33 + data_size):
 *     头(33B packed {cmd,p1,p2,p3,buf_size}) 在 buf[0..32], 数据区在 buf[33..]
 *     read() 返回值 = 操作结果长度；负值 = -errno
 *     读内存: 结果数据写回 buf+33；OPEN_PROCESS/INST_HWBP: 句柄写回 buf+33
 *     读写默认 is_force=1（临时改 PTE 强制读写, 不检查映射权限）
 *  引擎所有操作在 g_eng.mtx 下串行, 静态缓冲安全。 */

/* init_device_info（与上游 packed 结构一致, sizeof 1053） */
typedef struct __attribute__((packed)) rwproc_init_info {
    int   pid;
    int   tgid;
    char  my_name[17];
    char  my_auxv[1024];
    int   my_auxv_size;
} rwproc_init_info_t;

#define RWPROC_HDR_SZ   33
#define RWPROC_MAX_IO   (256u << 10)   /* 单次读写上限 256KB（= GG_SEARCH_CHUNK） */

/* 协议尺寸编译期校验（与上游 packed 结构一致, 错位会毁掉整条通道） */
_Static_assert(sizeof(rwproc_request_t) == 33, "rwproc request header must be 33 bytes");
_Static_assert(sizeof(rwproc_hwbp_hit_item_t) == 312, "rwproc HWBP_HIT_ITEM must be 312 bytes");
_Static_assert(sizeof(rwproc_init_info_t) == 1053, "rwproc init_device_info must be 1053 bytes");
_Static_assert(sizeof(rwproc_hwbp_regs_t) == 288, "rwproc my_user_pt_regs must be 288 bytes");

static uint8_t rwproc_buf[RWPROC_HDR_SZ + RWPROC_MAX_IO];

/* ⚠️ rwproc_buf 是**进程级共享**缓冲，不是每次调用一份。三处并发调用它：
 *   1) 搜索 worker 线程池（search.c 的 search_worker_main，N 线程并行 driver_read）；
 *   2) 冻结线程（freeze.c 的 freeze_loop，独立线程调 driver_write，只持 f->mtx，
 *      不在 g_eng.mtx 覆盖范围内）；
 *   3) 主线程的 handler（在 g_eng.mtx 内）。
 * 只有 (3) 之间是串行的。请求写入 → read() → 结果拷出必须在同一临界区，
 * 否则 A 线程刚 read 完还没拷走，B 线程的下一次 memset/read 就把结果覆盖掉：
 * 表现为读到别的地址的数据、句柄错位、搜索命中值对不上。
 * 删掉这把锁不是性能优化，是数据破坏。 */
static pthread_mutex_t rwproc_mtx = PTHREAD_MUTEX_INITIALIZER;

/* 通用调用: 往节点 fd 发一个请求。data 为 buf+33 处数据（可 NULL）。
 * 成功返回 read() 结果（>=0）；失败返回 -1 并置 last_error。
 *
 * resp_cap: 期望内核对 buf+33 回写的最大字节数，只影响 read() 的 count。
 *   为什么需要它：read() 的 count 决定内核最多能往用户缓冲区写多少字节，
 *   而请求负载长度（buf_size）与回写长度并不总是相等。OPEN_PROCESS 只发
 *   33 字节头（buf_size=0）却要收回 8 字节 handle —— 若 count 也只给 33，
 *   内核写不进去，随后的 memcpy(buf+33, 8) 取到的是上一次请求的残留数据。
 *   症状：读自身"成功"（残留碰巧有效），读别的进程返回 0 或 EFAULT。
 *
 * out/out_cap: 结果拷出目标，**在锁内完成**。read() 返回 r>0 时拷 r 字节；
 *   r==0（OPEN_PROCESS / INST_HWBP / HIT_COUNT 这类"成功但不报长度"的命令）
 *   拷 out_cap 字节。r 超过 out_cap 视为协议错位，直接判失败而不是截断 ——
 *   截断会把一次驱动异常伪装成"部分成功"。 */
static ssize_t rwproc_call_ex(gg_driver_t *d, int fd, char cmd,
                              uint64_t p1, uint64_t p2, uint64_t p3,
                              const void *data, size_t data_size, size_t resp_cap,
                              void *out, size_t out_cap) {
    if (fd < 0) { set_err(d, "rwproc node not open"); return -1; }
    if (data_size > RWPROC_MAX_IO) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwproc request too large: %zu", data_size);
        return -1;
    }
    if (resp_cap > RWPROC_MAX_IO) resp_cap = RWPROC_MAX_IO;
    size_t rd = RWPROC_HDR_SZ + (resp_cap > data_size ? resp_cap : data_size);
    ssize_t r;
    pthread_mutex_lock(&rwproc_mtx);
    rwproc_request_t *req = (rwproc_request_t *)rwproc_buf;
    /* 清到 rd：残留数据会被误当成内核回写的结果 */
    memset(rwproc_buf, 0, rd);
    req->cmd = cmd;
    req->param1 = p1;
    req->param2 = p2;
    req->param3 = p3;
    req->buf_size = data_size;
    if (data && data_size) memcpy(rwproc_buf + RWPROC_HDR_SZ, data, data_size);
    errno = 0;
    r = read(fd, rwproc_buf, rd);
    if (r < 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwproc read(cmd=%d) failed: %s", (int)cmd, strerror(errno));
        pthread_mutex_unlock(&rwproc_mtx);
        return -1;
    }
    if (out) {
        size_t n = (r > 0) ? (size_t)r : out_cap;
        if (n > out_cap) {
            snprintf(d->last_error, sizeof(d->last_error),
                     "rwproc result overflow: got %zu, cap %zu (cmd=%d)",
                     n, out_cap, (int)cmd);
            pthread_mutex_unlock(&rwproc_mtx);
            return -1;
        }
        if (n) memcpy(out, rwproc_buf + RWPROC_HDR_SZ, n);
    }
    pthread_mutex_unlock(&rwproc_mtx);
    return r;
}

static ssize_t rwproc_call(gg_driver_t *d, int fd, char cmd,
                           uint64_t p1, uint64_t p2, uint64_t p3,
                           const void *data, size_t data_size) {
    return rwproc_call_ex(d, fd, cmd, p1, p2, p3, data, data_size, 0, NULL, 0);
}

/* 节点归属：两个模块都用 /proc/<key>/<key> 的形状，光看路径分不出是谁。
 * 区分的唯一手段是发一条只有一方认识、且对另一方无害的命令。 */
#define RWPROC_KIND_MEM  1
#define RWPROC_KIND_BP   2

/* 探测一个已打开的节点属于哪个模块。
 *
 * 判据：GET_NUM_BRPS(cmd=3)。
 *   断点模块：cmd3 = 取执行断点槽数，read 返回正数（ARM64 实测 6）。
 *   内存模块：cmd3 = READ_PROCESS_MEMORY，p1=0 是空句柄 → 返回 -1/-14。
 * 只用这一条：内存模块的 cmd4 是 WRITE_PROCESS_MEMORY、cmd5 是 CLOSE_PROCESS，
 * 拿它们试探会在真正的硬件断点上写用户内存 / put_pid。
 *
 * 为什么必须做这个探测：bp 节点一旦错指到内存模块，hwbp 探测会拿到 -1，
 * 而 -1 和「0 起始枚举的旧模块」表现一模一样，排查方向直接被带偏
 * （AGENTS 第 25 条就是这么误判的）。
 *
 * 返回 RWPROC_KIND_*；read 异常（含 -EFAULT）一律按内存模块处理 —— 断点模块
 * 在这条命令上不会失败，把异常归给内存模块只会让 bp 侧继续找下一个候选。 */
static int rwproc_node_kind(int fd) {
    unsigned char buf[RWPROC_HDR_SZ];
    memset(buf, 0, sizeof(buf));
    ((rwproc_request_t *)buf)->cmd = (char)RWBP_CMD_GET_NUM_BRPS;
    ssize_t r = read(fd, buf, sizeof(buf));
    return (r > 0) ? RWPROC_KIND_BP : RWPROC_KIND_MEM;
}

/* 打开指定类型的模块节点。
 * key 非空：只试 /proc/<key>/<key>，并校验类型（配错 key 会明确报出来，
 *          而不是把内存节点的 fd 当成断点节点用）。
 * key 为空：扫描 /proc 找"目录名==内文件名"的候选，逐个探测类型
 *          （模块未隐藏时可发现；已隐藏则必须配置 key）。
 * 返回 fd；-1 失败（last_error 说明原因）。 */
static int rwproc_open_node(gg_driver_t *d, const char *key, int want_kind) {
    const char *want = (want_kind == RWPROC_KIND_BP) ? "breakpoint" : "memory";
    if (key && *key) {
        char path[256];
        snprintf(path, sizeof(path), "/proc/%s/%s", key, key);
        int fd = open(path, O_RDWR);
        if (fd < 0) {
            snprintf(d->last_error, sizeof(d->last_error),
                     "open %s failed: %s", path, strerror(errno));
            return -1;
        }
        int kind = rwproc_node_kind(fd);
        if (kind != want_kind) {
            snprintf(d->last_error, sizeof(d->last_error),
                     "%s is the %s module, not %s (GET_NUM_BRPS probe) — --rwproc-key / "
                     "--rwproc-bp-key 配反了", path,
                     kind == RWPROC_KIND_BP ? "breakpoint" : "memory", want);
            close(fd);
            return -1;
        }
        return fd;
    }
    DIR *dp = opendir("/proc");
    if (!dp) { set_err(d, "cannot scan /proc"); return -1; }
    struct dirent *de;
    int fd = -1;
    while ((de = readdir(dp)) != NULL && fd < 0) {
        if (de->d_name[0] >= '0' && de->d_name[0] <= '9') continue;   /* 跳过 pid 目录 */
        if (strcmp(de->d_name, "self") == 0 || strcmp(de->d_name, "thread-self") == 0) continue;
        char sub[512];
        snprintf(sub, sizeof(sub), "/proc/%s/%s", de->d_name, de->d_name);
        if (access(sub, F_OK) != 0) continue;
        int cand = open(sub, O_RDWR);
        if (cand < 0) continue;
        if (rwproc_node_kind(cand) == want_kind) {
            fd = cand;
            snprintf(d->last_error, sizeof(d->last_error),
                     "auto-discovered %s node: %s", want, sub);
        } else {
            close(cand);
        }
    }
    closedir(dp);
    if (fd < 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "%s module node not found (module not loaded? set %s)",
                 want, want_kind == RWPROC_KIND_BP ? "--rwproc-bp-key" : "--rwproc-key");
    }
    return fd;
}

static int rwproc_init_device_info(gg_driver_t *d, int fd) {
    rwproc_init_info_t info;
    memset(&info, 0, sizeof(info));
    info.pid = (int)getpid();
    info.tgid = (int)getpid();
    char comm[17] = "";
    FILE *cf = fopen("/proc/self/comm", "r");
    if (cf) {
        if (fgets(comm, sizeof(comm), cf)) {
            size_t l = strlen(comm);
            while (l && (comm[l - 1] == '\n' || comm[l - 1] == ' ')) comm[--l] = 0;
        }
        fclose(cf);
    }
    snprintf(info.my_name, sizeof(info.my_name), "%s", comm);
    /* auxv: /proc/self/auxv 是 {u64 type, u64 val} 对序列 */
    int afd = open("/proc/self/auxv", O_RDONLY);
    if (afd >= 0) {
        ssize_t n = read(afd, info.my_auxv, sizeof(info.my_auxv));
        if (n > 0) info.my_auxv_size = (int)n;
        close(afd);
    }
    ssize_t r = rwproc_call(d, fd, RWPROC_CMD_INIT_DEVICE_INFO, 0, 0, 0,
                            &info, sizeof(info));
    if (r < 0) return -1;
    if (r != 0) {   /* 内核返回 0 成功, 负值 -errno */
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwproc CMD_INIT_DEVICE_INFO failed: %lld", (long long)r);
        return -1;
    }
    return 0;
}

int driver_init_rwproc(gg_driver_t *d, const char *mem_key, const char *bp_key) {
    memset(d, 0, sizeof(*d));
    d->state = ST_UNAVAILABLE;
    d->rwproc_mem_fd = -1;
    d->rwproc_bp_fd = -1;
    d->rwproc_handle = 0;
    d->rwproc_pid = -1;
    if (mem_key && *mem_key) snprintf(d->rwproc_mem_key, sizeof(d->rwproc_mem_key), "%s", mem_key);
    if (bp_key && *bp_key) snprintf(d->rwproc_bp_key, sizeof(d->rwproc_bp_key), "%s", bp_key);
    return 0;
}

int rwproc_attach(gg_driver_t *d) {
    /* 1) 打开内存模块节点 */
    d->rwproc_mem_fd = rwproc_open_node(d, d->rwproc_mem_key, RWPROC_KIND_MEM);
    if (d->rwproc_mem_fd < 0) return -GGERR_NOT_FOUND;
    /* 2) 打开断点模块节点（可选; 失败不影响内存读写） */
    char mem_err[256];
    snprintf(mem_err, sizeof(mem_err), "%s", d->last_error);
    d->rwproc_bp_fd = rwproc_open_node(d, d->rwproc_bp_key, RWPROC_KIND_BP);
    if (d->rwproc_bp_fd < 0) {
        /* bp 节点缺失（单模块 apr_dlkm 无 hwbp）= watch 不可用，非错误：
           只进日志，恢复 last_error 为 mem 节点状态，避免 kernel_status 误报。 */
        log_msg("hwbp node not found (watch 不可用, 内存读写正常): %s", d->last_error);
        snprintf(d->last_error, sizeof(d->last_error), "%s", mem_err);
    }
    /* 3) CMD_INIT_DEVICE_INFO（必须首个命令） */
    if (rwproc_init_device_info(d, d->rwproc_mem_fd) != 0) {
        close(d->rwproc_mem_fd);
        d->rwproc_mem_fd = -1;
        return -GGERR_INTERNAL;
    }
    /* 4) 触发内核模块隐藏（CMD_HIDE_KERNEL_MODULE=13）:
     *    模块内 list_del(modules 链表) + kobject_del(/sys/module)，
     *    /proc/modules 与 /sys/module 立即消失。失败不致命（节点已开，
     *    fd 引用仍在，功能不受影响）。 */
    {
        ssize_t hr = rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_HIDE_KERNEL_MODULE,
                                 0, 0, 0, NULL, 0);
        if (hr < 0)
            log_msg("rwproc CMD_HIDE_KERNEL_MODULE failed: %s", d->last_error);
    }
    snprintf(d->ko_module, sizeof(d->ko_module), "rwproc33");
    snprintf(d->ko_version, sizeof(d->ko_version), "rwProcMem33");
    snprintf(d->ko_file, sizeof(d->ko_file),
             "proc nodes: mem=%s bp=%s", d->rwproc_mem_key, d->rwproc_bp_key);
    d->ko_loaded = 1;
    return 0;
}

int rwproc_open_process(gg_driver_t *d, pid_t pid, uint64_t *handle) {
    if (d->rwproc_mem_fd < 0) return -1;
    /* 换 pid: 关旧开新 */
    if (d->rwproc_handle && d->rwproc_pid != pid) {
        rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_CLOSE_PROCESS,
                    d->rwproc_handle, 0, 0, NULL, 0);
        d->rwproc_handle = 0;
    }
    if (!d->rwproc_handle) {
        uint64_t h = 0;
        ssize_t r = rwproc_call_ex(d, d->rwproc_mem_fd, RWPROC_CMD_OPEN_PROCESS,
                                   (uint64_t)pid, 0, 0, NULL, 0, sizeof(uint64_t),
                                   &h, sizeof(h));
        if (r < 0) return -1;
        if (r != 0) {
            snprintf(d->last_error, sizeof(d->last_error),
                     "rwproc CMD_OPEN_PROCESS(pid=%d) failed: %lld", (int)pid, (long long)r);
            return -1;
        }
        if (!h) {
            set_err(d, "rwproc CMD_OPEN_PROCESS returned null handle");
            return -1;
        }
        d->rwproc_handle = h;
        d->rwproc_pid = pid;
    }
    if (handle) *handle = d->rwproc_handle;
    return 0;
}

int rwproc_close_process(gg_driver_t *d, uint64_t handle) {
    if (d->rwproc_mem_fd < 0 || !handle) return -1;
    rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_CLOSE_PROCESS, handle, 0, 0, NULL, 0);
    if (d->rwproc_handle == handle) { d->rwproc_handle = 0; d->rwproc_pid = -1; }
    return 0;
}

/* rwproc 读（调用方已持有引擎锁）。len 上限 RWPROC_MAX_IO。
 * force=0: 只读映射权限允许的页（不翻转 PTE, 避免与目标进程活跃页竞争导致崩溃）。
 * force=1: 让驱动临时改 PTE 强制读（未驻留页/受限页）。
 * 读失败返回部分/错误, 调用方按实际字节数处理。 */
static ssize_t rwproc_read_mem(gg_driver_t *d, pid_t pid, uint64_t addr,
                               void *buf, size_t len, int force) {
    if (len > RWPROC_MAX_IO) len = RWPROC_MAX_IO;
    if (rwproc_open_process(d, pid, NULL) != 0) return -1;
    /* 结果拷出与长度校验都在 rwproc_call_ex 的临界区内完成：
     * 在锁外按返回值 memcpy 会被别的线程的下一次请求覆盖，r>len 时还会写穿调用方缓冲。 */
    return rwproc_call_ex(d, d->rwproc_mem_fd, RWPROC_CMD_READ_PROCESS_MEMORY,
                          d->rwproc_handle, addr, force ? 1 : 0,
                          NULL, len, len, buf, len);
}

static ssize_t rwproc_write_mem(gg_driver_t *d, pid_t pid, uint64_t addr,
                                const void *buf, size_t len) {
    if (len > RWPROC_MAX_IO) len = RWPROC_MAX_IO;
    if (rwproc_open_process(d, pid, NULL) != 0) return -1;
    return rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_WRITE_PROCESS_MEMORY,
                       d->rwproc_handle, addr, 0 /* is_force */, buf, len);
}

/* ---------- GG-TOUCH 内核虚拟触摸（rwProcMem_module CMD_GGTOUCH_*) ---------- */
/* 仅 rwproc33 型后端支持。display 坐标由驱动内 disp→raw 换算并注入 input 子系统。 */

int driver_touch_tap(gg_driver_t *d, uint32_t x, uint32_t y, uint32_t hold_ms) {
    if (d->rwproc_mem_fd < 0) {
        set_err(d, "aq-dsp: need rwproc33 driver");
        return -1;
    }
    rwproc_gg_touch_tap_t arg = { x, y, hold_ms };
    ssize_t r = rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_GGTOUCH_TAP,
                            0, 0, 0, &arg, sizeof(arg));
    if (r < 0) return -1;
    if (r != 0) {   /* 驱动返回 0 成功, 负值 -errno */
        snprintf(d->last_error, sizeof(d->last_error),
                 "aq-dsp CMD_TAP(%u,%u) failed: %lld", x, y, (long long)r);
        return -1;
    }
    return 0;
}

int driver_touch_swipe(gg_driver_t *d, uint32_t x1, uint32_t y1,
                       uint32_t x2, uint32_t y2, uint32_t ms) {
    if (d->rwproc_mem_fd < 0) {
        set_err(d, "aq-dsp: need rwproc33 driver");
        return -1;
    }
    rwproc_gg_touch_swipe_t arg = { x1, y1, x2, y2, ms };
    ssize_t r = rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_GGTOUCH_SWIPE,
                            0, 0, 0, &arg, sizeof(arg));
    if (r < 0) return -1;
    if (r != 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "aq-dsp CMD_SWIPE failed: %lld", (long long)r);
        return -1;
    }
    return 0;
}

/* ---------- rwproc33 硬件断点（hwBreakpointProc_module） ---------- */

int rwproc_hwbp_start(gg_driver_t *d, pid_t pid, uint64_t addr, int type, int len,
                      uint64_t *bp_id) {
    if (d->rwproc_bp_fd < 0) return -1;
    if (type <= 0) type = HW_BP_TYPE_W;
    if (len <= 0) len = 4;
    uint64_t handle = 0;
    /* 断点模块的 OPEN_PROCESS 是独立句柄（与内存模块句柄不同） */
    ssize_t r = rwproc_call_ex(d, d->rwproc_bp_fd, RWBP_CMD_OPEN_PROCESS,
                               (uint64_t)pid, 0, 0, NULL, 0, sizeof(uint64_t),
                               &handle, sizeof(handle));
    if (r < 0) return -1;
    if (r != 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwbp CMD_OPEN_PROCESS(pid=%d) failed: %lld", (int)pid, (long long)r);
        return -1;
    }
    if (!handle) { set_err(d, "rwbp CMD_OPEN_PROCESS null handle"); return -1; }
    /* INST: param3 = len | (type << 8) */
    uint64_t id = 0;
    ssize_t r2 = rwproc_call_ex(d, d->rwproc_bp_fd, RWBP_CMD_INST_PROCESS_HWBP,
                                handle, addr, RWBP_PARAM3(len, type), NULL, 0,
                                sizeof(uint64_t), &id, sizeof(id));
    if (r2 < 0) {
        rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_CLOSE_PROCESS, handle, 0, 0, NULL, 0);
        return -1;
    }
    if (r2 != 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwbp CMD_INST_PROCESS_HWBP failed: %lld", (long long)r2);
        rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_CLOSE_PROCESS, handle, 0, 0, NULL, 0);
        return -1;
    }
    /* id 已由 rwproc_call_ex 在临界区内拷出（不能在这里再 memcpy 共享缓冲） */
    if (!id) {
        rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_CLOSE_PROCESS, handle, 0, 0, NULL, 0);
        set_err(d, "rwbp INST returned null bp id");
        return -1;
    }
    /* 注册完成后立刻释放进程句柄：perf_event 已绑定到 task，模块后续按
     * bp_id 操作，不再需要 struct pid 引用。不释放则每个断点泄漏一个
     * 内核 pid 引用，反复 watch 会累积。 */
    rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_CLOSE_PROCESS, handle, 0, 0, NULL, 0);
    if (bp_id) *bp_id = id;
    return 0;
}

/* 探测断点模块的命令码基准。
 * 我们的构建：CMD_GET_NUM_BRPS = 3，返回本机执行断点槽数（实测 ARM64 = 6）。
 * afei 的 prebuilt 用 0 起始枚举，同一条 cmd=3 实际是 GET_NUM_WRPS，返回 4。
 * 只发这一条只读命令 —— 0 起始下 cmd=4 才是 INST，绝不能拿去探测。 */
int rwproc_hwbp_probe(gg_driver_t *d) {
    if (d->rwproc_bp_fd < 0) return -1;
    ssize_t r = rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_GET_NUM_BRPS,
                            0, 0, 0, NULL, 0);
    if (r < 0) return -1;
    return (int)r;
}

int rwproc_hwbp_get_hits(gg_driver_t *d, uint64_t bp_id,
                         rwproc_hwbp_hit_item_t *out, int cap, int *real) {
    if (d->rwproc_bp_fd < 0) return -1;
    if (real) *real = 0;
    /* 命中数（模块返回 {hit_total_count, hit_item_arr_count} 到数据区） */
    uint64_t counts[2] = {0, 0};
    ssize_t r = rwproc_call_ex(d, d->rwproc_bp_fd, RWBP_CMD_GET_HWBP_HIT_COUNT,
                               bp_id, 0, 0, NULL, 0, sizeof(uint64_t) * 2,
                               counts, sizeof(counts));
    if (r < 0) return -1;
    uint64_t avail = counts[1] ? counts[1] : counts[0];   /* 条目数 */
    if (avail == 0) return 0;
    int take = (int)(avail < (uint64_t)cap ? avail : (uint64_t)cap);
    if (take <= 0) return 0;
    size_t bytes = (size_t)take * sizeof(rwproc_hwbp_hit_item_t);
    /* data_size 必须是 bytes，不能是 0：模块端读的是 hdr->buf_size
     * （`size_t size = hdr->buf_size; if (size < sizeof(HWBP_HIT_ITEM)) return -EINVAL;`），
     * 而 resp_cap 只决定用户态 read() 的 count，不写进协议头。
     * 原来这里传 0 → 每命中一次 DETAIL 都 EINVAL → watch_poll 永远 0 条，
     * 表现是「断点注册成功但 hits 恒为 0」。内容不需要下发，所以 data 仍传 NULL。 */
    ssize_t r2 = rwproc_call_ex(d, d->rwproc_bp_fd, RWBP_CMD_GET_HWBP_HIT_DETAIL,
                                bp_id, 0, 0, NULL, bytes, bytes, out, bytes);
    if (r2 < 0) return -1;
    int got = (int)((size_t)r2 / sizeof(rwproc_hwbp_hit_item_t));
    if (real) *real = got;
    return 0;
}

int rwproc_hwbp_stop(gg_driver_t *d, uint64_t bp_id) {
    if (d->rwproc_bp_fd < 0) return -1;
    rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_UNINST_PROCESS_HWBP, bp_id, 0, 0, NULL, 0);
    return 0;
}

/* 通用批量注册（驱动 read() 命令分发 + CMD 13 INST_PROCESS_HWBP_ALL）:
 * 驱动内 for_each_thread 枚举进程全部线程并各注册一个断点, 返回句柄数组。
 * 不依赖用户态枚举线程, 新线程/线程数超上限由驱动兜底（上限 GG_HWBP_MAX_ALL=256）。 */
int rwproc_hwbp_start_all(gg_driver_t *d, pid_t pid, uint64_t addr, int type, int len,
                          uint64_t *ids, int cap, int *count) {
    if (d->rwproc_bp_fd < 0) return -1;
    if (type <= 0) type = HW_BP_TYPE_W;
    if (len <= 0) len = 4;
    if (count) *count = 0;
    if (!ids || cap <= 0) { set_err(d, "rwbp start_all bad args"); return -1; }

    /* 逐线程注册。驱动侧没有 CMD_INST_PROCESS_HWBP_ALL(13) 的实现，
     * 而 for_each_thread 在内核里遍历线程的锁风险高；这里在用户态枚举
     * /proc/<pid>/task，对每个线程走一次已验证可靠的 OPEN -> INST -> CLOSE。
     * perf_event 是 per-task 的，任务切换时硬件槽位自动换装，
     * 不受每 CPU 断点槽数限制，效果与驱动内批量注册等价。 */
    char dir[64];
    snprintf(dir, sizeof(dir), "/proc/%d/task", (int)pid);
    DIR *dp = opendir(dir);
    if (!dp) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwbp start_all: cannot open %s: %s", dir, strerror(errno));
        return -1;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL && n < cap) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        pid_t tid = (pid_t)atoi(de->d_name);
        uint64_t id = 0;
        /* 单个线程失败不致命：线程可能刚退出，或该 task 已被释放 */
        if (rwproc_hwbp_start(d, tid, addr, type, len, &id) != 0) continue;
        ids[n++] = id;
    }
    closedir(dp);
    if (count) *count = n;
    if (n == 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwbp start_all: no thread registered for pid %d", (int)pid);
        return -1;
    }
    return 0;
}

/* ---------- KO 加载 ---------- */

/* ---------- 对外接口 ---------- */

int driver_self_test(gg_driver_t *d) {
    static volatile uint32_t s_magic = 0x12345678u;
    uint32_t back = 0;
    pid_t self = (pid_t)getpid();
    d->state = ST_UNAVAILABLE;
    /* ko_loaded 以"通道自检/节点打开"为准，与 /proc/modules 名字匹配解耦：
     * 驱动伪装改名（apr_dlkm 等）后按名字查必然误报；自检回环通过即驱动
     * 真实加载（节点在、ioctl 注册、数据可读），名字无关。 */
    d->ko_loaded = 0;

    /* rwproc33 型: 打开自身进程句柄 + 读回环自检。
     *
     * ⚠️ 这个自检**不能当硬门禁**（2026-10-22 真机）：
     *   同一台设备、同一个 KO（build/ko/_dev/rwProcMem_gki.ko，307KB 修复版），
     *   干净重启后第一次跑 `channel OK`，之后再启动几次就变成
     *   `data r=4 0x0, stack r=4 0x0`（读成功、内容全 0）——
     *   HEAD 版和拆分版的二进制都复现，与代码无关。
     *   而同一时刻读**目标进程**完全正常（systemui/phone 搜索拿到 2500 条合理结果）。
     *
     *   也就是说：驱动对「目标 == 当前进程」这条路径有缺陷（返回成功但填 0），
     *   对正常跨进程读没有影响。拿它判死整条通道，等于让一个自检缺陷把
     *   内存读写/搜索/冻结全废掉 —— 得不偿失。
     *
     * 现在的口径：
     *   读到期望值          -> 通道 OK（正常路径）
     *   读成功但值不对      -> 通道按可用处理，但留 WARN（真问题，别让它静默）
     *   读失败 / 打不开句柄 -> 仍判不可用（那才是真的不通）
     *
     * 依据"节点能打开 + CMD_INIT_DEVICE_INFO 成功"这两条硬事实：attach 阶段
     * 已经跑过 INIT，走到这里说明驱动活着。 */
    if (d->rwproc_mem_fd >= 0) {
        volatile uint32_t s_stack_magic = 0x5a5a1234u;
        uint32_t back2 = 0;
        uint64_t h = 0;
        if (rwproc_open_process(d, self, &h) == 0) {
            ssize_t r1 = rwproc_read_mem(d, self, (uint64_t)(uintptr_t)&s_magic,
                                         &back, sizeof(back), 0);
            ssize_t r2 = rwproc_read_mem(d, self, (uint64_t)(uintptr_t)&s_stack_magic,
                                         &back2, sizeof(back2), 0);
            int ok1 = (r1 == (ssize_t)sizeof(back) && back == 0x12345678u);
            int ok2 = (r2 == (ssize_t)sizeof(back2) && back2 == 0x5a5a1234u);
            if (ok1 && ok2) {
                d->state = ST_DRIVER;
                d->ko_loaded = 1;
                return 1;
            }
            if ((r1 > 0 || r2 > 0) && !ok1 && !ok2) {
                log_msg("self-test: WARN self-read returned wrong values "
                        "(data r=%zd 0x%x, stack r=%zd 0x%x) — 驱动读自身进程有缺陷，"
                        "跨进程读不受影响；通道按可用处理", r1, (unsigned)back,
                        r2, (unsigned)back2);
                d->state = ST_DRIVER;
                d->ko_loaded = 1;
                return 1;
            }
            log_msg("self-test: both reads FAILED (handle=0x%llx data r=%zd, stack r=%zd)",
                    (unsigned long long)h, r1, r2);
        } else {
            log_msg("self-test: open_process(self=%d) failed: %s", (int)self, d->last_error);
        }
        set_err(d, "rwproc self-test read failed");
    }

    set_err(d, "all channels unavailable (not root? KO not loaded?)");
    d->state = ST_UNAVAILABLE;
    return -1;
}

ssize_t driver_read(gg_driver_t *d, pid_t pid, uint64_t addr, void *buf, size_t len) {
    return driver_read_ex(d, pid, addr, buf, len, 0);
}

ssize_t driver_read_ex(gg_driver_t *d, pid_t pid, uint64_t addr, void *buf,
                       size_t len, int force) {
    /* force=1 会让驱动临时改 PTE 去读未驻留/受限页。
     * 2026-10-22 实测：在 rwProcMem_6.12.69 上对活跃进程（agentd 自身）使用，
     * 连续两次触发内核崩溃 → 设备重启（uptime 归零、adb 掉线）。
     * 这与后端注释里"避免与目标进程活跃页竞争导致内核崩溃"的判断一致。
     * 驱动修好之前一律拒绝并给出原因 —— 比静默忽略或让用户把设备搞崩都好。 */
    if (force) {
        set_err(d, "force=1 disabled: rwproc33 is_force_read panics this kernel "
                   "(RWPROC33_ADAPTER.md 10.4); retry with force=0");
        return -1;
    }
    /* ---- 专用 KO 驱动 (dev/qx/rwproc): 内核通信唯一通道 ----
     * 驱动 ioctl/节点读失败 → 直接报错返回, 绝不留落到用户态
     * (裸 process_vm_readv / proc/pid/mem), 避免语义不一致/可被检测的越界读取。
     * 调用方（搜索引擎等）已按实际返回字节数处理部分读。 */
    {
        if (d->rwproc_mem_fd < 0) {
            set_err(d, "rwproc node not open (attach failed?)");
            return -1;
        }
        ssize_t r = rwproc_read_mem(d, pid, addr, buf, len, force);
        if (r >= 0) { atomic_fetch_add_explicit(&d->stat_reads, 1, memory_order_relaxed); return r; }
        return -1;   /* last_error 已由 rwproc_call 置好 */
    }
}

ssize_t driver_write(gg_driver_t *d, pid_t pid, uint64_t addr, const void *buf, size_t len) {
    /* 专用 KO 驱动: 内核通信唯一通道, 失败即报错, 不落用户态（同 driver_read） */
    {
        if (d->rwproc_mem_fd < 0) {
            set_err(d, "rwproc node not open (attach failed?)");
            return -1;
        }
        ssize_t r = rwproc_write_mem(d, pid, addr, buf, len);
        if (r >= 0) { atomic_fetch_add_explicit(&d->stat_writes, 1, memory_order_relaxed); return r; }
        return -1;
    }
}

void driver_close(gg_driver_t *d) {
    {
        if (d->rwproc_handle) {
            rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_CLOSE_PROCESS,
                        d->rwproc_handle, 0, 0, NULL, 0);
            d->rwproc_handle = 0;
        }
        if (d->rwproc_mem_fd >= 0) { close(d->rwproc_mem_fd); d->rwproc_mem_fd = -1; }
        if (d->rwproc_bp_fd >= 0) { close(d->rwproc_bp_fd); d->rwproc_bp_fd = -1; }
        return;
    }
}

const char *driver_state_str(int state) {
    switch (state) {
    case ST_DRIVER:  return "kernel-driver (rwproc33)";
    default:         return "unavailable";
    }
}
