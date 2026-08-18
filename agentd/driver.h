/* driver.h — agentd 内核内存驱动抽象层（KO 驱动方案）
 *
 * 只加载 KO 内核驱动（外挂圈子方式，不依赖 KPM/KPatch-Next）。四类后端：
 *   1) hook 型 KO: 内核 hook process_vm_readv/writev，用户态零协议，
 *      直接走 process_vm_readv（内核已接管）→ /proc/pid/mem 兜底。
 *   2) dev 型 KO: 注册 /dev 节点（misc 设备），用户态 open + ioctl 读写。
 *   3) qx 型（Aloha 开源 QX9-10, 雪花/Paradise 家族）: 隐藏进程持有已删除的设备节点,
 *      通过 /proc/<pid>/fd/3 的 st_rdev 重建节点后 open；
 *      ioctl 0x800 init_key / 0x801 读 / 0x802 写 / 0x803 模块基址,
 *      COPY_MEMORY{pid,addr,buffer,size} 自然对齐（非 packed）。
 *   4) rwproc33 型（abcz316/rwProcMem33, 开源）: 两个 LKM（rwProcMem_module +
 *      hwBreakpointProc_module），各自创建 /proc/<AUTH_KEY>/<AUTH_KEY> 节点，
 *      用 read() 系统调用走 33 字节头协议（非 ioctl）。
 *      详见 docs/RWPROC33_ADAPTER.md。
 *
 * KO 选择支持版本族容错：文件名版本与模式（如 6.12.*）做族匹配，
 * 同族补丁号任意均可加载（复用 version.c 的 version_match_family）。
 */
#ifndef GG_AGENTD_DRIVER_H
#define GG_AGENTD_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "../shared/wire_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* KO 后端类型 */
enum gg_ko_type {
    KO_TYPE_HOOK = 1,   /* 内核 hook process_vm_readv */
    KO_TYPE_DEV  = 2,   /* /dev 节点 + ioctl（通用布局） */
    KO_TYPE_QX   = 3,   /* Aloha 开源 QX9-10（隐藏进程 + 节点重建） */
    KO_TYPE_RWPROC = 4  /* abcz316/rwProcMem33（proc read 33B 头协议, 开源） */
};

/* QX ioctl 操作码（与 qx-对接.h 一致） */
#define QX_OP_INIT_KEY     0x800
#define QX_OP_READ_MEM     0x801
#define QX_OP_WRITE_MEM    0x802
#define QX_OP_MODULE_BASE  0x803
#define QX_OP_HIDE_PROCESS    0x804
#define QX_OP_RECOVER_PROCESS 0x805
#define QX_OP_GYRO            0x806
#define QX_OP_HWBP_ADD        0x807
#define QX_OP_HWBP_GET_HITS   0x808
#define QX_OP_HWBP_ENABLE     0x809
#define QX_OP_HWBP_CLEAR      0x810
#define QX_OP_HWBP_DISABLE    0x811
#define QX_OP_UPDATE_TRACKING 0x812
#define QX_KEY_MAX         0x100

/* 硬件断点类型 */
#define HW_BP_TYPE_R   1
#define HW_BP_TYPE_W   2
#define HW_BP_TYPE_RW  3
#define HW_BP_TYPE_X   4
#define QX_MAX_MODIFY_REGS 10

/* HWBP_ADD 参数（0x807, 与 qx-对接.h HW_BP_INFO 布局一致, 自然对齐） */
typedef struct qx_hw_bp_info {
    pid_t     pid;                    /* 0   */
    uintptr_t addr;                   /* 8   */
    int       type;                   /* 16  */
    int       len;                    /* 20  */
    int       is_write_gp_regs;       /* 24  (bool) + pad */
    int       gp_reg_count;           /* 28  */
    int       gp_reg_indices[QX_MAX_MODIFY_REGS];      /* 32  */
    uint64_t  gp_reg_values[QX_MAX_MODIFY_REGS];       /* 72  */
    int       is_write_fp_regs;       /* 152 */
    int       fp_reg_count;           /* 156 */
    int       fp_reg_indices[QX_MAX_MODIFY_REGS];      /* 160 */
    uint64_t  fp_reg_values[QX_MAX_MODIFY_REGS][2];    /* 200 */
} qx_hw_bp_info_t;                    /* sizeof 360 */

/* 命中寄存器快照 */
typedef struct qx_regs_info {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
} qx_regs_info_t;

