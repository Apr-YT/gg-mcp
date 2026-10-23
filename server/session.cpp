// session.cpp — MCP 工具实现（薄代理 → gg-agentd）
// 工具数量不在这里写死：以 schema.cpp::build() 为准，由
// tests/unit/check_dispatch_consistency.py 交叉校验（写死数字必然漂移）。
//
// M1 表驱动重构（2026-08-18）：call_tool 的分发从 if (name == ...) 链
// 改为 kTools[] 表（{name, fn, write_op}）。参数映射逐行搬运，零行为变化：
//   - safe_mode 写拦截从硬编码名字集合改为表的 write_op 字段（单一数据源）
//   - gg_read_bytes 与 gg_hex_dump 合并分支拆为两个表项、共用同一 handler
//   - 分发一致性护栏（tests/unit/check_dispatch_consistency.py）已升级支持表驱动解析
#include "session.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "shared_ctx.h"

#ifdef _WIN32
#include <process.h>
#define GG_GETPID() _getpid()
#else
#include <unistd.h>
#define GG_GETPID() getpid()
#endif

#ifdef _WIN32
#include <direct.h>
#define GG_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define GG_MKDIR(p) ::mkdir((p), 0755)
#endif

namespace gg {

// save_to 的落盘路径收敛：只接受 export_dir 下的相对路径。
// 拒绝绝对路径、盘符、以及任何 `..` 段 —— 否则 MCP 工具调用方可以让服务端
// 覆盖任意可写文件（配置、密钥、其它工程源码）。
static bool resolve_save_path(const std::string& raw, const std::string& root, std::string* out) {
    if (raw.empty() || raw.size() > 512) return false;
    if (raw.find('\0') != std::string::npos) return false;
    if (raw[0] == '/' || raw[0] == '\\') return false;         // 绝对路径
    if (raw.size() >= 2 && raw[1] == ':') return false;        // 盘符
    if (raw.find(':') != std::string::npos) return false;      // 备用数据流 / 盘符变体

    std::vector<std::string> parts;
    std::string cur;
    for (char c : raw) {
        if (c == '/' || c == '\\') {
            if (cur.empty()) return false;                     // 连续分隔符 / 空段
            parts.push_back(cur);
            cur.clear();
        } else {
            if (static_cast<unsigned char>(c) < 0x20) return false;   // 控制字符
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return false;
    for (const auto& p : parts) {
        if (p == "." || p == "..") return false;               // 目录穿越
    }

    const std::string base = root.empty() ? std::string("exports") : root;
    // 根目录本身不允许越出（配置里如果写了绝对路径那就按绝对路径建根，但工具侧只能相对它）
    std::string full = base;
    if (!full.empty() && full.back() != '/' && full.back() != '\\') full += '/';
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) full += '/';
        full += parts[i];
    }
    *out = full;
    return true;
}

// 逐级创建目录（父目录部分，不含文件名）
static void ensure_parent_dirs(const std::string& path) {
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string sub = path.substr(0, pos);
        if (!sub.empty()) GG_MKDIR(sub.c_str());
    }
}

// 地址参数统一解析：接受 JSON 整数或数字字符串。
// 为什么必须统一：JSON number 是 double，超过 2^53 的地址（0x7fxxxxxxxxxx 的十进制写法）
// 会被序列化成科学计数法，agentd 侧 strtoull 解析失败后静默当 0 —— 表现为「读不到」而不是
// 「地址写错了」。统一转成 "0x..." 字符串下行可以彻底避免精度问题。
static bool parse_addr(const json& v, uint64_t* out) {
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        char* end = nullptr;
        errno = 0;
        /* strtoull 接受前导 '-' 并做模运算：'-1' → 0xFFFFFFFFFFFFFFFF，且不设 ERANGE。
         * 不拦的话，写错的负地址会被当成一个巨大的合法地址去读。 */
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i < s.size() && s[i] == '-') return false;
        unsigned long long a = strtoull(s.c_str(), &end, 0);
        if (end == s.c_str() || *end != '\0' || errno == ERANGE) return false;
        *out = static_cast<uint64_t>(a);
        return true;
    }
    if (v.is_number_unsigned()) { *out = v.get<uint64_t>(); return true; }
    if (v.is_number_integer()) {
        long long n = v.get<long long>();
        if (n < 0) return false;
        *out = static_cast<uint64_t>(n);
        return true;
    }
    return false;
}

// 长度参数统一解析：缺省用 def，显式值必须是 1..maxv 的整数。
// 旧实现对 len 不做任何检查：传字符串会抛 json 异常被吞成 -32603，
// 传 0 或负数则被静默改成默认值 —— 两者都让调用方看不出是自己参数错了。
static bool parse_len(const json& v, long long def, long long maxv, long long* out) {
    long long n = def;
    if (!v.is_null()) {
        if (v.is_number_integer()) {
            n = v.get<long long>();
        } else if (v.is_string()) {
            const std::string s = v.get<std::string>();
            char* end = nullptr;
            errno = 0;
            long long t = strtoll(s.c_str(), &end, 0);
            if (end == s.c_str() || *end != '\0' || errno == ERANGE) return false;
            n = t;
        } else {
            return false;
        }
    }
    if (n <= 0 || n > maxv) return false;
    *out = n;
    return true;
}

// force 参数：只接受 0/1（bool / 整数 / 数字字符串）。
// 旧写法 a["force"].get<int>() 在传 "1"/true/null 时会抛 json::type_error，
// 被协议层统一吞成 -32603 Internal error —— 明明是参数错，却报成服务端故障。
static bool parse_flag01(const json& v, int* out) {
    if (v.is_null()) { *out = 0; return true; }
    if (v.is_boolean()) { *out = v.get<bool>() ? 1 : 0; return true; }
    if (v.is_number_integer()) {
        long long n = v.get<long long>();
        if (n != 0 && n != 1) return false;
        *out = static_cast<int>(n);
        return true;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s == "0") { *out = 0; return true; }
        if (s == "1") { *out = 1; return true; }
        return false;
    }
    return false;
}

/* force 参数已永久禁用：rwproc33 的 is_force_read 在本机内核上对活跃页会 panic
 * （实测连续两次崩内核重启，见 AGENTS 第 27 条），agentd 侧一律拒绝。
 * schema 里已不再声明该参数，但旧客户端/直连脚本仍可能传 —— 传了必须明确报错，
 * 不能静默忽略：那会让调用方以为"强制读生效了"，实际拿到的是普通读的结果。
 * 返回非 null 即表示本次调用应直接把该错误返回出去。 */
static json force_guard(Session::CallCtx& c, const json& a) {
    int f = 0;
    if (!parse_flag01(a.contains("force") ? a["force"] : json(), &f))
        return c.fail(GGERR_BAD_REQUEST, "force must be 0 or 1");
    if (!f) return json();
    return c.fail(GGERR_BAD_REQUEST,
                  "force is permanently disabled: rwproc33 is_force_read panics this kernel "
                  "on live pages. Remove the force parameter (plain reads are unaffected).");
}

// 把 uint64 地址格式化成下行用的字符串
static std::string addr_to_wire(uint64_t addr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)addr);
    return std::string(buf);
}

