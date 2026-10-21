/*
 * wire_protocol.h — gg-agentd ↔ gg-ghost-mcp 通信协议定义（唯一事实源）
 *
 * 帧格式：20 字节二进制头 + JSON 负载
 *   头:  { u32 magic; u16 ver; u16 cmd; u32 seq; u32 flags; u32 payload_len }
 *   负载: 请求 {"p":{...}} / 响应 {"r":{...}} / 错误 {"e":{"code":n,"message":"..."}}
 *        / 事件 {"ev":{...}}（flags 带 GGWF_EVENT）
 *
 * 本头文件为纯 C，PC(C++) 与设备(C) 共用。
 */
#ifndef GG_WIRE_PROTOCOL_H
#define GG_WIRE_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GGWM_MAGIC       0x47474D43u   /* "GGMC" */
#define GGWM_VERSION     1u
#define GGWM_HEADER_LEN  20u
#define GGWM_MAX_PAYLOAD (1u << 20)    /* 单帧负载上限 1MiB：防恶意/异常超大帧耗尽内存（后续大响应可分片/流式帧） */

/* 帧标志 */
#define GGWF_REQ    0x0001u   /* 请求 */
#define GGWF_RESP   0x0002u   /* 响应 */
#define GGWF_ERR    0x0004u   /* 错误响应（负载为 {"e":{code,message}}） */
#define GGWF_EVENT  0x0008u   /* 异步事件（无 seq 语义） */
#define GGWF_ENC    0x0010u   /* 负载已 XOR 混淆（滚动 key，防被动嗅探一眼看穿） */

/* 负载混淆（GGWF_ENC）: 滚动 XOR, 每字节异或 (GG_XOR_SEED + 位置)。
 * 同一函数既加密也解密（XOR 自逆）。仅作防"明文秒懂"的轻量混淆，
 * 非加密安全——对抗主动分析的强度取决于部署端随机化。 */
#define GG_XOR_SEED 0xA5u
static inline void gg_xor_payload(unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        buf[i] ^= (unsigned char)(GG_XOR_SEED + i);
}

typedef struct gg_wire_header {
    uint32_t magic;
    uint16_t ver;
    uint16_t cmd;
    uint32_t seq;
    uint32_t flags;
    uint32_t payload_len;
} gg_wire_header_t;

/* 命令码 */
enum gg_cmd {
    GGCMD_PING = 1,
    GGCMD_SELF_TEST,            /* 驱动通道自检 */
    GGCMD_LIST_PROCESSES,
    GGCMD_ATTACH,
    GGCMD_DETACH,
    GGCMD_TARGET_INFO,          /* 目标进程映射概况 + 模块列表 */
    GGCMD_MODULE_LIST,
    GGCMD_SEARCH,               /* 已知值精确搜索 */
    GGCMD_SEARCH_UNKNOWN_INIT,  /* 模糊搜索初始化（位图+快照） */
    GGCMD_SEARCH_REFINE,        /* 模糊细化 */
    GGCMD_SEARCH_RANGE,         /* 区间搜索 */
    GGCMD_RESULT_COUNT,
    GGCMD_GET_RESULTS,          /* 分页获取结果（地址+当前值） */
    GGCMD_CLEAR_RESULTS,
    GGCMD_READ,
    GGCMD_READ_BYTES,
    GGCMD_HEX_DUMP,
    GGCMD_WRITE,
    GGCMD_SET_RESULTS,          /* 按 index/address 批量修改 */
    GGCMD_FREEZE,
    GGCMD_UNFREEZE,
    GGCMD_MODULE_BASE,
    GGCMD_CALC_OFFSET,
    GGCMD_POINTER_SCAN,
    GGCMD_PAUSE,                /* SIGSTOP */
    GGCMD_RESUME,               /* SIGCONT */
    GGCMD_KERNEL_STATUS,
    GGCMD_LOAD_GG,              /* 加载 GG 核心（带版本族校验） */
    GGCMD_CORE_VERSIONS,        /* 列出可加载的 GG 版本族 */
    GGCMD_SHELL,                /* root shell（白名单，默认关闭） */
    GGCMD_DEVICE_INFO,
    GGCMD_WATCH,                /* 硬件断点监视（CE 式"什么访问了这个地址"） */
    GGCMD_DISASSEMBLE,          /* 反汇编目标进程内存（Capstone ARM64） */
    GGCMD_VIEW_MEMORY,          /* 结构化查看内存（int32/float32/pointer 元素列表，AI 理解结构体） */
    GGCMD_RESOLVE_SYMBOL,       /* 地址→模块/偏移/最近导出符号（本地静态解析，ELF .dynsym） */
    GGCMD_POINTER_CHAIN,        /* 多级指针追踪（逐级解引用到基址，标注模块偏移） */
    GGCMD_SUBSCRIBE_EVENTS,     /* 订阅异步事件流（手机悬浮窗等展示端；未订阅不收 GGWF_EVENT 帧） */
    GGCMD_UNSUBSCRIBE_EVENTS,   /* 取消订阅 */
    GGCMD_RELAY_EVENT,          /* PC 端转发事件（标注等 PC 本地动作 → agentd 广播给订阅端）:
                                 * 负载 {"p":{"type":"annotate","data":{...}}} → 事件 {"ev":{"type":...,"data":...}} */
    GGCMD_GHOST_TAP,            /* 内核虚拟触摸: 单点 tap {"p":{"x":..,"y":..,"hold_ms":..}} */
    GGCMD_GHOST_SWIPE,          /* 内核虚拟触摸: 滑动 {"p":{"x1":..,"y1":..,"x2":..,"y2":..,"ms":..}} */
    /* 握手鉴权（必须放在枚举末尾，保证既有命令码的数值不变）：
     * 请求 {"p":{"key":"<hex>"}}；成功响应 {"r":{"authed":true}}，失败 GGERR_PERM。
     * 未携带有效密钥的连接只允许 PING / AUTH，其余命令一律 GGERR_PERM。
     * 目的是堵住「设备上任意本地 App 连 127.0.0.1:34127 即可获得 root 能力」的面。 */
    GGCMD_AUTH,
    GGCMD_MAX
};