/* 命中条目 */
typedef struct qx_hwbp_hit_item {
    pid_t        task_id;    /* 0  */
    uintptr_t    hit_addr;   /* 8  */
    uint64_t     hit_time;   /* 16 */
    qx_regs_info_t regs;     /* 24 */
} qx_hwbp_hit_item_t;        /* sizeof 296 */

/* HWBP_GET_HITS 参数（0x808） */
typedef struct qx_hwbp_hit_args {
    pid_t     pid;           /* 0  */
    uintptr_t addr;          /* 8  */
    void     *out_buf;       /* 16 */
    int       out_len;       /* 24 */
    int       real_count;    /* 28 */
} qx_hwbp_hit_args_t;        /* sizeof 32 */

/* ---- rwproc33 型（abcz316/rwProcMem33） ---- */

/* rwProcMem_module / hwBreakpointProc_module 共用的 33 字节请求头（packed）：
 * read(fd, buf, 33+data_size)：头在 buf[0..32]，数据区在 buf[33..]；
 * 结果（读回的数据 / 返回句柄 / 命中条目）也写在 buf[33..]。 */
typedef struct __attribute__((packed)) rwproc_request {
    char     cmd;
    uint64_t param1;
    uint64_t param2;
    uint64_t param3;
    uint64_t buf_size;
} rwproc_request_t;            /* sizeof 33 */

/* GG-TOUCH 触摸注入参数（display 坐标, 与驱动 rwProcMem_module.c 的
 * struct gg_touch_tap / gg_touch_swipe 完全一致; 驱动内做 disp→raw 换算） */
typedef struct __attribute__((packed)) rwproc_gg_touch_tap {
    uint32_t x, y, hold_ms;
} rwproc_gg_touch_tap_t;       /* sizeof 12 */

typedef struct __attribute__((packed)) rwproc_gg_touch_swipe {
    uint32_t x1, y1, x2, y2, ms;
} rwproc_gg_touch_swipe_t;     /* sizeof 20 */

/* rwProcMem_module 命令码（与源码 rwProcMem_module.c 一致） */
enum {
    RWPROC_CMD_INIT_DEVICE_INFO     = 1,
    RWPROC_CMD_OPEN_PROCESS         = 2,
    RWPROC_CMD_READ_PROCESS_MEMORY  = 3,
    RWPROC_CMD_WRITE_PROCESS_MEMORY = 4,
    RWPROC_CMD_CLOSE_PROCESS        = 5,
    RWPROC_CMD_GET_PROCESS_MAPS_COUNT = 6,
    RWPROC_CMD_GET_PROCESS_MAPS_LIST  = 7,
    RWPROC_CMD_CHECK_PROCESS_ADDR_PHY = 8,
    RWPROC_CMD_GET_PID_LIST          = 9,
    RWPROC_CMD_SET_PROCESS_ROOT      = 10,
    RWPROC_CMD_GET_PROCESS_RSS       = 11,
    RWPROC_CMD_GET_PROCESS_CMDLINE_ADDR = 12,
    RWPROC_CMD_HIDE_KERNEL_MODULE    = 13,
    RWPROC_CMD_GGTOUCH_TAP           = 15,  /* 内核虚拟触摸: 单点 tap（display 坐标, 驱动内换算 raw） */
    RWPROC_CMD_GGTOUCH_SWIPE         = 16   /* 内核虚拟触摸: 滑动 swipe */
};

/* hwBreakpointProc_module 命令码（与源码 hwBreakpointProc_module.c 一致） */
enum {
    RWBP_CMD_OPEN_PROCESS        = 1,
    RWBP_CMD_CLOSE_PROCESS       = 2,
    RWBP_CMD_GET_NUM_BRPS        = 3,
    RWBP_CMD_GET_NUM_WRPS        = 4,
    RWBP_CMD_INST_PROCESS_HWBP   = 5,
    RWBP_CMD_UNINST_PROCESS_HWBP = 6,
    RWBP_CMD_SUSPEND_PROCESS_HWBP= 7,
    RWBP_CMD_RESUME_PROCESS_HWBP = 8,
    RWBP_CMD_GET_HWBP_HIT_COUNT  = 9,
    RWBP_CMD_GET_HWBP_HIT_DETAIL = 10,
    RWBP_CMD_SET_HOOK_PC         = 11,
    RWBP_CMD_HIDE_KERNEL_MODULE  = 12,
    RWBP_CMD_INST_PROCESS_HWBP_ALL = 13  /* 驱动内 for_each_thread 全线程批量注册 */
};

/* hwBreakpointProc_module 的命中条目（packed, 与源码 HWBP_HIT_ITEM 一致, sizeof 312） */
typedef struct __attribute__((packed)) rwproc_hwbp_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t orig_x0;
    uint64_t syscallno;
} rwproc_hwbp_regs_t;

