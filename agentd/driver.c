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
#include "version.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
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

/* ---------- 降级: process_vm_readv（hook 型 KO 的主通道） ---------- */
static ssize_t vm_read(pid_t pid, void *buf, size_t len, uint64_t addr) {
#if defined(__linux__) || defined(__ANDROID__)
    struct iovec local = { .iov_base = buf, .iov_len = len };
    struct iovec remote = { .iov_base = (void *)(uintptr_t)addr, .iov_len = len };
    return process_vm_readv(pid, &local, 1, &remote, 1, 0);
#else
    (void)pid; (void)buf; (void)len; (void)addr;
    errno = ENOSYS;
    return -1;
#endif
}

static ssize_t vm_write(pid_t pid, const void *buf, size_t len, uint64_t addr) {
#if defined(__linux__) || defined(__ANDROID__)
    struct iovec local = { .iov_base = (void *)buf, .iov_len = len };
    struct iovec remote = { .iov_base = (void *)(uintptr_t)addr, .iov_len = len };
    return process_vm_writev(pid, &local, 1, &remote, 1, 0);
#else
    (void)pid; (void)buf; (void)len; (void)addr;
    errno = ENOSYS;
    return -1;
#endif
}

/* ---------- 降级: /proc/<pid>/mem ---------- */
static ssize_t mem_io(pid_t pid, void *buf, size_t len, uint64_t addr, int write_mode) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)pid);
    int fd = open(path, write_mode ? O_RDWR : O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = write_mode ? pwrite(fd, buf, len, (off_t)addr)
                           : pread(fd, buf, len, (off_t)addr);
    int saved = errno;
    close(fd);
    errno = saved;
    return r;
}

/* ---------- dev 型后端 ---------- */
static int dev_open(gg_driver_t *d) {
    if (d->dev_path[0] == 0) { set_err(d, "dev path empty"); return -1; }
    int fd = open(d->dev_path, O_RDWR);
    if (fd < 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "open %s failed: %s", d->dev_path, strerror(errno));
        return -1;
    }
    d->dev_fd = fd;
    return 0;
}

static ssize_t dev_io(gg_driver_t *d, int reqcode, pid_t pid,
                      uint64_t addr, void *buf, size_t len) {
    if (d->dev_fd < 0 && dev_open(d) < 0) return -1;
    ko_ioctl_req_t req;
    memset(&req, 0, sizeof(req));
    req.pid  = (uint32_t)pid;
    req.addr = addr;
    req.buf  = (uint64_t)(uintptr_t)buf;
    req.size = (uint64_t)len;
    /* 内核内存 ioctl 可能被信号打断返回 -EINTR（驱动内部 wait 被打断）。
     * 有界重试，避免误判为通道故障。 */
    errno = 0;
    long r;
    int tries = 0;
    do {
        r = ioctl(d->dev_fd, reqcode, &req);
    } while (r < 0 && errno == EINTR && ++tries < 3);
    return (ssize_t)r;
}

/* ---------- qx 型后端（Aloha 开源 QX9-10） ---------- */

static int qx_io(gg_driver_t *d, int op, pid_t pid, uint64_t addr, void *buf, size_t len) {
    if (d->dev_fd < 0) return -1;
    qx_copy_memory_t cm;
    cm.pid = pid;
    cm.addr = (uintptr_t)addr;
    cm.buffer = buf;
    cm.size = len;
    errno = 0;
    int r;
    int tries = 0;
    do {
        r = ioctl(d->dev_fd, op, &cm);
    } while (r < 0 && errno == EINTR && ++tries < 3);
    return r;
}

/* 发现 QX 隐藏驱动进程: /proc/<pid>/exe -> /data/<6位小写>（已删除）。
 * 返回 pid；-1 未发现。 */
int qx_find_hidden_pid(void) {
    DIR *dp = opendir("/proc");
    if (!dp) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(dp)) != NULL && found < 0) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char exe[64];
        snprintf(exe, sizeof(exe), "/proc/%s/exe", de->d_name);
        char link[256];
        ssize_t n = readlink(exe, link, sizeof(link) - 1);
        if (n <= 0) continue;
        link[n] = 0;
        /* 部分内核 readlink 会把 " (deleted)" 拼进目标路径, 剥离后再匹配 */
        char *del = strstr(link, " (deleted)");
        if (del) *del = 0;
        /* 形如 /data/abcxyz 且目标已删除（access 失败） */
        if (strncmp(link, "/data/", 6) != 0) continue;
        if (strlen(link) != 12) continue;          /* "/data/" + 6 字符 */
        int ok = 1;
        for (int i = 6; i < 12; i++) {
            if (link[i] < 'a' || link[i] > 'z') { ok = 0; break; }
        }
        if (!ok) continue;
        if (access(link, F_OK) == 0) continue;      /* 目标还存在 → 不是 deleted */
        found = atoi(de->d_name);
    }
    closedir(dp);
    return found;
}

