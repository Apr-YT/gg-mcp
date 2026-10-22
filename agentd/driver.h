/* driver.h — agentd 内核内存驱动抽象层（仅 rwproc33）
 
 *
 * rwproc33 型说明：
 *   两个 LKM（rwProcMem_module + hwBreakpointProc_module），各自创建
 *   /proc/<AUTH_KEY>/<AUTH_KEY> 节点，用 read() 走 33 字节头协议（非 ioctl）。
 *   详见 docs/engineering/RWPROC33_ADAPTER.md。
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

/* 硬件断点类型（hwBreakpointProc_module 的 param3 编码用） */
#define HW_BP_TYPE_R   1
#define HW_BP_TYPE_W   2
#define HW_BP_TYPE_RW  3
#define HW_BP_TYPE_X   4

/* ---- rwproc33（abcz316/rwProcMem33） ---- */

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
    int   ko_loaded;         /* 通道自检/节点打开成功即置位 */
    char  ko_module[256];    /* 模块标识（固定 "rwproc33"） */
    char  ko_file[512];      /* 节点发现说明 */
    char  ko_version[64];    /* 驱动标识 */

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

/* 初始化 rwproc33 后端。mem_key/bp_key 为两模块的 proc 节点 auth key
 * （可 NULL/空 → 自动扫描 /proc 找 <key>/<key> 节点，但节点被隐藏时扫描不到）。 */
int driver_init_rwproc(gg_driver_t *d, const char *mem_key, const char *bp_key);

/* 打开两个 proc 节点 + CMD_INIT_DEVICE_INFO（须在一切读写前调用一次）。
 * 返回 0 成功；负值对应 GGERR_*。 */
int rwproc_attach(gg_driver_t *d);

/* 打开/关闭进程（句柄供 read/write/watch 使用）。0 成功。 */
int rwproc_open_process(gg_driver_t *d, pid_t pid, uint64_t *handle);
int rwproc_close_process(gg_driver_t *d, uint64_t handle);

/* 硬件断点（hwBreakpointProc_module, perf_event 框架, 无密钥门控）：
 *  rwproc_hwbp_start: 安装断点 {pid, addr, type(HW_BP_TYPE_*), len} → *bp_id（perf_event 指针）。
 *  rwproc_hwbp_get_hits: 取命中（out 数组, cap 个; *real 返回实际数）。模块无清除命令,
 *    命中在断点生命周期内累计（上限 1024），调用方需按已消费数去重。
 *  rwproc_hwbp_stop: 卸载断点。0 成功；-1 失败。 */
int rwproc_hwbp_start(gg_driver_t *d, pid_t pid, uint64_t addr, int type, int len,
                      uint64_t *bp_id);
/* 通用批量注册: 驱动内 for_each_thread 枚举进程全部线程并各注册一个断点。
 * ids 返回句柄数组（cap 个）, *count 返回实际注册数。一次调用全覆盖, 不依赖用户态枚举线程。 */
int rwproc_hwbp_start_all(gg_driver_t *d, pid_t pid, uint64_t addr, int type, int len,
                          uint64_t *ids, int cap, int *count);
/* 探测断点模块的命令码基准：GET_NUM_BRPS(3) 返回执行断点槽数。
 * 我们的构建返回 6；0 起始的旧模块会返回 4（那条其实是 GET_NUM_WRPS）。*/
int rwproc_hwbp_probe(gg_driver_t *d);

int rwproc_hwbp_get_hits(gg_driver_t *d, uint64_t bp_id,
                         rwproc_hwbp_hit_item_t *out, int cap, int *real);
int rwproc_hwbp_stop(gg_driver_t *d, uint64_t bp_id);

/* 自检: 回环读自身静态变量。返回 1=通道 OK, -1=不可用。 */
int driver_self_test(gg_driver_t *d);

/* 跨进程读/写。返回实际字节数；失败返回 -1 并置 last_error。 */
ssize_t driver_read(gg_driver_t *d, pid_t pid, uint64_t addr, void *buf, size_t len);

/* 带 force 的读：force=1 让驱动临时改 PTE 强制读（可用于未驻留页/受限页）。
 * 只有 rwproc 后端有此语义，其余后端忽略。driver_read 等价于 force=0。
 * 注意：force=1 会与目标进程的活跃页竞争，可能触发内核崩溃，只在必要时用。 */
ssize_t driver_read_ex(gg_driver_t *d, pid_t pid, uint64_t addr, void *buf,
                       size_t len, int force);
ssize_t driver_write(gg_driver_t *d, pid_t pid, uint64_t addr, const void *buf, size_t len);

/* GG-TOUCH 内核虚拟触摸注入。
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
