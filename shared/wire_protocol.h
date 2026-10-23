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

#include <stddef.h>   /* size_t —— 本头文件必须自包含: 只含 stdint.h 时,
                       * 任何未先引入 stddef 的翻译单元都会在 gg_xor_payload 的
                       * 签名上失败（Linux GCC: 'size_t' has not been declared）。 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GGWM_MAGIC       0x47474D43u   /* "GGMC" */
#define GGWM_VERSION     1u
#define GGWM_HEADER_LEN  20u
#define GGWM_MAX_PAYLOAD (1u << 20)    /* 单帧负载上限 1MiB：防恶意/异常超大帧耗尽内存（后续大响应可分片/流式帧） */

/* 响应负载能否装进单帧。
 *
 * 超限时**不要**直接把负载交给发送函数：帧头里的 payload_len 是 uint32，
 * 而超限帧会被拒发 —— 客户端什么也收不到，只能等到连接层超时（PC 端 300s），
 * 从外部完全看不出是"结果太大"。正确做法是回一个 GGERR_CAP 错误帧，
 * 把实际字节数和上限一起告诉调用方（见 agentd 的 send_ok）。 */
static inline int gg_payload_fits(size_t len) {
    return len <= (size_t)GGWM_MAX_PAYLOAD;
}

/* 大响应分片（GGWF_MORE）
 *
 * 单帧 1MiB 是硬上限，而响应体没有：gg_get_results limit=10000 就有 510KB，
 * 一旦将来放宽 limit 或加字段就会越界 —— 那时要么截断要么报错，都不好用。
 *
 * 规则：
 *   - 只有 GGWF_RESP 帧可以带 GGWF_MORE
 *   - 同一响应的所有分片共用同一个 seq；最后一片不带 MORE
 *   - 每片 ≤ GGWM_MAX_PAYLOAD，接收端拼起来不得超过 GGWM_MAX_REASSEMBLED
 *   - **能力协商**：客户端在 AUTH 请求里带 "frag":1 表示能重组，agentd 才分片；
 *     不带就仍按单帧上限处理（超限回 GGERR_CAP）。这样旧客户端行为完全不变。
 */
#define GGWM_MAX_REASSEMBLED (8u << 20)   /* 重组后的上限 8MiB，防对端无限分片 */

/* 帧标志 */
#define GGWF_REQ    0x0001u   /* 请求 */
#define GGWF_RESP   0x0002u   /* 响应 */
#define GGWF_ERR    0x0004u   /* 错误响应（负载为 {"e":{code,message}}） */
#define GGWF_EVENT  0x0008u   /* 异步事件（无 seq 语义） */
#define GGWF_ENC    0x0010u   /* 负载已 XOR 混淆（滚动 key，防被动嗅探一眼看穿） */
#define GGWF_MORE   0x0020u   /* 本帧是大响应的分片，后面还有同 seq 的帧（见上） */

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
    /* 28/29 保留占位：GG 核心注入链路（lib5_stub + libKernelGg.so）已于
     * 2026-10-21 整条删除。枚举值不能回收 —— 删掉会让后续命令码整体前移，
     * 与已部署的 agentd 错位。调用这两个命令会得到明确的 BAD_REQUEST。
     * 也不要往里插新命令：C 枚举允许重复值且不报错，插进来自动值会与
     * 硬编码的 28/29 撞车却查不出来。新命令一律追加在枚举末尾。 */
    GGCMD_LOAD_GG,              /* 已移除：加载 GG 核心（保留占位，勿复用） */
    GGCMD_CORE_VERSIONS,        /* 已移除：列出可加载的 GG 版本族（保留占位） */
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
    /* 握手鉴权（**新命令一律追加在它之后**，插到它前面会让 AUTH 及其后所有命令码整体后移，
     * 与已部署的另一端错位；下面 FREEZE_RESULTS 就是按这条规则追加的）：
     * 请求 {"p":{"key":"<hex>"}}；成功响应 {"r":{"authed":true}}，失败 GGERR_PERM。
     * 未携带有效密钥的连接只允许 PING / AUTH，其余命令一律 GGERR_PERM。
     * 目的是堵住「设备上任意本地 App 连 127.0.0.1:34127 即可获得 root 能力」的面。 */
    GGCMD_AUTH,
    /* 批量冻结当前结果集：{"p":{"type":<可选>, "value":<可选>, "limit":<可选>}}
     * - 不传 value 时按每条地址的**当前值**冻结（"锁住现状"语义），传了则全部冻结成该值；
     * - 冻结表容量 GG_FREEZE_MAX(256)，超出部分不冻结并在响应里回报 truncated；
     * - 出现短读的条目跳过并计入 skipped —— 冻结一个读不到的值等于凭空造值。
     * 追加在 AUTH 之后，保证既有命令码数值不变（见下方 AUTH 注释）。 */
    GGCMD_FREEZE_RESULTS,
    /* 结果集内过滤：{"p":{"min":<可选>,"max":<可选>,"exclude_nan":<可选>,"nonzero":<可选>}}
     * 按**当前值**原地压缩结果集，可用 GGCMD_SEARCH_REFINE(op=undo) 回退。
     * 与 GGCMD_SEARCH_REFINE 的分工：refine 比"变化方向"，这条比"绝对值区间"。
     * 搜索后候选上万条时用它砍到几十条 —— 否则 AI 只能分页看上百轮。 */
    GGCMD_FILTER_RESULTS,
    /* 签名 → 全局变量地址（一条命令走完 AoB 扫 + 反汇编 + ADRP/ADD 求址）：
     * {"p":{"pattern":"e1 ?? 40 b9 ...","back":<可选,默认32>,"count":<可选,默认8>,
     *       "deref":<可选,默认1>}}
     * 响应 {"r":{"hits":[{"addr":"0x..","refs":[{"insn":"0x..","target":"0x..",
     *       "is_load":true,"value":"0x.."}]}]}}
     * 存在理由：手工"扫描→反汇编→算地址"要来回 5 轮，AI 每次都得重新推理；
     * 固化成一条命令后一次拿到结果。（2026-10-23 UE4 定位实测的痛点） */
    GGCMD_AOB_RESOLVE,
    /* 截取屏幕指定区域并降采样成灰度：{"p":{"x":..,"y":..,"w":..,"h":..,"scale":..}}
     * 响应 {"r":{"x","y","w","h","sw","sh","scale","data":"<hex 灰度>"}}。
     * 为什么不是整屏 PNG：PNG 编码在设备上要 1.08s（单核压缩 1200x2608），raw 只要 0.18s
     * —— 慢的从来不是截图，是编码。所以走 raw 流 + 只回传 ROI 的灰度。
     * scale 会自动上调，保证 sw*sh <= GG_SCREEN_MAX_PIXELS，避免回传撑爆单帧（1MiB）。 */
    GGCMD_SCREENSHOT_ROI,
    GGCMD_MAX
};

/* 错误码 */
enum gg_err {
    GGERR_OK = 0,
    GGERR_BAD_REQUEST = 1,      /* 参数/负载错误 */
    GGERR_NO_DEVICE = 2,        /* 设备不可达（adb 断连） */
    GGERR_NO_SESSION = 3,       /* 未 attach 或会话已失效 */
    GGERR_PERM = 4,             /* 权限不足（非 root） */
    GGERR_VERSION_MISMATCH = 5, /* 未使用，保留占位：GG 核心注入链路已删除，
                                 * 不再存在"版本族不符"这个失败原因。数值不能回收 ——
                                 * 删掉会让后续错误码整体前移，与已部署的对端错位。 */
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