// args["addr"] → params["addr"]（"0x..." 字符串）。
// 必须在所有带地址的工具上统一走这里：schema 声明 addr 是 string，但 AI 客户端
// 传 JSON 数字（{"addr":4096}）是常态。数字原样透传下去，agentd 侧按
// ->valuestring 取会拿到 NULL —— gg_watch_inject 曾因此 strtoull(NULL) 直接段错误，
// 其余工具则误报 "addr required"（明明用户传了地址）。
static bool put_addr_param(const json& args, json& params) {
    uint64_t addr = 0;
    if (!parse_addr(args["addr"], &addr)) return false;
    params["addr"] = addr_to_wire(addr);
    return true;
}


Session::Session(ServerConfig cfg)
    : cfg_(std::move(cfg)),
      /* ⚠ 这里必须读 cfg_ 而不是形参 cfg。
       * 成员按声明顺序初始化：cfg_ 先于 client_，而 cfg_ 的初始化已经把形参
       * cfg 整体 move 走 —— 此后 cfg.adb / cfg.serial / cfg.wire_key 全是空串，
       * 只有 POD 的 agent_port 因为 move 对 int 等同拷贝才幸存。
       * 旧实现读的就是形参，后果是 AgentClient 拿到的 adb 恒为 ""，
       * 直连失败后 adb forward 拼不出来（adb_safe("") 直接拒绝），
       * 只能依赖部署脚本预先建好的 forward —— 这也是「重连有时不生效」的根因。 */
      client_([this] {
          AgentClient::Config ac;
          ac.adb = cfg_.adb;
          ac.serial = cfg_.serial;
          ac.agent_port = cfg_.agent_port;
          ac.wire_key = cfg_.wire_key;
          return ac;
      }()),
      store_(cfg_.annotations_file.empty() ? "annotations.json" : cfg_.annotations_file) {}

json Session::ok(const json& data) {
    json result;
    result["content"] = json::array();
    result["content"].push_back({{"type", "text"}, {"text", data.dump(2)}});
    result["isError"] = false;
    return result;
}

json Session::fail(int code, const std::string& msg) {
    json result;
    result["content"] = json::array();
    json err = {{"error", {{"code", code}, {"message", msg}}}};
    result["content"].push_back({{"type", "text"}, {"text", err.dump(2)}});
    result["isError"] = true;
    return result;
}

int Session::device_call(uint16_t cmd, const json& req, json* out) {
    int rc = client_.call(cmd, req, out);
    // 只有「连接都没建立起来」才是干净失败：请求一个字节都没发出去，重放不会
    // 改变任何远端状态。发送失败 / 响应超时 / 读中断 都可能意味着远端已经执行过，
    // 盲目重放会让 gg_write、gg_set_results、gg_freeze 这类有副作用的命令跑两次。
    if (rc != 0 && client_.last_outcome == CallOutcome::ConnectFailed) {
        client_.close();
        rc = client_.call(cmd, req, out);
    }
    return rc;
}

// 返回值约定：>=1 为合法枚举；-1 表示「显式传入但非法」。
// 旧实现把未知值一律回落成 DWORD/ANONYMOUS/CHANGED，等于把「type 拼错了」
// 静默变成「换个类型再搜一次」，调用方拿到的是另一个操作的结果。
int Session::norm_type(const json& t) {
    if (t.is_null()) return VT_DWORD;   // 未提供 → 默认 dword（保持兼容）
    if (t.is_string()) {
        const std::string s = t.get<std::string>();
        if (s == "byte" || s == "u8") return VT_BYTE;
        if (s == "word" || s == "u16") return VT_WORD;
        if (s == "dword" || s == "u32" || s == "int") return VT_DWORD;
        if (s == "qword" || s == "u64" || s == "long") return VT_QWORD;
        if (s == "float" || s == "f32") return VT_FLOAT;
        if (s == "double" || s == "f64") return VT_DOUBLE;
        if (s == "bytes" || s == "aob") return VT_BYTES;
        return -1;
    }
    if (t.is_number_integer()) {
        int v = t.get<int>();
        if (v >= VT_BYTE && v <= VT_BYTES) return v;
        return -1;
    }
    return -1;
}

int Session::norm_region(const json& r) {
    if (r.is_null()) return REGION_ANONYMOUS;
    if (r.is_string()) {
        const std::string s = r.get<std::string>();
        if (s == "anonymous" || s == "anon") return REGION_ANONYMOUS;
        if (s == "writable") return REGION_WRITABLE;
        if (s == "all") return REGION_ALL;
        return -1;
    }
    if (r.is_number_integer()) {
        int v = r.get<int>();
        if (v >= REGION_ANONYMOUS && v <= REGION_ALL) return v;
        return -1;
    }
    return -1;
}

int Session::norm_op(const json& o) {
    if (o.is_null()) return REFINE_CHANGED;
    if (o.is_string()) {
        const std::string s = o.get<std::string>();
        if (s == "increased" || s == "inc") return REFINE_INCREASED;
        if (s == "decreased" || s == "dec") return REFINE_DECREASED;
        if (s == "unchanged" || s == "same") return REFINE_UNCHANGED;
        if (s == "changed" || s == "diff") return REFINE_CHANGED;
        return -1;
    }
    if (o.is_number_integer()) {
        int v = o.get<int>();
        if (v >= REFINE_INCREASED && v <= REFINE_CHANGED) return v;
        return -1;
    }
    return -1;
}

// 会话概览。仅在 call_tool 的会话锁内调用（它读 client_.last_error，
// 而该字段由 AgentClient::call 在另一把锁下写）。
json Session::local_status() {
    json out;
    json ks;
    if (device_call(GGCMD_KERNEL_STATUS, json::object(), &ks) == 0) {
        out["kernel"] = ks;
    } else {
        out["kernel"] = {{"state_str", "unavailable"}, {"error", client_.last_error}};
    }
    return out;
}

// CallCtx::send —— 请求 agentd 并包装结果（嵌套类可访问 Session private）
json Session::CallCtx::send(uint16_t cmd) {
    json out;
    int rc = s->device_call(cmd, params, &out);
    if (rc != 0) return s->fail(rc, s->client_.last_error);
    return s->ok(out);
}

