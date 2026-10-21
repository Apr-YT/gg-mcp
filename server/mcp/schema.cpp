// schema.cpp — MCP 工具的 JSON Schema 定义（对外工具清单的唯一事实源）
#include "schema.h"

#include <iostream>

namespace gg {

static json prop(const std::string& type, const std::string& desc,
                 const std::string& extra = "") {
    json p{{"type", type}, {"description", desc}};
    if (!extra.empty()) {
        // extra 形如 "\"enum\":[\"a\",\"b\"]" 的 JSON 片段, 合并进属性
        try {
            json e = json::parse("{" + extra + "}");
            for (auto it = e.begin(); it != e.end(); ++it) p[it.key()] = it.value();
        } catch (...) {
            /* extra 是开发期硬编码的 JSON 片段，解析失败说明这里写错了。
             * 静默忽略会让 schema 丢掉 enum 之类的约束，AI 调用时看不出可选值，
             * 而且没有任何迹象 —— 开发期的笔误必须当场喊出来。 */
            std::cerr << "[schema] bad extra json (ignored): " << extra << "\n";
        }
    }
    return p;
}

static ToolDef tool(const std::string& name, const std::string& desc,
                    json props, json required) {
    ToolDef t;
    t.name = name;
    t.description = desc;
    t.input_schema = {{"type", "object"}, {"properties", props}, {"required", required}};
    return t;
}

static const std::vector<ToolDef>& build() {
    static std::vector<ToolDef> defs = {
        // ---- 进程管理 ----
        tool("gg_list_processes",
             "列出设备上所有进程（pid/名称/comm/uid），支持按名称过滤",
             json{{"filter", prop("string", "可选过滤关键字（匹配 name 或 comm）")}},
             json::array()),
        tool("gg_attach",
             "附加目标进程（pid 或包名/进程名），初始化内核通道与搜索会话；32 位进程不支持",
             json{
                 {"pid", prop("integer", "进程 pid（与 name 二选一）")},
                 {"name", prop("string", "进程名或包名（与 pid 二选一）")},
             },
             json::array()),
        tool("gg_detach",
             "分离当前目标进程，停止冻结并清空结果",
             json::object(), json::array()),
        tool("gg_target_info",
             "目标进程内存映射概况 + 模块列表（区域/权限/基址）",
             json::object(), json::array()),
        tool("gg_module_list",
             "目标进程已加载模块列表（路径+基址+大小）",
             json::object(), json::array()),

        // ---- 内存搜索 ----
        tool("gg_search",
             "已知值精确搜索。value 支持十进制/0x十六进制，float/double 支持小数；type=bytes 时 value 为 hex 字节序列（AoB 特征码，如 \"48 65 4C 4C\"）",
             json{
                 {"value", prop("string", "要搜索的值（如 \"100\"、\"0x64\"、\"3.14\"；type=bytes 时为 hex 序列如 \"48 65 4C 4C\"）")},
                 {"type", prop("string", "值类型", "\"enum\":[\"byte\",\"word\",\"dword\",\"qword\",\"float\",\"double\",\"bytes\"]")},
                 {"region", prop("string", "搜索区域", "\"enum\":[\"anonymous\",\"writable\",\"all\"]")},
             },
             json{"value"}),
        tool("gg_search_unknown_init",
             "模糊搜索初始化：未知初值，建立候选位图+快照（内存上限 256MB）",
             json{
                 {"type", prop("string", "元素类型（模糊粒度）", "\"enum\":[\"byte\",\"word\",\"dword\",\"qword\",\"float\",\"double\"]")},
                 {"region", prop("string", "搜索区域", "\"enum\":[\"anonymous\",\"writable\",\"all\"]")},
             },
             json::array()),
        tool("gg_search_refine",
             "模糊细化：按比较结果收缩候选（需先 gg_search_unknown_init）",
             json{
                 {"op", prop("string", "细化操作", "\"enum\":[\"increased\",\"decreased\",\"unchanged\",\"changed\"]")},
             },
             json{"op"}),
        tool("gg_search_range",
             "区间搜索：min <= 值 <= max（按类型解释）",
             json{
                 {"min", prop("number", "区间最小值")},
                 {"max", prop("number", "区间最大值")},
                 {"type", prop("string", "值类型", "\"enum\":[\"byte\",\"word\",\"dword\",\"qword\",\"float\",\"double\"]")},
                 {"region", prop("string", "搜索区域", "\"enum\":[\"anonymous\",\"writable\",\"all\"]")},
             },
             json{"min", "max"}),
        tool("gg_result_count",
             "当前搜索结果数量（含模糊候选数）",
             json::object(), json::array()),
        tool("gg_get_results",
             "分页获取搜索结果（地址+当前值），结果在设备端维护；值的类型自动取搜索结果集自身类型，无需传 type",
             json{
                 {"offset", prop("integer", "起始下标（默认 0）")},
                 {"limit", prop("integer", "返回条数（默认 100，上限 10000）")},
                 {"type", prop("string", "已废弃/忽略（agentd 按结果集自身类型读值）", "\"enum\":[\"byte\",\"word\",\"dword\",\"qword\",\"float\",\"double\"]")},
             },
             json::array()),
        tool("gg_clear_results",
             "清空当前搜索结果（含模糊状态）",
             json::object(), json::array()),

        // ---- 读写修改 ----
        tool("gg_read",
             "读取指定地址的值（按类型解释）",
             json{
                 {"addr", prop("string", "目标地址（0x 十六进制）")},
                 {"type", prop("string", "值类型", "\"enum\":[\"byte\",\"word\",\"dword\",\"qword\",\"float\",\"double\"]")},
                 {"force", prop("integer", "强制读取（已禁用）：is_force_read 会在本机驱动上崩内核并重启设备，传 1 直接返回错误；请保持 0")},
             },
             json{"addr"}),
        tool("gg_read_bytes",
             "读取指定地址的原始字节（hex）。save_to 指定时为落盘模式："
             "server 自动分段读取（agentd 单段 64KB 上限），原始二进制写入 export_dir 下的文件，"
             "返回 {saved,size,read_ok,segments,complete}，不占对话带宽（适合大段 dump）。"
             "读取失败的区间会被补 0 保长度对齐，此时 complete=false 并给出 missing 区间，不得当作完整数据使用",
             json{
                 {"addr", prop("string", "目标地址")},
                 {"len", prop("integer", "字节数（默认 64；hex 模式上限 65536，save_to 模式自动分段，受 max_save_bytes 约束）")},
                 {"force", prop("integer", "强制读取（已禁用）：is_force_read 会在本机驱动上崩内核并重启设备，传 1 直接返回错误；请保持 0")},
                 {"save_to", prop("string", "落盘相对路径（可选）：只能写到配置的 export_dir 内，拒绝绝对路径、盘符与 `..`；不再返回 hex")},
             },
             json{"addr"}),
        tool("gg_hex_dump",
             "读取指定地址的字节并以 hex 转储（同 gg_read_bytes）",
             json{
                 {"addr", prop("string", "目标地址")},
                 {"len", prop("integer", "字节数（默认 64）")},
                 {"force", prop("integer", "强制读取（已禁用）：is_force_read 会在本机驱动上崩内核并重启设备，传 1 直接返回错误；请保持 0")},
             },
             json{"addr"}),
        tool("gg_view_memory",
             "结构化查看内存：把 addr 起的一段内存按 element_type 自动解析为元素列表，"
             "AI 据此理解结构体布局。int32/float32 输出数值+原始位；pointer（arm64 8 字节）"
             "额外标注目标地址是否映射及所在模块，可继续用 gg_read / gg_view_memory 跟随指针。",
             json{
                 {"addr", prop("string", "起始地址（十六进制 0x...）")},
                 {"range", prop("integer", "元素个数（按 element_type 宽度逐个解析；默认 8，上限 512）")},
                 {"element_type", prop("string", "元素解释类型", "\"enum\":[\"int32\",\"float32\",\"pointer\"]")},
                 {"force", prop("integer", "强制读取（已禁用）：is_force_read 会在本机驱动上崩内核并重启设备，传 1 直接返回错误；请保持 0")},
             },
             json{"addr"}),
        tool("gg_write",
             "写入指定地址（整型支持 0x 前缀；float/double 支持小数）",
             json{
                 {"addr", prop("string", "目标地址")},
                 {"value", prop("string", "要写入的值")},
                 {"type", prop("string", "值类型", "\"enum\":[\"byte\",\"word\",\"dword\",\"qword\",\"float\",\"double\"]")},
             },
             json{"addr", "value"}),
        tool("gg_set_results",
             "批量修改搜索结果：values=[{index|addr, value}]",
             json{
                 {"values", prop("array", "批量条目：[{\"index\":0,\"value\":\"99999\"}] 或 [{\"addr\":\"0x...\",\"value\":\"...\"}]")},
                 {"type", prop("string", "值类型", "\"enum\":[\"byte\",\"word\",\"dword\",\"qword\",\"float\",\"double\"]")},
             },
             json{"values"}),
        tool("gg_freeze",
             "冻结地址：后台循环持续写入锁定值（detach 自动清理）",
             json{
                 {"addr", prop("string", "目标地址")},
                 {"value", prop("string", "锁定值")},
                 {"type", prop("string", "值类型", "\"enum\":[\"byte\",\"word\",\"dword\",\"qword\",\"float\",\"double\"]")},
             },
             json{"addr", "value"}),
        tool("gg_unfreeze",
             "解冻：不带 addr 则全部解冻",
             json{
                 {"addr", prop("string", "可选：解冻指定地址")},
             },
             json::array()),

        // ---- 分析定位 ----
        tool("gg_module_base",
             "按名称找模块基址（子串匹配）",
             json{
                 {"name", prop("string", "模块名关键字，如 \"libil2cpp.so\"")},
             },
             json{"name"}),
        tool("gg_calc_offset",
             "计算地址相对模块基址的偏移",
             json{
                 {"addr", prop("string", "目标地址")},
                 {"module", prop("string", "模块名关键字（缺省返回原地址）")},
             },
             json{"addr"}),
        tool("gg_pointer_scan",
             "指针扫描：在可写区找指向当前搜索结果地址的指针（dword/qword）",
             json{
                 {"type", prop("string", "指针宽度", "\"enum\":[\"dword\",\"qword\"]")},
                 {"cap", prop("integer", "输出上限（默认 5000）")},
             },
             json::array()),
        tool("gg_pointer_chain",
             "多级指针追踪：从 addr（指针容器）开始逐级解引用 8 字节指针，"
             "每级标注目标映射/模块/模块内偏移/是否落在区域起点（基址），"
             "直到空指针、未映射、环或达到 max_depth。配合 gg_pointer_scan 使用："
             "先找指向目标值的容器，再沿链追到基址，还原 base+offset 结构。",
             json{
                 {"addr", prop("string", "起始地址（此处存有 8 字节指针；十六进制 0x...）")},
                 {"max_depth", prop("integer", "最大追踪深度（默认 8，上限 32）")},
             },
             json{"addr"}),
        tool("gg_resolve_symbol",
             "地址静态解析：解析目标地址所属模块 / 模块加载基址 / 模块内偏移，"
             "并在 ELF .dynsym 中查找最近的导出符号，得到函数名（纯本地解析，不依赖外部工具）。",
             json{
                 {"addr", prop("string", "目标地址（十六进制 0x...）")},
             },
             json{"addr"}),
        tool("gg_pause",
             "暂停目标进程（SIGSTOP）",
             json::object(), json::array()),
        tool("gg_resume",
             "恢复目标进程（SIGCONT）",
             json::object(), json::array()),

        // ---- 内核与 GG 核心加载 ----
        tool("gg_kernel_status",
             "内核驱动通道状态：kernel-driver / 降级层 / 自检 / 统计",
             json::object(), json::array()),

        // ---- 硬件断点监视（CE 式"什么访问了这个地址"） ----
        tool("gg_watch_set",
             "对目标进程某地址设置硬件写监视点（rwproc33 hwbp 模块，CE 式「什么访问了这个地址」）。"
             "设置成功后可监视目标代码对该地址的访问，用 gg_watch_poll 取命中（含 pc 与寄存器），"
             "据此定位写者指令 → 反查权威变量。\n"
             "依赖自编译的 hwBreakpointProc 模块（scripts/build_hwbp_ko.sh 产出，"
             "由 deploy.ps1 自动部署）。agentd 首次调用时会探测模块命令码基准："
             "afei 那份 prebuilt 是 0 起始枚举且回调用 mutex，发 INST 会打崩内核，"
             "探测不通过则返回 DEGRADED 并说明原因。",
             json{
                 {"addr", prop("string", "要监视的地址（十六进制 0x...）")},
                 {"type", prop("integer", "断点类型：1=读 2=写(默认) 3=读写 4=执行")},
                 {"len", prop("integer", "监视长度（默认 4）")},
             },
             json{"addr"}),
        tool("gg_watch_poll",
             "取走硬件监视点命中列表，自动附带：模块归属（module/module_base/module_offset）、"
             "调用栈回溯（stack，FP 链最多 5 层）、预反汇编 pc±16 条（disasm）",
             json::object(), json::array()),
        tool("gg_watch_inject",
             "调试：手动构造一条命中（pc/fp/sp/lr）走完整 enrich 管道（模块/回溯/反汇编），"
             "用于在没有真实命中时校验日志与反汇编格式",
             json{
                 {"pc", prop("string", "命中 pc（十六进制）")},
                 {"fp", prop("string", "frame pointer x29（十六进制，回溯起点）")},
                 {"sp", prop("string", "sp")},
                 {"lr", prop("string", "link register x30")},
             },
             json::array()),
        tool("gg_watch_clear",
             "停止监视并 detach 目标全部线程",
             json::object(), json::array()),

        // ---- 反汇编（Capstone ARM64） ----
        tool("gg_disassemble",
             "反汇编目标进程内存（Capstone ARM64）。返回结构化指令列表："
             "地址/字节/助记符/操作数/注释。断点命中后对 pc 反汇编 ±32 条即可分析写者代码。",
             json{
                 {"addr", prop("string", "起始地址（十六进制 0x...）")},
                 {"length", prop("integer", "反汇编长度（字节，默认 64，上限 4096）")},
             },
             json{"addr"}),

        // ---- 系统 ----
        tool("gg_status",
             "会话状态：内核通道、冻结数、安全模式等",
             json::object(), json::array()),
        tool("gg_shell",
             "在设备上执行白名单 shell 命令（需配置 shell_enabled=true）",
             json{
                 {"cmd", prop("string", "命令（白名单：ps/ls/cat/dumpsys/getprop/id）")},
             },
             json{"cmd"}),
        tool("gg_device_info",
             "设备信息：架构/root/agentd 版本/驱动状态",
             json::object(), json::array()),

        // ---- AI 标注库（跨会话记忆） ----
        tool("gg_annotate_add",
             "保存/更新一条字段标注（AI 逆向发现字段后调用；按 tag 去重，持久化到本地 JSON）。"
             "推荐带 module+module_offset（进程重启地址会变，模块偏移可重定位）",
             json{
                 {"tag", prop("string", "字段名（唯一键，如 coins/hp/pos_x）")},
                 {"description", prop("string", "字段说明（如 金币数量）")},
                 {"address", prop("string", "发现时的绝对地址（十六进制 0x...）")},
                 {"module", prop("string", "所属模块（如 libgame.so）")},
                 {"module_offset", prop("string", "模块内偏移（0x...，可重定位）")},
                 {"type", prop("string", "值类型 byte/word/dword/qword/float/double")},
                 {"size", prop("integer", "字节数")},
                 {"frozen", prop("boolean", "是否冻结")},
                 {"confidence", prop("number", "置信度 0~1")},
                 {"notes", prop("string", "分析备注")},
             },
             json{"tag"}),
        tool("gg_annotate_list",
             "列出全部/指定字段标注",
             json{{"tag", prop("string", "按 tag 过滤（空=全部）")}},
             json::array()),
        tool("gg_annotate_get",
             "取单个标注",
             json{{"tag", prop("string", "字段名")}},
             json{"tag"}),
        tool("gg_annotate_remove",
             "删除标注",
             json{{"tag", prop("string", "字段名")}},
             json{"tag"}),
        tool("gg_annotate_context",
             "生成标注上下文纯文本（会话开始/分析前调用，注入到 AI 上下文）",
             json{{"tag", prop("string", "只注入该 tag（空=全部）")}},
             json::array()),

        // ---- GG-TOUCH 内核虚拟触摸 ----
        tool("gg_ghost_tap",
             "内核虚拟触摸注入：单点 tap（display 坐标，横屏 2608x1200 逻辑像素；"
             "经 rwproc33 驱动注册的 gg-touch 虚拟 input 设备注入，无新设备暴露在 InputManager 之外的标准管线）",
             json{
                 {"x", prop("integer", "display X（横屏 0-2607）")},
                 {"y", prop("integer", "display Y（横屏 0-1199）")},
                 {"hold_ms", prop("integer", "按下保持毫秒（默认 120）")},
             },
             json{"x", "y"}),
        tool("gg_ghost_swipe",
             "内核虚拟触摸注入：滑动（display 坐标，线性插值）",
             json{
                 {"x1", prop("integer", "起点 X")},
                 {"y1", prop("integer", "起点 Y")},
                 {"x2", prop("integer", "终点 X")},
                 {"y2", prop("integer", "终点 Y")},
                 {"ms", prop("integer", "滑动时长毫秒（默认 300）")},
             },
             json{"x1", "y1", "x2", "y2"}),
    };
    return defs;
}

const std::vector<ToolDef>& tool_defs() {
    return build();
}

}  // namespace gg