int qx_attach(gg_driver_t *d) {
    /* 1) 发现隐藏驱动进程 */
    int pid = qx_find_hidden_pid();
    if (pid < 0) {
        set_err(d, "QX driver not found (no /data/[a-z]{6} deleted exe process)");
        return -GGERR_NOT_FOUND;
    }
    /* 2) 从 /proc/<pid>/fd/3 取主次设备号 */
    char fdpath[64];
    snprintf(fdpath, sizeof(fdpath), "/proc/%d/fd/3", pid);
    struct stat st;
    if (stat(fdpath, &st) != 0 || !S_ISCHR(st.st_mode)) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "stat %s failed (fd 3 not char device?)", fdpath);
        return -GGERR_NOT_FOUND;
    }
    unsigned maj = major(st.st_rdev);
    unsigned min = minor(st.st_rdev);

    /* 3) 重建节点（路径自选, 主次号一致即可） */
    snprintf(d->dev_path, sizeof(d->dev_path), "/dev/ggqx");
    unlink(d->dev_path);
    if (mknod(d->dev_path, S_IFCHR | 0666, makedev(maj, min)) != 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "mknod %s c %u %u failed: %s", d->dev_path, maj, min, strerror(errno));
        return -GGERR_PERM;
    }
    chmod(d->dev_path, 0666);

    /* 4) open */
    d->dev_fd = open(d->dev_path, O_RDWR);
    if (d->dev_fd < 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "open %s failed: %s", d->dev_path, strerror(errno));
        return -GGERR_PERM;
    }

    /* 5) init_key 认证（可选） */
    if (d->qx_have_key) {
        char key[QX_KEY_MAX];
        memset(key, 0, sizeof(key));
        strncpy(key, d->qx_key, sizeof(key) - 1);
        if (ioctl(d->dev_fd, QX_OP_INIT_KEY, key) != 0) {
            snprintf(d->last_error, sizeof(d->last_error),
                     "QX init_key (0x800) failed: %s", strerror(errno));
            return -GGERR_PERM;
        }
    }

    snprintf(d->ko_module, sizeof(d->ko_module), "qx");
    snprintf(d->ko_version, sizeof(d->ko_version), "qx9-10");
    snprintf(d->ko_file, sizeof(d->ko_file),
             "attached via %s (maj %u min %u, pid %d)", d->dev_path, maj, min, pid);
    d->ko_loaded = 1;
    return 0;
}

int driver_module_base(gg_driver_t *d, pid_t pid, const char *name, uint64_t *base) {
    if (d->ko_type != KO_TYPE_QX || d->dev_fd < 0) return -1;
    qx_module_base_t mb;
    char namebuf[0x100];
    memset(namebuf, 0, sizeof(namebuf));
    strncpy(namebuf, name, sizeof(namebuf) - 1);
    mb.pid = pid;
    mb.name = namebuf;
    mb.base = 0;
    if (ioctl(d->dev_fd, QX_OP_MODULE_BASE, &mb) != 0) return -1;
    *base = mb.base;
    return 0;
}

/* ---------- QX 硬件断点（0x807-0x811, qx-对接.h Paradise 家族） ---------- */

int qx_hwbp_add(gg_driver_t *d, pid_t pid, uint64_t addr, int type, int len) {
    if (d->ko_type != KO_TYPE_QX || d->dev_fd < 0) return -1;
    qx_hw_bp_info_t bp;
    memset(&bp, 0, sizeof(bp));
    bp.pid = pid;
    bp.addr = addr;
    bp.type = type;
    bp.len = len;
    errno = 0;
    return ioctl(d->dev_fd, QX_OP_HWBP_ADD, &bp);
}

int qx_hwbp_get_hits(gg_driver_t *d, pid_t pid, uint64_t addr,
                     qx_hwbp_hit_item_t *out, int cap, int *real) {
    if (d->ko_type != KO_TYPE_QX || d->dev_fd < 0) return -1;
    qx_hwbp_hit_args_t args;
    memset(&args, 0, sizeof(args));
    args.pid = pid;
    args.addr = addr;
    args.out_buf = out;
    args.out_len = cap * (int)sizeof(qx_hwbp_hit_item_t);
    args.real_count = 0;
    errno = 0;
    int rc = ioctl(d->dev_fd, QX_OP_HWBP_GET_HITS, &args);
    if (real) *real = args.real_count;
    return rc;
}

/* enable/disable/clear 的参数布局 qx-对接.h 未明示; 常见实现为 pid。
 * 先传 &pid, 失败再试 NULL。 */
