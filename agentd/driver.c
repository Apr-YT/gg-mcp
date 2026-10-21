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

/* 通用调用: 往节点 fd 发一个请求。data 为 buf+33 处数据（可 NULL）。
 * 成功返回 read() 结果（>=0）；失败返回 -1 并置 last_error。
 *
 * resp_cap: 期望内核对 buf+33 回写的最大字节数，只影响 read() 的 count。
 *   为什么需要它：read() 的 count 决定内核最多能往用户缓冲区写多少字节，
 *   而请求负载长度（buf_size）与回写长度并不总是相等。OPEN_PROCESS 只发
 *   33 字节头（buf_size=0）却要收回 8 字节 handle —— 若 count 也只给 33，
 *   内核写不进去，随后的 memcpy(buf+33, 8) 取到的是上一次请求的残留数据。
 *   症状：读自身"成功"（残留碰巧有效），读别的进程返回 0 或 EFAULT。 */
static ssize_t rwproc_call_ex(gg_driver_t *d, int fd, char cmd,
                              uint64_t p1, uint64_t p2, uint64_t p3,
                              const void *data, size_t data_size, size_t resp_cap) {
    if (fd < 0) { set_err(d, "rwproc node not open"); return -1; }
    if (data_size > RWPROC_MAX_IO) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwproc request too large: %zu", data_size);
        return -1;
    }
    if (resp_cap > RWPROC_MAX_IO) resp_cap = RWPROC_MAX_IO;
    size_t rd = RWPROC_HDR_SZ + (resp_cap > data_size ? resp_cap : data_size);
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
    ssize_t r = read(fd, rwproc_buf, rd);
    if (r < 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwproc read(cmd=%d) failed: %s", (int)cmd, strerror(errno));
        return -1;
    }
    return r;
}

static ssize_t rwproc_call(gg_driver_t *d, int fd, char cmd,
                           uint64_t p1, uint64_t p2, uint64_t p3,
                           const void *data, size_t data_size) {
    return rwproc_call_ex(d, fd, cmd, p1, p2, p3, data, data_size, 0);
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
    d->rwproc_mem_fd = rwproc_open_node(d, d->rwproc_mem_key);
    if (d->rwproc_mem_fd < 0) return -GGERR_NOT_FOUND;
    /* 2) 打开断点模块节点（可选; 失败不影响内存读写） */
    char mem_err[256];
    snprintf(mem_err, sizeof(mem_err), "%s", d->last_error);
    d->rwproc_bp_fd = rwproc_open_node(d, d->rwproc_bp_key);
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
        ssize_t r = rwproc_call_ex(d, d->rwproc_mem_fd, RWPROC_CMD_OPEN_PROCESS,
                                   (uint64_t)pid, 0, 0, NULL, 0, sizeof(uint64_t));
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
    if (d->rwproc_mem_fd < 0 || !handle) return -1;
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
    /* 防御：内核回写量不该超过我们申请的 len，超出就按 len 截断，
     * 否则 memcpy 会溢出调用方的缓冲区。 */
    if ((size_t)r > len) r = (ssize_t)len;
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
                               (uint64_t)pid, 0, 0, NULL, 0, sizeof(uint64_t));
    if (r < 0) return -1;
    if (r != 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwbp CMD_OPEN_PROCESS(pid=%d) failed: %lld", (int)pid, (long long)r);
        return -1;
    }
    memcpy(&handle, rwproc_buf + RWPROC_HDR_SZ, sizeof(handle));
    if (!handle) { set_err(d, "rwbp CMD_OPEN_PROCESS null handle"); return -1; }
    /* INST: param3 = len | (type << 8) */
    ssize_t r2 = rwproc_call_ex(d, d->rwproc_bp_fd, RWBP_CMD_INST_PROCESS_HWBP,
                                handle, addr, RWBP_PARAM3(len, type), NULL, 0,
                                sizeof(uint64_t));
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
    if (d->rwproc_bp_fd < 0) return -1;
    if (real) *real = 0;
    /* 命中数（模块返回 {hit_total_count, hit_item_arr_count} 到数据区） */
    ssize_t r = rwproc_call_ex(d, d->rwproc_bp_fd, RWBP_CMD_GET_HWBP_HIT_COUNT,
                               bp_id, 0, 0, NULL, 0, sizeof(uint64_t) * 2);
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
    uint64_t handle = 0;
    ssize_t r = rwproc_call_ex(d, d->rwproc_bp_fd, RWBP_CMD_OPEN_PROCESS,
                               (uint64_t)pid, 0, 0, NULL, 0, sizeof(uint64_t));
    if (r < 0) return -1;
    if (r != 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwbp CMD_OPEN_PROCESS(pid=%d) failed: %lld", (int)pid, (long long)r);
        return -1;
    }
    memcpy(&handle, rwproc_buf + RWPROC_HDR_SZ, sizeof(handle));
    if (!handle) { set_err(d, "rwbp CMD_OPEN_PROCESS null handle"); return -1; }
    /* INST_ALL: 返回区 = 4B 注册数 + N*8B 句柄数组 */
    ssize_t r2 = rwproc_call_ex(d, d->rwproc_bp_fd, RWBP_CMD_INST_PROCESS_HWBP_ALL,
                                handle, addr, RWBP_PARAM3(len, type), NULL, 0,
                                sizeof(int) + (size_t)cap * sizeof(uint64_t));
    if (r2 < 0) {
        rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_CLOSE_PROCESS, handle, 0, 0, NULL, 0);
        return -1;
    }
    if (r2 != 0) {
        snprintf(d->last_error, sizeof(d->last_error),
                 "rwbp CMD_INST_ALL failed: %lld", (long long)r2);
        rwproc_call(d, d->rwproc_bp_fd, RWBP_CMD_CLOSE_PROCESS, handle, 0, 0, NULL, 0);
        return -1;
    }
    int n = 0;
    memcpy(&n, rwproc_buf + RWPROC_HDR_SZ, sizeof(n));
    if (n < 0) n = 0;
    if (n > cap) n = cap;
    if (n > 0)
        memcpy(ids, rwproc_buf + RWPROC_HDR_SZ + sizeof(int),
               (size_t)n * sizeof(uint64_t));
    if (count) *count = n;
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

    /* rwproc33 型: 打开自身进程句柄 + 读回环 */
    if (d->rwproc_mem_fd >= 0) {
        uint64_t h = 0;
        if (rwproc_open_process(d, self, &h) == 0) {
            ssize_t r = rwproc_read_mem(d, self, (uint64_t)(uintptr_t)&s_magic,
                                        &back, sizeof(back));
            if (r == (ssize_t)sizeof(back) && back == 0x12345678u) {
                d->state = ST_DRIVER;
                d->ko_loaded = 1;
                return 1;
            }
        }
        set_err(d, "rwproc self-test read failed");
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
    {
        if (d->rwproc_mem_fd < 0) {
            set_err(d, "rwproc node not open (attach failed?)");
            return -1;
        }
        ssize_t r = rwproc_read_mem(d, pid, addr, buf, len);
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
