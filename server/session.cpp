// session.cpp — 45 个 MCP 工具实现（薄代理 → gg-agentd）
//
// M1 表驱动重构（2026-08-18）：call_tool 的分发从 if (name == ...) 链
// 改为 kTools[] 表（{name, fn, write_op}）。参数映射逐行搬运，零行为变化：
//   - safe_mode 写拦截从硬编码名字集合改为表的 write_op 字段（单一数据源）
//   - gg_read_bytes 与 gg_hex_dump 合并分支拆为两个表项、共用同一 handler
//   - 分发一致性护栏（tests/unit/check_dispatch_consistency.py）已升级支持表驱动解析
#include "session.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

// 把 uint64 地址格式化成下行用的字符串
static std::string addr_to_wire(uint64_t addr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)addr);
    return std::string(buf);
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
          ac.print_events = cfg_.print_events;
          ac.wire_key = cfg_.wire_key;
          return ac;
      }()),
      store_(cfg_.annotations_file.empty() ? "annotations.json" : cfg_.annotations_file) {}

void Session::relay_annotate(const std::string& op, const json& ann) {
    json out;
    json params{{"type", "annotate"},
                {"data", {{"op", op}, {"ann", ann}}}};
    device_call(GGCMD_RELAY_EVENT, params, &out);   // best-effort，失败忽略
}

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