static int qx_hwbp_simple(gg_driver_t *d, pid_t pid, int op) {
    if (d->ko_type != KO_TYPE_QX || d->dev_fd < 0) return -1;
    errno = 0;
    if (ioctl(d->dev_fd, op, &pid) == 0) return 0;
    return ioctl(d->dev_fd, op, NULL);
}

int qx_hwbp_enable(gg_driver_t *d, pid_t pid)  { return qx_hwbp_simple(d, pid, QX_OP_HWBP_ENABLE); }
int qx_hwbp_disable(gg_driver_t *d, pid_t pid) { return qx_hwbp_simple(d, pid, QX_OP_HWBP_DISABLE); }
int qx_hwbp_clear(gg_driver_t *d, pid_t pid)   { return qx_hwbp_simple(d, pid, QX_OP_HWBP_CLEAR); }

/* ---------- rwproc33 型后端（abcz316/rwProcMem33, proc read 33B 头协议） ----------
 * 协议（见 docs/RWPROC33_ADAPTER.md + 上游源码）:
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

/* 通用调用: 往节点 fd 发一个请求。data 为 buf+33 处数据（可 NULL）。
 * 成功返回 read() 结果（>=0）；失败返回 -1 并置 last_error。 */
static ssize_t rwproc_call(gg_driver_t *d, int fd, char cmd,
                           uint64_t p1, uint64_t p2, uint64_t p3,
                           const void *data, size_t data_size) {
    if (fd < 0) { set_err(d, "rwproc node not open"); return -1; }
    if (data_size > RWPROC_MAX_IO) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwproc request too large: %zu", data_size);
        return -1;
    }
    size_t total = RWPROC_HDR_SZ + data_size;
    rwproc_request_t *req = (rwproc_request_t *)rwproc_buf;
    memset(rwproc_buf, 0, total);
    req->cmd = cmd;
    req->param1 = p1;
    req->param2 = p2;
    req->param3 = p3;
    req->buf_size = data_size;
    if (data && data_size) memcpy(rwproc_buf + RWPROC_HDR_SZ, data, data_size);
    errno = 0;
    ssize_t r = read(fd, rwproc_buf, total);
    if (r < 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwproc read(cmd=%d) failed: %s", (int)cmd, strerror(errno));
        return -1;
    }
    return r;
}

/* 打开 /proc/<key>/<key> 节点。key 为空时扫描 /proc 找"目录名==内文件名"的节点
 * （模块未隐藏时可发现；已隐藏则必须配置 key）。返回 fd；-1 失败。 */
static int rwproc_open_node(gg_driver_t *d, const char *key) {
    if (key && *key) {
        char path[256];
        snprintf(path, sizeof(path), "/proc/%s/%s", key, key);
        int fd = open(path, O_RDWR);
        if (fd >= 0) return fd;
        snprintf(d->last_error, sizeof(d->last_error),
                 "open %s failed: %s", path, strerror(errno));
        return -1;
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
        fd = open(sub, O_RDWR);
        if (fd >= 0) {
            snprintf(d->last_error, sizeof(d->last_error),
                     "auto-discovered rwproc node: %s", sub);
        }
    }
    closedir(dp);
    if (fd < 0) set_err(d, "rwproc node not found (module not loaded? set --rwproc-key)");
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
    int rc = driver_init(d, "rwproc", NULL, 0, 0);
    if (rc != 0) return rc;
    d->ko_type = KO_TYPE_RWPROC;
    d->rwproc_mem_fd = -1;
    d->rwproc_bp_fd = -1;
    d->rwproc_handle = 0;
    d->rwproc_pid = -1;
    if (mem_key && *mem_key) snprintf(d->rwproc_mem_key, sizeof(d->rwproc_mem_key), "%s", mem_key);
    if (bp_key && *bp_key) snprintf(d->rwproc_bp_key, sizeof(d->rwproc_bp_key), "%s", bp_key);
    return 0;
}

int rwproc_attach(gg_driver_t *d) {
    if (d->ko_type != KO_TYPE_RWPROC) return -1;
    /* 1) 打开内存模块节点 */
    d->rwproc_mem_fd = rwproc_open_node(d, d->rwproc_mem_key);
    if (d->rwproc_mem_fd < 0) return -GGERR_NOT_FOUND;
    /* 2) 打开断点模块节点（可选; 失败不影响内存读写） */
    d->rwproc_bp_fd = rwproc_open_node(d, d->rwproc_bp_key);
    if (d->rwproc_bp_fd < 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "hwBreakpoint node open failed (watch 不可用, 内存读写正常): %s",
                 d->last_error);
    }
    /* 3) CMD_INIT_DEVICE_INFO（必须首个命令） */
    if (rwproc_init_device_info(d, d->rwproc_mem_fd) != 0) {
        close(d->rwproc_mem_fd);
        d->rwproc_mem_fd = -1;
        return -GGERR_INTERNAL;
    }
    snprintf(d->ko_module, sizeof(d->ko_module), "rwproc33");
    snprintf(d->ko_version, sizeof(d->ko_version), "rwProcMem33");
    snprintf(d->ko_file, sizeof(d->ko_file),
             "proc nodes: mem=%s bp=%s", d->rwproc_mem_key, d->rwproc_bp_key);
    d->ko_loaded = 1;
    return 0;
}