/* 错误码 */
enum gg_err {
    GGERR_OK = 0,
    GGERR_BAD_REQUEST = 1,      /* 参数/负载错误 */
    GGERR_NO_DEVICE = 2,        /* 设备不可达（adb 断连） */
    GGERR_NO_SESSION = 3,       /* 未 attach 或会话已失效 */
    GGERR_PERM = 4,             /* 权限不足（非 root） */
    GGERR_VERSION_MISMATCH = 5, /* GG 核心版本族不符 */
    GGERR_DEGRADED = 6,         /* 驱动不可用，当前为降级模式 */
    GGERR_TIMEOUT = 7,
    GGERR_NOT_FOUND = 8,        /* 进程/模块/地址不存在 */
    GGERR_ARCH_UNSUPPORTED = 9, /* 32 位目标进程不支持 */
    GGERR_CAP = 10,             /* 结果超过上限 */
    GGERR_BUSY = 11,            /* 引擎忙（并发冲突） */
    GGERR_INTERNAL = 12,
    GGERR_SAFE_MODE = 13,       /* 只读会话拒绝写操作 */
    GGERR_SHELL_DISABLED = 14,
    GGERR_MAX
};

/* 值类型 */
enum gg_vtype {
    VT_BYTE = 1,
    VT_WORD = 2,   /* u16 */
    VT_DWORD = 3,  /* u32 */
    VT_QWORD = 4,  /* u64 */
    VT_FLOAT = 5,  /* f32 */
    VT_DOUBLE = 6, /* f64 */
    VT_BYTES = 7   /* AoB 字节序列（value=hex 字符串, 如 "48 65 4C 4C"） */
};

/* 模糊细化操作 */
enum gg_refine_op {
    REFINE_INCREASED = 1,
    REFINE_DECREASED = 2,
    REFINE_UNCHANGED = 3,
    REFINE_CHANGED = 4
};

/* 搜索区域过滤 */
enum gg_region {
    REGION_ANONYMOUS = 1,  /* 匿名映射（堆/栈，默认） */
    REGION_WRITABLE = 2,   /* 可写映射 */
    REGION_ALL = 3
};

/* 会话/引擎状态，供 KERNEL_STATUS / STATUS 使用 */
enum gg_engine_state {
    ST_DRIVER = 1,       /* 内核驱动通道可用（rwproc33） */
    ST_UNAVAILABLE = 4   /* 通道不可用；2/3/5 为已删除的降级态，不再产生 */
};

/* 公共请求/响应字段名（JSON payload 约定） */
#define GG_J_REQ      "p"
#define GG_J_RES      "r"
#define GG_J_ERR      "e"
#define GG_J_EVT      "ev"

#ifdef __cplusplus
}
#endif

#endif /* GG_WIRE_PROTOCOL_H */