// ==================== 表驱动 handlers（匿名 namespace） ====================
namespace {

typedef json (*tool_fn)(Session::CallCtx& c, const json& args);

struct ToolEntry {
    const char* name;
    tool_fn fn;
    bool write_op;          // true = safe_mode 下本地拒绝（与 agentd 双保险）
};

// ---- 进程/会话 ----
json tool_list_processes(Session::CallCtx& c, const json& a) {
    if (a.contains("filter")) c.params["filter"] = a["filter"];
    return c.send(GGCMD_LIST_PROCESSES);
}
json tool_attach(Session::CallCtx& c, const json& a) {
    if (a.contains("pid")) c.params["pid"] = a["pid"];
    if (a.contains("name")) c.params["name"] = a["name"];
    return c.send(GGCMD_ATTACH);
}
json tool_detach(Session::CallCtx& c, const json&) { return c.send(GGCMD_DETACH); }
json tool_target_info(Session::CallCtx& c, const json&) { return c.send(GGCMD_TARGET_INFO); }
json tool_module_list(Session::CallCtx& c, const json&) { return c.send(GGCMD_MODULE_LIST); }

// ---- 搜索 ----
json tool_search(Session::CallCtx& c, const json& a) {
    if (!a.contains("value")) return c.fail(GGERR_BAD_REQUEST, "value required");
    if (!c.put_type(a.value("type", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid type");
    c.params["value"] = a["value"];
    if (!c.put_region(a.value("region", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid region");
    return c.send(GGCMD_SEARCH);
}
json tool_search_unknown_init(Session::CallCtx& c, const json& a) {
    if (!c.put_type(a.value("type", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid type");
    if (!c.put_region(a.value("region", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid region");
    return c.send(GGCMD_SEARCH_UNKNOWN_INIT);
}
json tool_search_refine(Session::CallCtx& c, const json& a) {
    if (!c.put_op(a.value("op", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid op");
    return c.send(GGCMD_SEARCH_REFINE);
}
json tool_search_undo(Session::CallCtx& c, const json& a) {
    (void)a;
    /* wire 层复用 SEARCH_REFINE + op="undo"：不新增命令码，协议 ABI 不变。 */
    c.params["op"] = "undo";
    return c.send(GGCMD_SEARCH_REFINE);
}
json tool_filter_results(Session::CallCtx& c, const json& a) {
    /* 一个条件都不给等于"原样重读全量候选"：设备端会拒绝，这里提前挡掉省一次往返。 */
    if (!a.contains("min") && !a.contains("max") &&
        !a.contains("exclude_nan") && !a.contains("nonzero"))
        return c.fail(GGERR_BAD_REQUEST, "at least one of min/max/exclude_nan/nonzero required");
    if (a.contains("min")) c.params["min"] = a["min"];
    if (a.contains("max")) c.params["max"] = a["max"];
    if (a.contains("exclude_nan")) c.params["exclude_nan"] = a["exclude_nan"];
    if (a.contains("nonzero")) c.params["nonzero"] = a["nonzero"];
    return c.send(GGCMD_FILTER_RESULTS);
}

/* 屏幕差分（gg_screen_diff）。
 *
 * 分工：视觉只回答「这块画面变了没」，不读数字 —— OCR 认错一个数字（1234→1235）
 * 会让整轮搜索报废。精确值一律走内存（gg_read / search_refine）。
 *
 * 基线存在 PC 侧（screen_frames_），设备端每次只回传降采样灰度，不带历史状态：
 * 这样 agentd 重启后差分仍然可用，也不会因为多客户端而互相踩基线。
 */
json tool_screen_diff(Session::CallCtx& c, const json& a) {
    for (const char* k : {"x", "y", "w", "h"}) {
        if (!a.contains(k) || !a[k].is_number())
            return c.fail(GGERR_BAD_REQUEST, std::string(k) + " must be a number");
    }
    json req = {{"x", a["x"]}, {"y", a["y"]}, {"w", a["w"]}, {"h", a["h"]}};
    if (a.contains("scale")) req["scale"] = a["scale"];
    json shot;
    int rc = c.device_call(GGCMD_SCREENSHOT_ROI, req, &shot);
    if (rc != 0) return c.fail(rc, c.last_error());
    if (!shot.is_object() || !shot.contains("data") || !shot["data"].is_string())
        return c.fail(GGERR_INTERNAL, "malformed screenshot response");

    const std::string hexs = shot["data"].get<std::string>();
    std::vector<unsigned char> cur;
    cur.reserve(hexs.size() / 2);
    for (size_t i = 0; i + 1 < hexs.size(); i += 2) {
        auto nib = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        int hi = nib(hexs[i]);
        int lo = nib(hexs[i + 1]);
        if (hi < 0 || lo < 0) return c.fail(GGERR_INTERNAL, "bad hex from device");
        cur.push_back((unsigned char)((hi << 4) | lo));
    }

    int threshold = (a.contains("threshold") && a["threshold"].is_number())
                        ? a["threshold"].get<int>() : 16;
    double min_ratio = (a.contains("min_ratio") && a["min_ratio"].is_number())
                           ? a["min_ratio"].get<double>() : 0.02;
    bool reset = a.contains("reset") && a["reset"].is_boolean() && a["reset"].get<bool>();
    std::string key = std::to_string(shot.value("x", 0)) + ":" + std::to_string(shot.value("y", 0)) +
                      ":" + std::to_string(shot.value("w", 0)) + ":" + std::to_string(shot.value("h", 0));

    std::lock_guard<std::mutex> lk(c.screen_mtx());
    auto it = c.screen_frames().find(key);
    /* 尺寸变化（设备自动抬了 scale）就重建基线：拿不同尺寸的两帧逐像素比毫无意义 */
    bool baseline = reset || it == c.screen_frames().end() || it->second.size() != cur.size();
    json r;
    if (baseline) {
        r["baseline"] = true;
        r["note"] = "首帧已建立基线。等画面上的数值真的变化后再调一次";
        c.screen_frames()[key] = std::move(cur);
    } else {
        const std::vector<unsigned char>& prev = it->second;
        size_t diff_px = 0;
        int maxd = 0;
        for (size_t i = 0; i < cur.size(); i++) {
            int d = std::abs((int)cur[i] - (int)prev[i]);
            if (d >= threshold) diff_px++;
            if (d > maxd) maxd = d;
        }
        double ratio = cur.empty() ? 0.0 : (double)diff_px / (double)cur.size();
        r["changed"] = ratio >= min_ratio;
        r["ratio"] = ratio;
        r["max_delta"] = maxd;
        r["changed_pixels"] = diff_px;
        it->second = std::move(cur);   /* 滚动基线：下一次仍与最近一帧比 */
    }
    r["sw"] = shot.value("sw", 0);
    r["sh"] = shot.value("sh", 0);
    r["scale"] = shot.value("scale", 1);
    return c.ok(r);
}

/* 等画面变化 → 自动 refine（gg_screen_watch_refine）。
 *
 * 把「人盯着屏幕等数字变」这件事替 AI 做完：轮询 ROI，检测到变化就立刻对当前
 * 结果集做一次 refine。省掉的不是截图时间，是 AI 自己"截图→比对→再截图"的两三轮推理。
 *
 * 为什么同步阻塞在 handler 里：这本来就是"等一件事发生"，异步化只会让 AI 多一次
 * 轮询问"好了吗"。所以 timeout_ms 有硬上限。
 *
 * 上限为什么是 10s 而不是 60s：call_tool 全程持会话锁（结果集是设备端全局单例，
 * 并发工具调用会互相踩），阻塞期间其他所有工具调用都在排队。60s 的上限意味着
 * 一次误用能把整个服务冻住一分钟。10s 足够覆盖"点一下等数字变"，再长请改用
 * gg_screen_diff + 手动 refine，把等待切回给调用方。
 *
 * settle_ms 存在的理由：数值滚动动画（金币从 100 涨到 150 会逐帧跳）期间 refine，
 * 会按中间值判定。默认等 100ms 再 refine；目标用逐帧插值时调大。
 */
json tool_screen_watch_refine(Session::CallCtx& c, const json& a) {
    for (const char* k : {"x", "y", "w", "h"}) {
        if (!a.contains(k) || !a[k].is_number())
            return c.fail(GGERR_BAD_REQUEST, std::string(k) + " must be a number");
    }
    if (!c.put_op(a.value("op", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid op");

    auto clamp_i = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    int timeout_ms = clamp_i((a.contains("timeout_ms") && a["timeout_ms"].is_number())
                                 ? a["timeout_ms"].get<int>() : 5000, 200, 10000);
    int poll_ms = clamp_i((a.contains("poll_ms") && a["poll_ms"].is_number())
                              ? a["poll_ms"].get<int>() : 250, 100, 5000);
    int settle_ms = clamp_i((a.contains("settle_ms") && a["settle_ms"].is_number())
                                ? a["settle_ms"].get<int>() : 100, 0, 5000);
    int threshold = (a.contains("threshold") && a["threshold"].is_number())
                        ? a["threshold"].get<int>() : 16;
    double min_ratio = (a.contains("min_ratio") && a["min_ratio"].is_number())
                           ? a["min_ratio"].get<double>() : 0.02;
    bool reset = a.contains("reset") && a["reset"].is_boolean() && a["reset"].get<bool>();

    json req = {{"x", a["x"]}, {"y", a["y"]}, {"w", a["w"]}, {"h", a["h"]}};
    if (a.contains("scale")) req["scale"] = a["scale"];
    const std::string key = std::to_string(req["x"].get<double>()) + ":" +
                            std::to_string(req["y"].get<double>()) + ":" +
                            std::to_string(req["w"].get<double>()) + ":" +
                            std::to_string(req["h"].get<double>());

    auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const long long t_start = now_ms();
    const long long t_deadline = t_start + timeout_ms;
    int polls = 0;
    double max_ratio = 0.0;

    while (true) {
        json shot;
        int rc = c.device_call(GGCMD_SCREENSHOT_ROI, req, &shot);
        if (rc != 0) return c.fail(rc, c.last_error());
        if (!shot.is_object() || !shot.contains("data") || !shot["data"].is_string())
            return c.fail(GGERR_INTERNAL, "malformed screenshot response");
        const std::string hexs = shot["data"].get<std::string>();
        std::vector<unsigned char> cur;
        cur.reserve(hexs.size() / 2);
        for (size_t i = 0; i + 1 < hexs.size(); i += 2) {
            auto nib = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            int hi = nib(hexs[i]);
            int lo = nib(hexs[i + 1]);
            if (hi < 0 || lo < 0) return c.fail(GGERR_INTERNAL, "bad hex from device");
            cur.push_back((unsigned char)((hi << 4) | lo));
        }
        polls++;

        bool baseline = false;
        size_t diff_px = 0;
        int maxd = 0;
        double ratio = 0.0;
        {
            std::lock_guard<std::mutex> lk(c.screen_mtx());
            auto& frames = c.screen_frames();
            auto it = frames.find(key);
            if (reset || it == frames.end() || it->second.size() != cur.size()) {
                /* reset 只作用于第一帧，避免每轮都重建基线导致永远检测不到变化 */
                reset = false;
                frames[key] = cur;
                baseline = true;
            } else {
                const std::vector<unsigned char>& prev = it->second;
                for (size_t i = 0; i < cur.size(); i++) {
                    int d = std::abs((int)cur[i] - (int)prev[i]);
                    if (d >= threshold) diff_px++;
                    if (d > maxd) maxd = d;
                }
                ratio = cur.empty() ? 0.0 : (double)diff_px / (double)cur.size();
                frames[key] = cur;   /* 滚动基线 */
            }
        }
        if (baseline) {
            /* 首帧只建基线，不计入轮询次数 */
            polls--;
        } else if (ratio >= min_ratio) {
            if (ratio > max_ratio) max_ratio = ratio;
            if (settle_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
            json ref;
            int rrc = c.device_call(GGCMD_SEARCH_REFINE, c.params, &ref);
            json r;
            r["triggered"] = true;
            r["waited_ms"] = (double)(now_ms() - t_start);
            r["polls"] = polls;
            r["ratio"] = ratio;
            r["max_delta"] = maxd;
            if (rrc == 0) {
                r["refine"] = ref;
            } else {
                /* 视觉触发成功但 refine 失败（常见：还没搜过）——
                 * 分开报，不要让调用方以为是"画面没变" */
                r["refine_error"] = c.last_error();
            }
            return c.ok(r);
        } else if (ratio > max_ratio) {
            max_ratio = ratio;
        }

        if (now_ms() >= t_deadline) {
            json r;
            r["triggered"] = false;
            r["timeout"] = true;
            r["waited_ms"] = (double)(now_ms() - t_start);
            r["polls"] = polls;
            r["max_ratio"] = max_ratio;
            r["note"] = "画面在超时内没有超过阈值的变化；确认 ROI 框住了会变的那块，"
                        "或调低 min_ratio / 调大 timeout_ms";
            return c.ok(r);
        }
        /* 用剩余时间做 sleep，避免最后一轮跨过 deadline */
        long long left = t_deadline - now_ms();
        long long sleep_for = poll_ms < left ? poll_ms : left;
        if (sleep_for > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_for));
    }
}
json tool_search_range(Session::CallCtx& c, const json& a) {
    if (!a.contains("min") || !a.contains("max"))
        return c.fail(GGERR_BAD_REQUEST, "min and max required");
    if (!c.put_type(a.value("type", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid type");
    c.params["min"] = a["min"];
    c.params["max"] = a["max"];
    if (!c.put_region(a.value("region", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid region");
    return c.send(GGCMD_SEARCH_RANGE);
}
json tool_result_count(Session::CallCtx& c, const json& a) {
    /* summary 只在明确要时才下发：设备端要为此读一遍全量候选（3 万条约 20ms）。 */
    if (a.contains("summary")) c.params["summary"] = a["summary"];
    return c.send(GGCMD_RESULT_COUNT);
}
json tool_get_results(Session::CallCtx& c, const json& a) {
    c.params["offset"] = a.value("offset", 0);
    c.params["limit"] = a.value("limit", 100);
    /* type 由 agentd 按结果集自身类型（exact_vt/fuzzy_vt）决定，不再传递，
       避免调用方 type 误导读值字节数（2026-08-18 CF 实测修复） */
    return c.send(GGCMD_GET_RESULTS);
}
json tool_clear_results(Session::CallCtx& c, const json&) { return c.send(GGCMD_CLEAR_RESULTS); }

// ---- 内存读/写 ----
json tool_read(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    uint64_t addr = 0;
    if (!parse_addr(a["addr"], &addr))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    c.params["addr"] = addr_to_wire(addr);
    if (!c.put_type(a.value("type", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid type");
    if (json g = force_guard(c, a); !g.is_null()) return g;
    return c.send(GGCMD_READ);
}
json tool_read_bytes(Session::CallCtx& c, const json& a) {   // gg_read_bytes / gg_hex_dump
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    // 落盘模式（save_to）：server 自动分段循环读（agentd 单段上限 65536），
    // 原始二进制写本地文件，只返回元信息，不占对话带宽。
    // 三条硬约束：
    //   1. 路径只能落在 cfg.export_dir 内（拒绝绝对路径 / 盘符 / `..`）；
    //   2. len 与 addr 必须显式校验，解析失败不许静默变成默认值；
    //   3. 分段失败补 0 保长度对齐，但结果必须带 complete=false 与缺失区间，
    //      不能像旧实现那样一律报「成功 + 请求长度」。
    if (a.contains("save_to")) {
        if (!a["save_to"].is_string())
            return c.fail(GGERR_BAD_REQUEST, "save_to must be a string");

        std::string path;
        if (!resolve_save_path(a["save_to"].get<std::string>(), c.cfg().export_dir, &path))
            return c.fail(GGERR_BAD_REQUEST,
                          "save_to rejected: relative path inside export_dir only");

        // ---- len ----
        long long len = 64;
        if (a.contains("len")) {
            const json& lj = a["len"];
            if (lj.is_number_integer()) {
                len = lj.get<long long>();
            } else if (lj.is_string()) {
                const std::string ls = lj.get<std::string>();
                char* end = nullptr;
                errno = 0;
                long long v = strtoll(ls.c_str(), &end, 0);
                len = (end == ls.c_str() || *end != '\0' || errno == ERANGE) ? -1 : v;
            } else {
                len = -1;
            }
        }
        if (len <= 0) return c.fail(GGERR_BAD_REQUEST, "len must be a positive integer");
        if (static_cast<unsigned long long>(len) > c.cfg().max_save_bytes)
            return c.fail(GGERR_BAD_REQUEST, "len exceeds max_save_bytes limit");

        // ---- addr ----
        uint64_t addr = 0;
        if (a["addr"].is_string()) {
            const std::string as = a["addr"].get<std::string>();
            char* end = nullptr;
            errno = 0;
            addr = strtoull(as.c_str(), &end, 0);
            if (end == as.c_str() || *end != '\0' || errno == ERANGE)
                return c.fail(GGERR_BAD_REQUEST, "addr is not a valid address");
        } else if (a["addr"].is_number_unsigned()) {
            addr = a["addr"].get<uint64_t>();
        } else if (a["addr"].is_number_integer()) {
            long long v = a["addr"].get<long long>();
            if (v < 0) return c.fail(GGERR_BAD_REQUEST, "addr must not be negative");
            addr = static_cast<uint64_t>(v);
        } else {
            return c.fail(GGERR_BAD_REQUEST, "addr must be a number or numeric string");
        }
        if (addr > UINT64_MAX - static_cast<uint64_t>(len))
            return c.fail(GGERR_BAD_REQUEST, "addr + len overflows the address space");

        if (json g = force_guard(c, a); !g.is_null()) return g;

        // 先写 .part，全部段成功才改名 —— 避免中途失败把同名旧文件截断成半截数据。
        ensure_parent_dirs(path);
        const std::string tmp_path = path + ".part";
        FILE* f = fopen(tmp_path.c_str(), "wb");
        if (!f) return c.fail(GGERR_BAD_REQUEST, "cannot open save_to: " + tmp_path);

        /* 每段 256KB：等于 agentd 的 GG_SEARCH_CHUNK，也是驱动单次读上限
         * （RWPROC_MAX_IO）。此前写的是 65536，注释却声称"与 agentd 一致"，
         * 实际把 dump 速度白砍到 1/4 —— 设备端一直能收 256KB。 */
        const long long SEG = 256 * 1024;
        unsigned long long file_len = 0, read_ok = 0;
        int segs = 0;
        int first_err = 0;
        json missing = json::array();

        for (long long done = 0; done < len; done += SEG) {
            long long chunk = (len - done < SEG) ? (len - done) : SEG;
            char addr_buf[32];
            snprintf(addr_buf, sizeof(addr_buf), "0x%llx",
                     (unsigned long long)(addr + (uint64_t)done));
            json params;
            params["addr"] = addr_buf;
            params["len"] = chunk;
            json out;
            int rc = c.device_call(GGCMD_READ_BYTES, params, &out);
            std::string hex;
            if (rc == 0 && out.contains("hex") && out["hex"].is_string())
                hex = out["hex"].get<std::string>();
            size_t n = hex.size() / 2;
            if (n > (size_t)chunk) n = (size_t)chunk;   // 防御远端返回超长
            if (n < (size_t)chunk) {
                if (!first_err && rc != 0) first_err = rc;
                missing.push_back({{"offset", (unsigned long long)done},
                                   {"len", (long long)((size_t)chunk - n)}});
            }
            for (size_t i = 0; i < n; i++) {
                unsigned int b;
                if (sscanf(hex.c_str() + i * 2, "%2x", &b) != 1) b = 0;
                fputc((int)b, f);
            }
            for (size_t i = n; i < (size_t)chunk; i++) fputc(0, f);   // 缺失段补 0
            file_len += (unsigned long long)chunk;
            read_ok += n;
            segs++;
        }

        // 注意不能写 (fflush(f)==0 && fclose(f)==0)：短路会让 fclose 漏调用
        bool write_ok = (fflush(f) == 0);
        if (fclose(f) != 0) write_ok = false;

        bool promoted = false;
        if (write_ok && missing.empty()) {
            /* 先按 POSIX 语义直接 rename（原子覆盖）。
             * 失败再走 Windows 的 .bak 中转 —— 不能像旧实现那样先 std::remove(path)：
             * 一旦 rename 失败，唯一的一份旧数据已经没了，与"写 .part 保护旧文件"的
             * 初衷正好相反。 */
            promoted = (std::rename(tmp_path.c_str(), path.c_str()) == 0);
            if (!promoted) {
                const std::string bak_path = path + ".bak";
                std::remove(bak_path.c_str());
                bool had_old = (std::rename(path.c_str(), bak_path.c_str()) == 0);
                if (had_old) {
                    promoted = (std::rename(tmp_path.c_str(), path.c_str()) == 0);
                    if (promoted) std::remove(bak_path.c_str());
                    else std::rename(bak_path.c_str(), path.c_str());   // 回滚旧文件
                }
            }
            if (!promoted) write_ok = false;
        }

        json r{{"saved", promoted ? path : tmp_path},
               {"size", file_len},
               {"read_ok", read_ok},
               {"segments", segs},
               {"complete", missing.empty() && write_ok}};
        if (!missing.empty()) {
            r["missing"] = missing;
            r["missing_bytes"] = (unsigned long long)(file_len - read_ok);
            if (first_err) r["first_error"] = first_err;
            r["warning"] = "partial read: failed ranges were zero-filled; data is NOT complete";
        }
        if (!write_ok)
            r["warning"] = "local write did not finish cleanly (flush/close/rename failed)";
        return c.ok(r);
    }
    /* 非落盘模式（hex 返回）：addr/len 同样要显式校验 ——
     * 旧实现直接把 a["addr"] 塞进 params，大地址经 JSON double 会丢精度，
     * len 传字符串则抛异常被吞成 -32603。
     * 上限 65536 是**有意收紧**（设备端能收 256KB）：hex 是一字节两字符，
     * 256KB 会变成 512KB 文本直接灌进对话上下文。要 dump 大段请用 save_to。 */
    uint64_t addr = 0;
    if (!parse_addr(a["addr"], &addr))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    long long len = 64;
    if (!parse_len(a.contains("len") ? a["len"] : json(), 64, 65536, &len))
        return c.fail(GGERR_BAD_REQUEST, "len must be an integer in 1..65536");
    c.params["addr"] = addr_to_wire(addr);
    c.params["len"] = len;
    if (json g = force_guard(c, a); !g.is_null()) return g;
    return c.send(GGCMD_READ_BYTES);
}
json tool_view_memory(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    if (!put_addr_param(a, c.params))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    long long rng = 8;
    if (a.contains("range")) {
        const json& rj = a["range"];
        if (rj.is_number()) rng = rj.get<long long>();
        else if (rj.is_string()) rng = strtoll(rj.get<std::string>().c_str(), nullptr, 0);
    }
    c.params["range"] = rng;
    if (a.contains("element_type")) c.params["element_type"] = a["element_type"];
    if (json g = force_guard(c, a); !g.is_null()) return g;
    return c.send(GGCMD_VIEW_MEMORY);
}
json tool_write(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr") || !a.contains("value"))
        return c.fail(GGERR_BAD_REQUEST, "addr and value required");
    uint64_t addr = 0;
    if (!parse_addr(a["addr"], &addr))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    c.params["addr"] = addr_to_wire(addr);
    c.params["value"] = a["value"];
    if (!c.put_type(a.value("type", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid type");
    return c.send(GGCMD_WRITE);
}
json tool_set_results(Session::CallCtx& c, const json& a) {
    /* all=true：整个结果集改成同一值，设备端遍历（10000 条不必过线）。
     * 这条路径**不填 type**：norm_type(空) 默认是 dword，会把 qword 结果集写成半个值；
     * 缺省交给设备端按结果集自身类型处理。 */
    bool all = false;
    if (a.contains("all")) {
        if (a["all"].is_boolean()) all = a["all"].get<bool>();
        else if (a["all"].is_number()) all = a["all"].get<double>() != 0;
    }
    if (all) {
        if (!a.contains("value")) return c.fail(GGERR_BAD_REQUEST, "value required when all=true");
        c.params["all"] = true;
        c.params["value"] = a["value"];
    } else {
        if (!a.contains("values")) return c.fail(GGERR_BAD_REQUEST, "values required");
        c.params["values"] = a["values"];
    }
    if (a.contains("type") && !c.put_type(a["type"]))
        return c.fail(GGERR_BAD_REQUEST, "invalid type");
    return c.send(GGCMD_SET_RESULTS);
}
json tool_freeze(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr") || !a.contains("value"))
        return c.fail(GGERR_BAD_REQUEST, "addr and value required");
    if (!put_addr_param(a, c.params))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    c.params["value"] = a["value"];
    if (!c.put_type(a.value("type", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid type");
    return c.send(GGCMD_FREEZE);
}
json tool_unfreeze(Session::CallCtx& c, const json& a) {
    if (a.contains("addr") && !put_addr_param(a, c.params))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    return c.send(GGCMD_UNFREEZE);
}
json tool_freeze_results(Session::CallCtx& c, const json& a) {
    /* value 省略 = 锁住每条当前值（设备端读回再冻），不在这里补默认值。 */
    if (a.contains("value")) c.params["value"] = a["value"];
    if (a.contains("type") && !c.put_type(a["type"]))
        return c.fail(GGERR_BAD_REQUEST, "invalid type");
    if (a.contains("limit")) {
        if (!a["limit"].is_number() || a["limit"].get<double>() < 1)
            return c.fail(GGERR_BAD_REQUEST, "limit must be a positive integer");
        c.params["limit"] = a["limit"];
    }
    return c.send(GGCMD_FREEZE_RESULTS);
}

// ---- 模块/符号 ----
json tool_module_base(Session::CallCtx& c, const json& a) {
    if (!a.contains("name")) return c.fail(GGERR_BAD_REQUEST, "name required");
    c.params["name"] = a["name"];
    return c.send(GGCMD_MODULE_BASE);
}
json tool_calc_offset(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    if (!put_addr_param(a, c.params))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    if (a.contains("module")) c.params["module"] = a["module"];
    return c.send(GGCMD_CALC_OFFSET);
}
json tool_aob_resolve(Session::CallCtx& c, const json& a) {
    if (!a.contains("pattern"))
        return c.fail(GGERR_BAD_REQUEST, "pattern required");
    c.params["pattern"] = a["pattern"];
    if (a.contains("back")) c.params["back"] = a["back"];
    if (a.contains("count")) c.params["count"] = a["count"];
    if (a.contains("deref")) c.params["deref"] = a["deref"];
    return c.send(GGCMD_AOB_RESOLVE);
}
json tool_pointer_scan(Session::CallCtx& c, const json& a) {
    if (!c.put_type(a.value("type", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid type");
    c.params["cap"] = a.value("cap", 5000);
    /* addr：单目标模式（不传则扫当前结果集的每个地址） */
    if (a.contains("addr")) {
        if (!put_addr_param(a, c.params))
            return c.fail(GGERR_BAD_REQUEST,
                          "addr must be a non-negative integer or hex string");
    }
    /* lookback / aligned 只在显式给出时才下发，保持与旧版 agentd 的兼容
     * （旧 agentd 不认识这两个键也无害，它会忽略）。 */
    if (a.contains("lookback")) {
        const json& jl = a["lookback"];
        if (!jl.is_number() || jl.get<double>() < 0)
            return c.fail(GGERR_BAD_REQUEST, "lookback must be a non-negative number");
        c.params["lookback"] = jl;
    }
    if (a.contains("aligned")) {
        const json& ja = a["aligned"];
        int flag;
        if (ja.is_boolean()) flag = ja.get<bool>() ? 1 : 0;
        else if (ja.is_number()) flag = ja.get<double>() != 0 ? 1 : 0;
        else return c.fail(GGERR_BAD_REQUEST, "aligned must be boolean");
        c.params["aligned"] = flag;
    }
    return c.send(GGCMD_POINTER_SCAN);
}
json tool_pointer_chain(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    if (!put_addr_param(a, c.params))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    long long md = 8;
    if (a.contains("max_depth")) {
        const json& mj = a["max_depth"];
        if (mj.is_number()) md = mj.get<long long>();
        else if (mj.is_string()) md = strtoll(mj.get<std::string>().c_str(), nullptr, 0);
    }
    c.params["max_depth"] = md;
    return c.send(GGCMD_POINTER_CHAIN);
}
json tool_resolve_symbol(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    if (!put_addr_param(a, c.params))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    json out;
    int rc = c.device_call(GGCMD_RESOLVE_SYMBOL, c.params, &out);
    if (rc != 0) return c.fail(rc, c.last_error());
    return c.ok(out);
}

// ---- 进程控制 ----
json tool_pause(Session::CallCtx& c, const json&) { return c.send(GGCMD_PAUSE); }
json tool_resume(Session::CallCtx& c, const json&) { return c.send(GGCMD_RESUME); }

// ---- GG-TOUCH 内核虚拟触摸 ----
json tool_ghost_tap(Session::CallCtx& c, const json& a) {
    if (!a.contains("x") || !a.contains("y"))
        return c.fail(GGERR_BAD_REQUEST, "x,y required");
    c.params["x"] = a["x"];
    c.params["y"] = a["y"];
    if (a.contains("hold_ms")) c.params["hold_ms"] = a["hold_ms"];
    return c.send(GGCMD_GHOST_TAP);
}
json tool_ghost_swipe(Session::CallCtx& c, const json& a) {
    if (!a.contains("x1") || !a.contains("y1") ||
        !a.contains("x2") || !a.contains("y2"))
        return c.fail(GGERR_BAD_REQUEST, "x1,y1,x2,y2 required");
    c.params["x1"] = a["x1"]; c.params["y1"] = a["y1"];
    c.params["x2"] = a["x2"]; c.params["y2"] = a["y2"];
    if (a.contains("ms")) c.params["ms"] = a["ms"];
    return c.send(GGCMD_GHOST_SWIPE);
}

// ---- 状态/内核 ----
json tool_self_test(Session::CallCtx& c, const json&) { return c.send(GGCMD_SELF_TEST); }
json tool_kernel_status(Session::CallCtx& c, const json&) { return c.send(GGCMD_KERNEL_STATUS); }
json tool_status(Session::CallCtx& c, const json&) {
    return c.ok(c.s->local_status());
}
json tool_shell(Session::CallCtx& c, const json& a) {
    if (!c.cfg().shell_enabled)
        return c.fail(GGERR_SHELL_DISABLED, "shell disabled (set shell_enabled=true in config)");
    if (!a.contains("cmd")) return c.fail(GGERR_BAD_REQUEST, "cmd required");
    c.params["cmd"] = a["cmd"];
    return c.send(GGCMD_SHELL);
}
json tool_device_info(Session::CallCtx& c, const json&) { return c.send(GGCMD_DEVICE_INFO); }

// ---- UI 联动（人和 AI 共用一个工作上下文）--------------------------------
//
// 前提：agentd 的 g_eng 是全局单例，UI 和 MCP 服务器各开一条连接时，
// **搜索结果集、attach 目标本来就是同一份**。所以这里要补的不是数据同步，
// 而是"对方在干什么"：
//
//   gg_context     AI 读人手动搜了什么值/类型/区域、选中了哪一行
//   gg_ui_command  AI 让人面前的窗口执行动作（人不用把参数再敲一遍）
//
// 通道是 build/gg-shared/ 下的文件（见 server/shared_ctx.h）：不用端口、
// 崩溃安全、出问题 cat 一下就知道谁写了什么。UI 侧每 200ms 轮询指令。
//
// 设计取舍：命令只覆盖"看"和"定位"类操作（attach / 搜索 / 选中 / 读取 /
// 导出），不含 write / freeze —— 改内存让 AI 走 gg_write 那条已有的、
// 有 safe_mode 拦截和审计的路径，别在这里开第二条。

json tool_context(Session::CallCtx& c, const json&) {
    json out = json::object();
    json face;
    if (!ctx::load(&face)) {
        out["ui"] = json{{"running", false},
                         {"hint", "没读到 build/gg-shared/context.json —— gg-ui 没启动过，"
                                  "或者两个进程的 GG_SHARED_DIR / 工作目录不一致"}};
    } else {
        const bool alive = ctx::ui_alive(face);
        const long long age = ctx::now_secs() - face.value("ts", (long long)0);
        out["ui"] = json{{"running", alive}, {"stale_secs", age}};
        // 死了的上下文仍有价值（能看到人最后在搜什么），所以状态字段照给，
        // 只是显式标出它是旧的 —— 别让 AI 把十分钟前的选中行当成当前值。
        out["ui"]["target"]      = face.value("target", json::object());
        out["ui"]["search"]      = face.value("search", json::object());
        out["ui"]["selected"]    = face.value("selected", json::object());
        out["ui"]["results"]     = face.value("results", json::object());
        out["ui"]["last_action"] = face.value("last_action", "");
        out["ui"]["log_tail"]    = face.value("log_tail", json::array());
        out["ui"]["window"]      = face.value("window", json::object());
        if (!alive)
            out["ui"]["hint"] = "上下文超过 3 秒没更新，gg-ui 可能已关闭；"
                                "下面的 search/selected 只是它最后的状态";
    }

    // 设备侧实时快照：文件里的 total 是 UI 上次刷新时的数字，可能已经变了
    // （比如 AI 刚搜过）。给一份当下的，AI 才不会拿旧数字推理。
    json live;
    if (c.device_call(GGCMD_RESULT_COUNT, json::object(), &live) == 0)
        out["device"] = live;
    return c.ok(out);
}

json tool_ui_command(Session::CallCtx& c, const json& a) {
    const std::string op = a.value("op", "");
    static const char* kOps[] = {"attach", "search", "search_range", "fuzzy_init",
                                 "refine", "select", "read", "export", "refresh"};
    bool known = false;
    for (const char* k : kOps) if (op == k) { known = true; break; }
    if (!known)
        return c.fail(GGERR_BAD_REQUEST,
                      "op 必须是 attach / search / search_range / fuzzy_init / refine / "
                      "select / read / export / refresh 之一");

    // 先确认对端活着。UI 没开时把命令写进去等于埋雷：用户下次开 UI 会执行
    // 一条几十分钟前的 attach。
    json face;
    if (!ctx::load(&face) || !ctx::ui_alive(face))
        return c.fail(GGERR_BUSY,
                      "gg-ui 没在运行（context.json 缺失或超过 3 秒未更新）——"
                      "先启动 build\\pc\\gg-ui.exe 再下发指令");

    static unsigned seq = 0;
    char idbuf[64];
    // id 带 pid + 时间戳：UI 靠它去重（同一个 id 只执行一次），
    // 单用递增计数器在多进程/重启后会撞。
    snprintf(idbuf, sizeof(idbuf), "%d-%lld-%u",
             (int)GG_GETPID(), ctx::now_secs(), ++seq);

    json cmd;
    cmd["id"]   = idbuf;
    cmd["op"]   = op;
    cmd["args"] = a.value("args", json::object());
    cmd["ts"]   = ctx::now_secs();
    if (!ctx::save_cmd(cmd))
        return c.fail(GGERR_INTERNAL, "写不了 build/gg-shared/cmd.json（目录不可写？）");

    // 等 UI 消费。UI 轮询 200ms，正常 1 秒内出结果；等满 5 秒就当它没处理
    // （比如 UI 卡在别的同步操作里），返回 queued 让调用方自己决定要不要重试。
    json res;
    for (int i = 0; i < 100; ++i) {
        if (ctx::load_result(&res) && res.value("id", std::string()) == idbuf) {
            json wrapped = json{{"op", op}, {"ok", res.value("ok", false)}};
            if (res.contains("data")) wrapped["data"] = res["data"];
            if (res.contains("err"))  wrapped["err"]  = res["err"];
            return c.ok(wrapped);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return c.ok(json{{"op", op}, {"queued", true}, {"id", idbuf},
                     {"hint", "UI 5 秒内没回执，指令仍留在队列里；"
                              "可以用 gg_context 看它有没有执行"}});
}

// ---- 硬件断点 ----
json tool_watch_set(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    c.params["op"] = "set";
    if (!put_addr_param(a, c.params))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    if (a.contains("type")) c.params["type"] = a["type"];
    if (a.contains("len")) c.params["len"] = a["len"];
    return c.send(GGCMD_WATCH);
}
json tool_watch_poll(Session::CallCtx& c, const json&) {
    c.params["op"] = "poll";
    return c.send(GGCMD_WATCH);
}
json tool_watch_inject(Session::CallCtx& c, const json& a) {
    /* 调试: 手动构造命中, 走模块解析+回溯+预反汇编管道 */
    c.params["op"] = "inject";
    /* pc/fp/sp/lr 同样规范化：schema 写的是 string，客户端传数字时若原样透传，
     * JSON double 会丢掉 >2^53 的精度，且 agentd 侧要额外兜一次。 */
    for (const char* k : {"pc", "fp", "sp", "lr"}) {
        if (!a.contains(k)) continue;
        uint64_t v = 0;
        if (!parse_addr(a[k], &v))
            return c.fail(GGERR_BAD_REQUEST,
                          std::string(k) + " must be a non-negative integer or numeric string");
        c.params[k] = addr_to_wire(v);
    }
    return c.send(GGCMD_WATCH);
}
json tool_watch_clear(Session::CallCtx& c, const json&) {
    c.params["op"] = "clear";
    return c.send(GGCMD_WATCH);
}
json tool_disassemble(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    if (!put_addr_param(a, c.params))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    if (a.contains("length")) c.params["length"] = a["length"];
    return c.send(GGCMD_DISASSEMBLE);
}

// ---- AI 标注库（本地 JSON 持久化, 不经过 agentd） ----
json tool_annotate_add(Session::CallCtx& c, const json& a) {
    if (!a.contains("tag")) return c.fail(GGERR_BAD_REQUEST, "tag required");
    json saved = c.store().add(a);
    if (saved.is_null()) return c.fail(GGERR_BAD_REQUEST, "add failed");
    return c.ok(json{{"saved", saved}});
}
json tool_annotate_list(Session::CallCtx& c, const json& a) {
    std::string tag = a.value("tag", "");
    return c.ok(c.store().list(tag));
}
json tool_annotate_get(Session::CallCtx& c, const json& a) {
    std::string tag = a.value("tag", "");
    json saved = c.store().get(tag);
    if (saved.is_null()) return c.fail(GGERR_NOT_FOUND, "annotation not found: " + tag);
    return c.ok(saved);
}
json tool_annotate_remove(Session::CallCtx& c, const json& a) {
    std::string tag = a.value("tag", "");
    bool removed = c.store().remove(tag);
    return c.ok(json{{"removed", removed}, {"tag", tag}});
}
json tool_annotate_context(Session::CallCtx& c, const json& a) {
    std::string tag = a.value("tag", "");
    return c.ok(json{{"context", c.store().context_text(tag)}});
}

// ---- 分发表（单一数据源；与 schema.cpp::build() 的工具清单交叉校验见
//      tests/unit/check_dispatch_consistency.py）----
const ToolEntry kTools[] = {
    {"gg_list_processes",   tool_list_processes,   false},
    {"gg_attach",           tool_attach,           false},
    {"gg_detach",           tool_detach,           false},
    {"gg_target_info",      tool_target_info,      false},
    {"gg_module_list",      tool_module_list,      false},

    {"gg_search",           tool_search,           false},
    {"gg_search_unknown_init", tool_search_unknown_init, false},
    {"gg_search_refine",    tool_search_refine,    false},
    {"gg_search_undo",      tool_search_undo,      false},
    {"gg_filter_results",   tool_filter_results,   false},
    {"gg_screen_diff",      tool_screen_diff,      false},
    {"gg_screen_watch_refine", tool_screen_watch_refine, false},
    {"gg_search_range",     tool_search_range,     false},
    {"gg_result_count",     tool_result_count,     false},
    {"gg_get_results",      tool_get_results,      false},
    {"gg_clear_results",    tool_clear_results,    false},

    {"gg_read",             tool_read,             false},
    {"gg_read_bytes",       tool_read_bytes,       false},
    {"gg_hex_dump",         tool_read_bytes,       false},
    {"gg_view_memory",      tool_view_memory,      false},
    {"gg_write",            tool_write,            true},
    {"gg_set_results",      tool_set_results,      true},
    {"gg_freeze",           tool_freeze,           true},
    {"gg_unfreeze",         tool_unfreeze,         true},
    {"gg_freeze_results",   tool_freeze_results,   true},

    {"gg_module_base",      tool_module_base,      false},
    {"gg_calc_offset",      tool_calc_offset,      false},
    {"gg_aob_resolve",      tool_aob_resolve,      false},
    {"gg_pointer_scan",     tool_pointer_scan,     false},
    {"gg_pointer_chain",    tool_pointer_chain,    false},
    {"gg_resolve_symbol",   tool_resolve_symbol,   false},

    {"gg_pause",            tool_pause,            true},
    {"gg_resume",           tool_resume,           true},

    {"gg_ghost_tap",        tool_ghost_tap,        true},
    {"gg_ghost_swipe",      tool_ghost_swipe,      true},

    {"gg_self_test",        tool_self_test,        false},
    {"gg_kernel_status",    tool_kernel_status,    false},
    {"gg_status",           tool_status,           false},
    {"gg_shell",            tool_shell,            true},
    {"gg_device_info",      tool_device_info,      false},

    {"gg_watch_set",        tool_watch_set,        true},
    {"gg_watch_poll",       tool_watch_poll,       false},
    {"gg_watch_inject",     tool_watch_inject,     false},
    {"gg_watch_clear",      tool_watch_clear,      true},
    {"gg_disassemble",      tool_disassemble,      false},

    {"gg_annotate_add",     tool_annotate_add,     false},
    {"gg_annotate_list",    tool_annotate_list,    false},
    {"gg_annotate_get",     tool_annotate_get,     false},
    {"gg_annotate_remove",  tool_annotate_remove,  false},
    {"gg_annotate_context", tool_annotate_context, false},

    // UI 联动：和 gg-ui 共用一份工作上下文
    {"gg_context",          tool_context,          false},
    {"gg_ui_command",       tool_ui_command,       false},
};

}  // namespace

json Session::call_tool(const std::string& name, const json& args) {
    /* HTTP 传输下 httplib 用线程池并发处理 POST /mcp，本函数会并发进入。
     * AgentClient::mtx_ 只串行化单次 call，保护不到两件事：
     *   - client_.last_error / last_outcome 的跨调用读写（std::string 并发写 = UB）；
     *   - device_call 的 close() + 重放会把另一个线程正在用的连接掐断。
     * 用会话级锁把整次工具调用串起来。设备端本来就单连接串行，
     * 这把锁不会带来额外的实际串行代价。
     * 注意：local_status() 假定调用方持有本锁，不要再自己加锁（std::mutex 非递归）。 */
    std::lock_guard<std::mutex> lk(mtx_);
    for (const ToolEntry& t : kTools) {
        if (name == t.name) {
            // 写操作在 safe_mode 下本地拒绝（双保险；表驱动 write_op 字段为单一数据源）
            if (t.write_op && cfg_.safe_mode) {
                return fail(GGERR_SAFE_MODE, "safe mode: " + name + " denied (read-only session)");
            }
            CallCtx c{this, json::object()};
            return t.fn(c, args);
        }
    }
    return fail(GGERR_BAD_REQUEST, "unknown tool: " + name);
}

}  // namespace gg