int rwproc_open_process(gg_driver_t *d, pid_t pid, uint64_t *handle) {
    if (d->ko_type != KO_TYPE_RWPROC || d->rwproc_mem_fd < 0) return -1;
    /* 换 pid: 关旧开新 */
    if (d->rwproc_handle && d->rwproc_pid != pid) {
        rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_CLOSE_PROCESS,
                    d->rwproc_handle, 0, 0, NULL, 0);
        d->rwproc_handle = 0;
    }
    if (!d->rwproc_handle) {
        ssize_t r = rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_OPEN_PROCESS,
                                (uint64_t)pid, 0, 0, NULL, 0);
        if (r < 0) return -1;
        if (r != 0) {
            snprintf(d->last_error, sizeof(d->last_error),
                     "rwproc CMD_OPEN_PROCESS(pid=%d) failed: %lld", (int)pid, (long long)r);
            return -1;
        }
        uint64_t h = 0;
        memcpy(&h, rwproc_buf + RWPROC_HDR_SZ, sizeof(h));
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
    if (d->ko_type != KO_TYPE_RWPROC || d->rwproc_mem_fd < 0 || !handle) return -1;
    rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_CLOSE_PROCESS, handle, 0, 0, NULL, 0);
    if (d->rwproc_handle == handle) { d->rwproc_handle = 0; d->rwproc_pid = -1; }
    return 0;
}

/* rwproc 读（调用方已持有引擎锁）。len 上限 RWPROC_MAX_IO。
 * is_force=0: 只读映射权限允许的页（不翻转 PTE, 避免与目标进程活跃页竞争导致崩溃）。
 * 读失败返回部分/错误, 调用方按实际字节数处理。 */
static ssize_t rwproc_read_mem(gg_driver_t *d, pid_t pid, uint64_t addr,
                               void *buf, size_t len) {
    if (len > RWPROC_MAX_IO) len = RWPROC_MAX_IO;
    if (rwproc_open_process(d, pid, NULL) != 0) return -1;
    ssize_t r = rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_READ_PROCESS_MEMORY,
                            d->rwproc_handle, addr, 0 /* is_force */, NULL, len);
    if (r < 0) return -1;
    memcpy(buf, rwproc_buf + RWPROC_HDR_SZ, (size_t)r);
    return r;
}

static ssize_t rwproc_write_mem(gg_driver_t *d, pid_t pid, uint64_t addr,
                                const void *buf, size_t len) {
    if (len > RWPROC_MAX_IO) len = RWPROC_MAX_IO;
    if (rwproc_open_process(d, pid, NULL) != 0) return -1;
    return rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_WRITE_PROCESS_MEMORY,
                       d->rwproc_handle, addr, 0 /* is_force */, buf, len);
}

/* ---------- rwproc33 硬件断点（hwBreakpointProc_module） ---------- */

int rwproc_hwbp_start(gg_driver_t *d, pid_t pid, uint64_t addr, int type, int len,
                      uint64_t *bp_id) {
    if (d->ko_type != KO_TYPE_RWPROC || d->rwproc_bp_fd < 0) return -1;
    if (type <= 0) type = HW_BP_TYPE_W;
    if (len <= 0) len = 4;
    uint64_t handle = 0;
    /* 断点模块的 OPEN_PROCESS 是独立句柄（与内存模块句柄不同） */
    ssize_t r = rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_OPEN_PROCESS,
                            (uint64_t)pid, 0, 0, NULL, 0);
    if (r < 0) return -1;
    if (r != 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwbp CMD_OPEN_PROCESS(pid=%d) failed: %lld", (int)pid, (long long)r);
        return -1;
    }
    memcpy(&handle, rwproc_buf + RWPROC_HDR_SZ, sizeof(handle));
    if (!handle) { set_err(d, "rwbp CMD_OPEN_PROCESS null handle"); return -1; }
    /* INST: param3 = len | (type << 8) */
    ssize_t r2 = rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_INST_PROCESS_HWBP,
                             handle, addr, RWBP_PARAM3(len, type), NULL, 0);
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
    uint64_t id = 0;
    memcpy(&id, rwproc_buf + RWPROC_HDR_SZ, sizeof(id));
    if (!id) { set_err(d, "rwbp INST returned null bp id"); return -1; }
    if (bp_id) *bp_id = id;
    return 0;
}