typedef struct __attribute__((packed)) rwproc_hwbp_hit_item {
    uint64_t task_id;
    uint64_t hit_addr;        /* 模块实现里 = 命中指令 pc */
    uint64_t hit_time;
    rwproc_hwbp_regs_t regs_info;
} rwproc_hwbp_hit_item_t;     /* sizeof 312 */

/* INST_PROCESS_HWBP 的 param3 编码: 低字节 len, 次字节 type（HW_BP_TYPE_*） */
#define RWBP_PARAM3(len, type)  ((uint64_t)((len) & 0xFF) | ((uint64_t)(type) << 8))

typedef struct gg_driver {
    int   state;             /* gg_engine_state */

    /* KO 加载信息 */
    int   ko_loaded;         /* KO 是否已加载（/proc/modules 可查） */
    char  ko_module[256];    /* 模块名（.ko 文件名去后缀 / "qx"） */
    char  ko_file[512];      /* 实际加载的 .ko 路径 / 节点发现说明 */
    char  ko_version[64];    /* 文件名提取的版本 / 驱动标识 */
    int   ko_type;           /* gg_ko_type */

    /* dev 型后端 */
    int   dev_fd;            /* /dev 节点 fd（-1=未打开） */
    char  dev_path[128];     /* /dev 节点路径 */
    int   ioctl_read;        /* 读请求码（dev: 0x7e1a; qx: 0x801） */
    int   ioctl_write;       /* 写请求码（dev: 0x7e1b; qx: 0x802） */

    /* qx 后端 */
    char  qx_key[QX_KEY_MAX]; /* init_key 密钥（0x100 字节） */
    int   qx_have_key;        /* 是否配置了密钥 */

    /* rwproc33 后端 */
    int    rwproc_mem_fd;     /* rwProcMem_module 节点 fd（-1=未打开） */
    int    rwproc_bp_fd;      /* hwBreakpointProc_module 节点 fd（-1=未打开） */
    uint64_t rwproc_handle;   /* 当前 OPEN_PROCESS 的进程句柄（0=未打开） */
    pid_t  rwproc_pid;        /* 句柄对应的 pid（换 pid 时自动关旧开新） */
    char   rwproc_mem_key[96];/* 内存模块 auth key（节点名, 空=自动扫描） */
    char   rwproc_bp_key[96]; /* 断点模块 auth key */

    /* X3: 统计计数原子化——KERNEL_STATUS 走快速通道（无锁）在搜索期间读取，
     * 与搜索线程的原子累加并发；_Atomic 消除数据竞争（ARM64 上零额外开销）。 */
    _Atomic uint64_t stat_reads;
    _Atomic uint64_t stat_writes;
    _Atomic uint64_t stat_fallbacks;
    char  last_error[256];   /* X3: 快速通道读取容忍撕裂（仅内容短暂不完整, JSON 仍合法） */
} gg_driver_t;

/* 初始化后端: type ∈ "hook" | "dev" | "qx"。
 * dev 型需 dev_path/ioctl 码；qx 型用 driver_init_qx。返回 0 或负错误码。 */
int driver_init(gg_driver_t *d, const char *type,
                const char *dev_path, int ioctl_read, int ioctl_write);

/* 初始化 qx 后端（key 可为 NULL/空 → 跳过 init_key 认证） */
int driver_init_qx(gg_driver_t *d, const char *key);

/* 初始化 rwproc33 后端。mem_key/bp_key 为两模块的 proc 节点 auth key
 * （可 NULL/空 → 自动扫描 /proc 找 <key>/<key> 节点，但节点被隐藏时扫描不到）。 */
int driver_init_rwproc(gg_driver_t *d, const char *mem_key, const char *bp_key);

/* rwproc33 后端: 打开两个 proc 节点 + CMD_INIT_DEVICE_INFO（须在一切读写前调用一次）。
 * 返回 0 成功；负值对应 GGERR_*。 */
int rwproc_attach(gg_driver_t *d);

/* rwproc33: 打开/关闭进程（句柄供 read/write/watch 使用）。0 成功。 */
int rwproc_open_process(gg_driver_t *d, pid_t pid, uint64_t *handle);
int rwproc_close_process(gg_driver_t *d, uint64_t handle);