int Session::device_call(uint16_t cmd, const json& params, json* out) {
    int rc = client_.call(cmd, params, out);
    // 只有「连接都没建立起来」才是干净失败：请求一个字节都没发出去，重放不会
    // 改变任何远端状态。发送失败 / 响应超时 / 读中断 都可能意味着远端已经执行过，
    // 盲目重放会让 gg_write、gg_set_results、gg_freeze 这类有副作用的命令跑两次。
    if (rc != 0 && client_.last_outcome == CallOutcome::ConnectFailed) {
        client_.close();
        rc = client_.call(cmd, params, out);
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
json tool_search_range(Session::CallCtx& c, const json& a) {
    if (!a.contains("min") || !a.contains("max"))
        return c.fail(GGERR_BAD_REQUEST, "min and max required");
    if (!c.put_type(a.value("type", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid type");
    c.params["min"] = a["min"];
    c.params["max"] = a["max"];
    if (!c.put_region(a.value("region", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid region");
    return c.send(GGCMD_SEARCH_RANGE);
}
json tool_result_count(Session::CallCtx& c, const json&) { return c.send(GGCMD_RESULT_COUNT); }
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
    if (a.contains("force")) c.params["force"] = a["force"].get<int>();
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

        int force = a.contains("force") ? a["force"].get<int>() : 0;

        // 先写 .part，全部段成功才改名 —— 避免中途失败把同名旧文件截断成半截数据。
        ensure_parent_dirs(path);
        const std::string tmp_path = path + ".part";
        FILE* f = fopen(tmp_path.c_str(), "wb");
        if (!f) return c.fail(GGERR_BAD_REQUEST, "cannot open save_to: " + tmp_path);

        const long long SEG = 65536;   // 与 agentd h_read_bytes 上限一致
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
            if (force) params["force"] = force;
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
            std::remove(path.c_str());   // Windows 上 rename 覆盖已存在文件会失败
            promoted = (std::rename(tmp_path.c_str(), path.c_str()) == 0);
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
     * len 传字符串则抛异常被吞成 -32603。上限 65536 与 agentd h_read_bytes 一致。 */
    uint64_t addr = 0;
    if (!parse_addr(a["addr"], &addr))
        return c.fail(GGERR_BAD_REQUEST, "addr must be a non-negative integer or numeric string");
    long long len = 64;
    if (!parse_len(a.contains("len") ? a["len"] : json(), 64, 65536, &len))
        return c.fail(GGERR_BAD_REQUEST, "len must be an integer in 1..65536");
    c.params["addr"] = addr_to_wire(addr);
    c.params["len"] = len;
    if (a.contains("force")) c.params["force"] = a["force"].get<int>();
    return c.send(GGCMD_READ_BYTES);
}
json tool_view_memory(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    c.params["addr"] = a["addr"];
    long long rng = 8;
    if (a.contains("range")) {
        const json& rj = a["range"];
        if (rj.is_number()) rng = rj.get<long long>();
        else if (rj.is_string()) rng = strtoll(rj.get<std::string>().c_str(), nullptr, 0);
    }
    c.params["range"] = rng;
    if (a.contains("element_type")) c.params["element_type"] = a["element_type"];
    if (a.contains("force")) c.params["force"] = a["force"].get<int>();
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
    if (!a.contains("values")) return c.fail(GGERR_BAD_REQUEST, "values required");
    c.params["values"] = a["values"];
    if (!c.put_type(a.value("type", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid type");
    return c.send(GGCMD_SET_RESULTS);
}
json tool_freeze(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr") || !a.contains("value"))
        return c.fail(GGERR_BAD_REQUEST, "addr and value required");
    c.params["addr"] = a["addr"];
    c.params["value"] = a["value"];
    if (!c.put_type(a.value("type", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid type");
    return c.send(GGCMD_FREEZE);
}
json tool_unfreeze(Session::CallCtx& c, const json& a) {
    if (a.contains("addr")) c.params["addr"] = a["addr"];
    return c.send(GGCMD_UNFREEZE);
}

// ---- 模块/符号 ----
json tool_module_base(Session::CallCtx& c, const json& a) {
    if (!a.contains("name")) return c.fail(GGERR_BAD_REQUEST, "name required");
    c.params["name"] = a["name"];
    return c.send(GGCMD_MODULE_BASE);
}
json tool_calc_offset(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    c.params["addr"] = a["addr"];
    if (a.contains("module")) c.params["module"] = a["module"];
    return c.send(GGCMD_CALC_OFFSET);
}
json tool_pointer_scan(Session::CallCtx& c, const json& a) {
    if (!c.put_type(a.value("type", json()))) return c.fail(GGERR_BAD_REQUEST, "invalid type");
    c.params["cap"] = a.value("cap", 5000);
    return c.send(GGCMD_POINTER_SCAN);
}
json tool_pointer_chain(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    c.params["addr"] = a["addr"];
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
    c.params["addr"] = a["addr"];
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
json tool_kernel_status(Session::CallCtx& c, const json&) { return c.send(GGCMD_KERNEL_STATUS); }
json tool_kernel_load_gg(Session::CallCtx& c, const json& a) {
    c.params["pattern"] = a.value("pattern", c.cfg().gg_version_pattern);
    c.params["strict"] = a.value("strict", c.cfg().gg_strict);
    if (a.contains("dir")) c.params["dir"] = a["dir"];
    return c.send(GGCMD_LOAD_GG);
}
json tool_core_versions(Session::CallCtx& c, const json& a) {
    c.params["pattern"] = a.value("pattern", c.cfg().gg_version_pattern);
    return c.send(GGCMD_CORE_VERSIONS);
}
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

// ---- 硬件断点 ----
json tool_watch_set(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    c.params["op"] = "set";
    c.params["addr"] = a["addr"];
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
    if (a.contains("pc")) c.params["pc"] = a["pc"];
    if (a.contains("fp")) c.params["fp"] = a["fp"];
    if (a.contains("sp")) c.params["sp"] = a["sp"];
    if (a.contains("lr")) c.params["lr"] = a["lr"];
    return c.send(GGCMD_WATCH);
}
json tool_watch_clear(Session::CallCtx& c, const json&) {
    c.params["op"] = "clear";
    return c.send(GGCMD_WATCH);
}
json tool_disassemble(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    c.params["addr"] = a["addr"];
    if (a.contains("length")) c.params["length"] = a["length"];
    return c.send(GGCMD_DISASSEMBLE);
}

// ---- AI 标注库（本地 JSON 持久化, 不经过 agentd；变更经中继同步到手机悬浮窗） ----
json tool_annotate_add(Session::CallCtx& c, const json& a) {
    if (!a.contains("tag")) return c.fail(GGERR_BAD_REQUEST, "tag required");
    json saved = c.store().add(a);
    if (saved.is_null()) return c.fail(GGERR_BAD_REQUEST, "add failed");
    c.relay_annotate("add", saved);
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
    c.relay_annotate("remove", json{{"tag", tag}});
    return c.ok(json{{"removed", removed}, {"tag", tag}});
}
json tool_annotate_context(Session::CallCtx& c, const json& a) {
    std::string tag = a.value("tag", "");
    return c.ok(json{{"context", c.store().context_text(tag)}});
}

// ---- 分发表（单一数据源；与 schema.cpp::build() 43 工具交叉校验见
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

    {"gg_module_base",      tool_module_base,      false},
    {"gg_calc_offset",      tool_calc_offset,      false},
    {"gg_pointer_scan",     tool_pointer_scan,     false},
    {"gg_pointer_chain",    tool_pointer_chain,    false},
    {"gg_resolve_symbol",   tool_resolve_symbol,   false},

    {"gg_pause",            tool_pause,            true},
    {"gg_resume",           tool_resume,           true},

    {"gg_ghost_tap",        tool_ghost_tap,        true},
    {"gg_ghost_swipe",      tool_ghost_swipe,      true},

    {"gg_kernel_status",    tool_kernel_status,    false},
    {"gg_kernel_load_gg",   tool_kernel_load_gg,   true},
    {"gg_core_versions",    tool_core_versions,    false},
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
};

}  // namespace

json Session::call_tool(const std::string& name, const json& args) {
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