int rwproc_hwbp_get_hits(gg_driver_t *d, uint64_t bp_id,
                         rwproc_hwbp_hit_item_t *out, int cap, int *real) {
    if (d->ko_type != KO_TYPE_RWPROC || d->rwproc_bp_fd < 0) return -1;
    if (real) *real = 0;
    /* 命中数（模块返回 {hit_total_count, hit_item_arr_count} 到数据区） */
    ssize_t r = rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_GET_HWBP_HIT_COUNT,
                            bp_id, 0, 0, NULL, 0);
    if (r < 0) return -1;
    uint64_t counts[2] = {0, 0};
    memcpy(counts, rwproc_buf + RWPROC_HDR_SZ, sizeof(counts));
    uint64_t avail = counts[1] ? counts[1] : counts[0];   /* 条目数 */
    if (avail == 0) return 0;
    int take = (int)(avail < (uint64_t)cap ? avail : (uint64_t)cap);
    if (take <= 0) return 0;
    size_t bytes = (size_t)take * sizeof(rwproc_hwbp_hit_item_t);
    ssize_t r2 = rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_GET_HWBP_HIT_DETAIL,
                             bp_id, 0, 0, NULL, bytes);
    if (r2 < 0) return -1;
    int got = (int)((size_t)r2 / sizeof(rwproc_hwbp_hit_item_t));
    if (got > 0) memcpy(out, rwproc_buf + RWPROC_HDR_SZ,
                        (size_t)got * sizeof(rwproc_hwbp_hit_item_t));
    if (real) *real = got;
    return 0;
}

int rwproc_hwbp_stop(gg_driver_t *d, uint64_t bp_id) {
    if (d->ko_type != KO_TYPE_RWPROC || d->rwproc_bp_fd < 0) return -1;
    rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_UNINST_PROCESS_HWBP, bp_id, 0, 0, NULL, 0);
    return 0;
}

/* ---------- KO 加载 ---------- */

static int ko_insmod(const char *path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "insmod '%s' 2>/dev/null", path);
    int rc = system(cmd);
    if (rc == 0) return 0;
    /* 部分设备 insmod 不在 PATH：尝试 /system/bin/insmod */
    snprintf(cmd, sizeof(cmd), "/system/bin/insmod '%s' 2>/dev/null", path);
    return system(cmd);
}