/* rwproc33 硬件断点（hwBreakpointProc_module, perf_event 框架, 无密钥门控）：
 *  rwproc_hwbp_start: 安装断点 {pid, addr, type(HW_BP_TYPE_*), len} → *bp_id（perf_event 指针）。
 *  rwproc_hwbp_get_hits: 取命中（out 数组, cap 个; *real 返回实际数）。模块无清除命令,
 *    命中在断点生命周期内累计（上限 1024），调用方需按已消费数去重。
 *  rwproc_hwbp_stop: 卸载断点。0 成功；-1 失败/后端不支持。 */
int rwproc_hwbp_start(gg_driver_t *d, pid_t pid, uint64_t addr, int type, int len,
                      uint64_t *bp_id);
/* 通用批量注册: 驱动内 for_each_thread 枚举进程全部线程并各注册一个断点。
 * ids 返回句柄数组（cap 个）, *count 返回实际注册数。一次调用全覆盖, 不依赖用户态枚举线程。 */
int rwproc_hwbp_start_all(gg_driver_t *d, pid_t pid, uint64_t addr, int type, int len,
                          uint64_t *ids, int cap, int *count);
int rwproc_hwbp_get_hits(gg_driver_t *d, uint64_t bp_id,
                         rwproc_hwbp_hit_item_t *out, int cap, int *real);
int rwproc_hwbp_stop(gg_driver_t *d, uint64_t bp_id);

/* 加载 KO：
 *   dir      — .ko 所在目录（如 /data/adb/vendor_security/ko）
 *   file     — 指定 .ko 文件名（空=自动按版本族选择第一个）
 *   pattern  — 版本族模式（如 "6.12.*"；空=不校验）
 *   strict   — 族外拒绝
 * 返回 0 成功；负值对应 GGERR_*。
 */
int ko_load(gg_driver_t *d, const char *dir, const char *file,
            const char *pattern, int strict);

/* qx 后端: 发现隐藏驱动进程 → 重建设备节点 → open → init_key。
 * 返回 0 成功；负值对应 GGERR_*。 */
int qx_attach(gg_driver_t *d);

/* 发现 QX 隐藏驱动进程: /proc/<pid>/exe -> /data/<6位小写>（已删除）。
 * 返回 pid；-1 未发现。 */
int qx_find_hidden_pid(void);

/* 驱动级模块基址（qx 0x803）。返回 0 成功；-1 后端不支持/失败。 */
int driver_module_base(gg_driver_t *d, pid_t pid, const char *name, uint64_t *base);

/* QX 硬件断点（qx-对接.h 0x807-0x811, Paradise 家族）:
 *  qx_hwbp_add:     添加断点 {pid, addr, type(HW_BP_TYPE_*), len}。0 成功。
 *  qx_hwbp_get_hits: 取命中（out 数组, cap 个; *real 返回实际数）。0 成功。
 *  qx_hwbp_enable/disable/clear: 启用/停用/清空。0 成功。
 * 非 qx 后端返回 -1。 */
int qx_hwbp_add(gg_driver_t *d, pid_t pid, uint64_t addr, int type, int len);
int qx_hwbp_get_hits(gg_driver_t *d, pid_t pid, uint64_t addr,
                     qx_hwbp_hit_item_t *out, int cap, int *real);
int qx_hwbp_enable(gg_driver_t *d, pid_t pid);
int qx_hwbp_disable(gg_driver_t *d, pid_t pid);
int qx_hwbp_clear(gg_driver_t *d, pid_t pid);

/* 自检: 回环读自身静态变量。返回 1=驱动通道 OK, 0=仅降级可用, -1=不可用。 */
int driver_self_test(gg_driver_t *d);

/* 跨进程读/写。返回实际字节数；失败返回 -1 并置 last_error。 */
ssize_t driver_read(gg_driver_t *d, pid_t pid, uint64_t addr, void *buf, size_t len);
ssize_t driver_write(gg_driver_t *d, pid_t pid, uint64_t addr, const void *buf, size_t len);

/* GG-TOUCH 内核虚拟触摸注入（仅 rwproc33 型后端支持; 其他后端返回 -1）。
 * display 坐标（横屏 2608x1200 逻辑像素）, 驱动内部换算 raw 并注入 input 子系统。
 * 返回 0 成功；-1 失败并置 last_error。 */
int driver_touch_tap(gg_driver_t *d, uint32_t x, uint32_t y, uint32_t hold_ms);
int driver_touch_swipe(gg_driver_t *d, uint32_t x1, uint32_t y1,
                       uint32_t x2, uint32_t y2, uint32_t ms);

void driver_close(gg_driver_t *d);

/* 状态描述串（供 KERNEL_STATUS 输出） */
const char *driver_state_str(int state);

#ifdef __cplusplus
}
#endif

#endif /* GG_AGENTD_DRIVER_H */