static int ko_in_modules(const char *modname) {
    FILE *f = fopen("/proc/modules", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, modname, strlen(modname)) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

/* 根据 /proc/misc 建 dev 节点: misc 主设备号 10, 次设备号从 /proc/misc 查 */
static int ko_mknod_misc(const char *name) {
    FILE *f = fopen("/proc/misc", "r");
    if (!f) return -1;
    char line[256];
    int minor = -1;
    while (fgets(line, sizeof(line), f)) {
        int m;
        char n[128];
        if (sscanf(line, "%d %127s", &m, n) == 2 && strcmp(n, name) == 0) { minor = m; break; }
    }
    fclose(f);
    if (minor < 0) return -1;
    char path[160];
    snprintf(path, sizeof(path), "/dev/%s", name);
    unlink(path);
    if (mknod(path, S_IFCHR | 0666, makedev(10, minor)) < 0) return -1;
    chmod(path, 0666);
    return 0;
}

int ko_load(gg_driver_t *d, const char *dir, const char *file,
            const char *pattern, int strict) {
    char path[1024];
    char chosen[512] = "";
    char chosen_ver[64] = "";

    if (file && *file) {
        snprintf(path, sizeof(path), "%s/%s", dir, file);
        if (access(path, R_OK) != 0) {
            snprintf(d->last_error, sizeof(d->last_error), "ko file not found: %s", path);
            return -GGERR_NOT_FOUND;
        }
        snprintf(chosen, sizeof(chosen), "%s", file);
        version_from_ko_name(file, chosen_ver, sizeof(chosen_ver));
    } else {
        /* 自动选择：目录内第一个族匹配的 .ko */
        DIR *dp = opendir(dir);
        if (!dp) {
            snprintf(d->last_error, sizeof(d->last_error), "ko dir not found: %s", dir);
            return -GGERR_NOT_FOUND;
        }
        struct dirent *de;
        int best = -1;   /* 0=找到, 1=找到但族不符 */
        while ((de = readdir(dp)) != NULL) {
            size_t l = strlen(de->d_name);
            if (l < 4 || strcmp(de->d_name + l - 3, ".ko") != 0) continue;
            char ver[64] = "";
            version_from_ko_name(de->d_name, ver, sizeof(ver));
            int match = 1;
            if (pattern && *pattern && ver[0]) {
                int m = version_match_family(ver, pattern);
                if (m < 0) match = 1;      /* 版本不可解析 → 放行 */
                else match = m;
            }
            if (match) {
                snprintf(chosen, sizeof(chosen), "%s", de->d_name);
                snprintf(chosen_ver, sizeof(chosen_ver), "%s", ver);
                best = 0;
                break;
            } else if (best < 0) {
                best = 1;   /* 记录存在族外文件 */
                snprintf(chosen, sizeof(chosen), "%s", de->d_name);
                snprintf(chosen_ver, sizeof(chosen_ver), "%s", ver);
            }
        }
        closedir(dp);
        if (chosen[0] == 0) {
            set_err(d, "no .ko file found in ko dir");
            return -GGERR_NOT_FOUND;
        }
        if (best == 1 && strict) {
            snprintf(d->last_error, sizeof(d->last_error),
                     "no .ko matches version family '%s' (found %s v%s); strict mode",
                     pattern ? pattern : "", chosen, chosen_ver);
            return -GGERR_VERSION_MISMATCH;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, chosen);
    }

    /* SELinux: 多数社区 KO 需要 permissive */
    system("setenforce 0 2>/dev/null");

    /* 已加载则跳过 insmod */
    char modname[256];
    snprintf(modname, sizeof(modname), "%s", chosen);
    char *dot = strstr(modname, ".ko");
    if (dot) *dot = 0;
    d->ko_loaded = ko_in_modules(modname);

    if (!d->ko_loaded) {
        if (ko_insmod(path) != 0) {
            snprintf(d->last_error, sizeof(d->last_error),
                     "insmod %s failed (kernel version mismatch? SELinux? perms?)", path);
            return -GGERR_PERM;
        }
        d->ko_loaded = ko_in_modules(modname);
        if (!d->ko_loaded) {
            set_err(d, "insmod succeeded but module not in /proc/modules");
            return -GGERR_INTERNAL;
        }
    }

    snprintf(d->ko_module, sizeof(d->ko_module), "%s", modname);
    snprintf(d->ko_file, sizeof(d->ko_file), "%s", path);
    snprintf(d->ko_version, sizeof(d->ko_version), "%s", chosen_ver);

    /* dev 型: 建节点并打开 */
    if (d->ko_type == KO_TYPE_DEV) {
        if (d->dev_path[0] == 0) {
            /* 未显式指定 → 尝试 misc 设备名 = 模块名 */
            snprintf(d->dev_path, sizeof(d->dev_path), "/dev/%s", modname);
        }
        if (access(d->dev_path, F_OK) != 0) {
            ko_mknod_misc(modname);
        }
        if (dev_open(d) < 0) {
            return -GGERR_INTERNAL;   /* last_error 已置 */
        }
    }
    return 0;
}

/* ---------- 对外接口 ---------- */

int driver_init(gg_driver_t *d, const char *type,
                const char *dev_path, int ioctl_read, int ioctl_write) {
    memset(d, 0, sizeof(*d));
    d->dev_fd = -1;
    d->ioctl_read  = ioctl_read  ? ioctl_read  : 0x7e1a;
    d->ioctl_write = ioctl_write ? ioctl_write : 0x7e1b;
    d->state = ST_UNAVAILABLE;

    if (type && strcmp(type, "dev") == 0) {
        d->ko_type = KO_TYPE_DEV;
        if (dev_path) snprintf(d->dev_path, sizeof(d->dev_path), "%s", dev_path);
    } else if (type && strcmp(type, "qx") == 0) {
        d->ko_type = KO_TYPE_QX;
        d->ioctl_read  = QX_OP_READ_MEM;
        d->ioctl_write = QX_OP_WRITE_MEM;
    } else if (type && strcmp(type, "rwproc") == 0) {
        d->ko_type = KO_TYPE_RWPROC;
        d->rwproc_mem_fd = -1;
        d->rwproc_bp_fd = -1;
        d->rwproc_pid = -1;
    } else {
        d->ko_type = KO_TYPE_HOOK;   /* 默认 hook 型 */
    }
    return 0;
}

int driver_init_qx(gg_driver_t *d, const char *key) {
    int rc = driver_init(d, "qx", NULL, 0, 0);
    if (rc != 0) return rc;
    if (key && *key) {
        memset(d->qx_key, 0, sizeof(d->qx_key));
        strncpy(d->qx_key, key, sizeof(d->qx_key) - 1);
        d->qx_have_key = 1;
    }
    return 0;
}

int driver_self_test(gg_driver_t *d) {
    static volatile uint32_t s_magic = 0x12345678u;
    uint32_t back = 0;
    pid_t self = (pid_t)getpid();
    d->state = ST_UNAVAILABLE;

    /* rwproc33 型: 打开自身进程句柄 + 读回环 */
    if (d->ko_type == KO_TYPE_RWPROC && d->rwproc_mem_fd >= 0) {
        uint64_t h = 0;
        if (rwproc_open_process(d, self, &h) == 0) {
            ssize_t r = rwproc_read_mem(d, self, (uint64_t)(uintptr_t)&s_magic,
                                        &back, sizeof(back));
            if (r == (ssize_t)sizeof(back) && back == 0x12345678u) {
                d->state = ST_DRIVER;
                return 1;
            }
        }
        set_err(d, "rwproc self-test read failed");
    }

    /* qx / dev 型: ioctl 回环 */
    if ((d->ko_type == KO_TYPE_QX || d->ko_type == KO_TYPE_DEV) && d->dev_fd >= 0) {
        ssize_t r;
        if (d->ko_type == KO_TYPE_QX) {
            r = qx_io(d, QX_OP_READ_MEM, self,
                      (uint64_t)(uintptr_t)&s_magic, &back, sizeof(back));
            if (r == 0) {
                d->state = ST_DRIVER;
                return 1;
            }
        } else {
            r = dev_io(d, d->ioctl_read, self,
                       (uint64_t)(uintptr_t)&s_magic, &back, sizeof(back));
            if (r == (ssize_t)sizeof(back) && back == 0x12345678u) {
                d->state = ST_DRIVER;
                return 1;
            }
            /* 写通道回环 */
            ssize_t w = dev_io(d, d->ioctl_write, self,
                               (uint64_t)(uintptr_t)&s_magic, (void *)&s_magic, sizeof(s_magic));
            ssize_t r2 = dev_io(d, d->ioctl_read, self,
                                (uint64_t)(uintptr_t)&s_magic, &back, sizeof(back));
            if (w == (ssize_t)sizeof(s_magic) && r2 == (ssize_t)sizeof(back) &&
                back == 0x12345678u) {
                d->state = ST_DRIVER;
                return 1;
            }
        }
    }

    /* hook 型: process_vm_readv 是内核 hook 的主通道, /proc/pid/mem 为内核兜底。
     * 其它专用驱动型（dev/qx/rwproc）不在此兜底——KO 自检失败即如实报不可用,
     * 不被裸 process_vm_readv 假阳性掩盖（用户态通道）。 */
    if (d->ko_type == KO_TYPE_HOOK) {
        ssize_t rv = vm_read(self, &back, sizeof(back), (uint64_t)(uintptr_t)&s_magic);
        if (rv == (ssize_t)sizeof(back) && back == 0x12345678u) {
            d->state = ST_KO_HOOK;
            return 1;
        }
        ssize_t rm = mem_io(self, &back, sizeof(back), (uint64_t)(uintptr_t)&s_magic, 0);
        if (rm == (ssize_t)sizeof(back) && back == 0x12345678u) {
            d->state = ST_PROC_MEM;
            return 0;
        }
    }
    set_err(d, "all channels unavailable (not root? KO not loaded?)");
    d->state = ST_UNAVAILABLE;
    return -1;
}

ssize_t driver_read(gg_driver_t *d, pid_t pid, uint64_t addr, void *buf, size_t len) {
    /* ---- 专用 KO 驱动 (dev/qx/rwproc): 内核通信唯一通道 ----
     * 驱动 ioctl/节点读失败 → 直接报错返回, 绝不留落到用户态
     * (裸 process_vm_readv / /proc/pid/mem), 避免语义不一致/可被检测的越界读取。
     * 调用方（搜索引擎等）已按实际返回字节数处理部分读。 */
    if (d->ko_type == KO_TYPE_RWPROC) {
        if (d->rwproc_mem_fd < 0) {
            set_err(d, "rwproc node not open (attach failed?)");
            return -1;
        }
        ssize_t r = rwproc_read_mem(d, pid, addr, buf, len);
        if (r >= 0) { atomic_fetch_add_explicit(&d->stat_reads, 1, memory_order_relaxed); return r; }
        return -1;   /* last_error 已由 rwproc_call 置好 */
    }
    if (d->ko_type == KO_TYPE_QX) {
        if (d->dev_fd < 0) {
            set_err(d, "qx dev node not open (attach failed?)");
            return -1;
        }
        if (qx_io(d, QX_OP_READ_MEM, pid, addr, buf, len) == 0) {
            atomic_fetch_add_explicit(&d->stat_reads, 1, memory_order_relaxed);
            return (ssize_t)len;
        }
        snprintf(d->last_error, sizeof(d->last_error),
                 "qx read ioctl failed: %s", strerror(errno));
        return -1;
    }
    if (d->ko_type == KO_TYPE_DEV) {
        ssize_t r = dev_io(d, d->ioctl_read, pid, addr, buf, len);
        if (r >= 0) { atomic_fetch_add_explicit(&d->stat_reads, 1, memory_order_relaxed); return r; }
        if (d->last_error[0] == 0)
            snprintf(d->last_error, sizeof(d->last_error),
                     "dev read ioctl failed: %s", strerror(errno));
        return -1;
    }

    /* ---- hook 型 KO: 内核 hook process_vm_readv, /proc/pid/mem 为内核兜底 ----
     * 这两条都是内核通道（hook 接管 process_vm_readv）, 保留降级。 */
    atomic_fetch_add_explicit(&d->stat_fallbacks, 1, memory_order_relaxed);
    ssize_t r = vm_read(pid, buf, len, addr);
    if (r >= 0) {
        if (d->state != ST_DRIVER && d->state != ST_KO_HOOK) d->state = ST_PROC_VM;
        return r;
    }
    r = mem_io(pid, buf, len, addr, 0);
    if (r >= 0) {
        if (d->state != ST_DRIVER && d->state != ST_KO_HOOK) d->state = ST_PROC_MEM;
        return r;
    }
    snprintf(d->last_error, sizeof(d->last_error),
             "read failed pid=%d addr=0x%llx len=%zu: %s",
             pid, (unsigned long long)addr, len, strerror(errno));
    return -1;
}

ssize_t driver_write(gg_driver_t *d, pid_t pid, uint64_t addr, const void *buf, size_t len) {
    /* 专用 KO 驱动: 内核通信唯一通道, 失败即报错, 不落用户态（同 driver_read） */
    if (d->ko_type == KO_TYPE_RWPROC) {
        if (d->rwproc_mem_fd < 0) {
            set_err(d, "rwproc node not open (attach failed?)");
            return -1;
        }
        ssize_t r = rwproc_write_mem(d, pid, addr, buf, len);
        if (r >= 0) { atomic_fetch_add_explicit(&d->stat_writes, 1, memory_order_relaxed); return r; }
        return -1;
    }
    if (d->ko_type == KO_TYPE_QX) {
        if (d->dev_fd < 0) {
            set_err(d, "qx dev node not open (attach failed?)");
            return -1;
        }
        if (qx_io(d, QX_OP_WRITE_MEM, pid, addr, (void *)buf, len) == 0) {
            atomic_fetch_add_explicit(&d->stat_writes, 1, memory_order_relaxed);
            return (ssize_t)len;
        }
        snprintf(d->last_error, sizeof(d->last_error),
                 "qx write ioctl failed: %s", strerror(errno));
        return -1;
    }
    if (d->ko_type == KO_TYPE_DEV) {
        ssize_t r = dev_io(d, d->ioctl_write, pid, addr, (void *)buf, len);
        if (r >= 0) { atomic_fetch_add_explicit(&d->stat_writes, 1, memory_order_relaxed); return r; }
        if (d->last_error[0] == 0)
            snprintf(d->last_error, sizeof(d->last_error),
                     "dev write ioctl failed: %s", strerror(errno));
        return -1;
    }

    /* hook 型 KO: 内核通道降级 */
    atomic_fetch_add_explicit(&d->stat_fallbacks, 1, memory_order_relaxed);
    ssize_t r = vm_write(pid, buf, len, addr);
    if (r >= 0) {
        if (d->state != ST_DRIVER && d->state != ST_KO_HOOK) d->state = ST_PROC_VM;
        return r;
    }
    r = mem_io(pid, (void *)buf, len, addr, 1);
    if (r >= 0) {
        if (d->state != ST_DRIVER && d->state != ST_KO_HOOK) d->state = ST_PROC_MEM;
        return r;
    }
    snprintf(d->last_error, sizeof(d->last_error),
             "write failed pid=%d addr=0x%llx len=%zu: %s",
             pid, (unsigned long long)addr, len, strerror(errno));
    return -1;
}

void driver_close(gg_driver_t *d) {
    if (d->ko_type == KO_TYPE_RWPROC) {
        if (d->rwproc_handle) {
            rwproc_call(d, d->rwproc_mem_fd, RWPROC_CMD_CLOSE_PROCESS,
                        d->rwproc_handle, 0, 0, NULL, 0);
            d->rwproc_handle = 0;
        }
        if (d->rwproc_mem_fd >= 0) { close(d->rwproc_mem_fd); d->rwproc_mem_fd = -1; }
        if (d->rwproc_bp_fd >= 0) { close(d->rwproc_bp_fd); d->rwproc_bp_fd = -1; }
        return;
    }
    if (d->dev_fd >= 0) { close(d->dev_fd); d->dev_fd = -1; }
}

const char *driver_state_str(int state) {
    switch (state) {
    case ST_DRIVER:      return "kernel-driver (kernel module)";
    case ST_KO_HOOK:     return "ko-hook (kernel hooks process_vm_readv)";
    case ST_PROC_VM:     return "process_vm_readv (degraded)";
    case ST_PROC_MEM:    return "/proc/pid/mem (degraded)";
    default:             return "unavailable";
    }
}
