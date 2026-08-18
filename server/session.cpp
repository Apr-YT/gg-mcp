// session.cpp — 43 个 MCP 工具实现（薄代理 → gg-agentd）
//
// M1 表驱动重构（2026-08-18）：call_tool 的分发从 if (name == ...) 链
// 改为 kTools[] 表（{name, fn, write_op}）。参数映射逐行搬运，零行为变化：
//   - safe_mode 写拦截从硬编码名字集合改为表的 write_op 字段（单一数据源）
//   - gg_read_bytes 与 gg_hex_dump 合并分支拆为两个表项、共用同一 handler
//   - 分发一致性护栏（tests/unit/check_dispatch_consistency.py）已升级支持表驱动解析
#include "session.h"

#include "ida_client.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gg {

Session::Session(ServerConfig cfg)
    : cfg_(std::move(cfg)),
      client_([&] {
          AgentClient::Config ac;
          ac.adb = cfg.adb;
          ac.serial = cfg.serial;
          ac.agent_port = cfg.agent_port;
          ac.print_events = cfg.print_events;
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
    if (rc == GGERR_NO_DEVICE) {
        // 一次重连重试
        client_.close();
        rc = client_.call(cmd, params, out);
    }
    return rc;
}

int Session::norm_type(const json& t) {
    if (t.is_string()) {
        const std::string s = t.get<std::string>();
        if (s == "byte" || s == "u8") return VT_BYTE;
        if (s == "word" || s == "u16") return VT_WORD;
        if (s == "dword" || s == "u32" || s == "int") return VT_DWORD;
        if (s == "qword" || s == "u64" || s == "long") return VT_QWORD;
        if (s == "float" || s == "f32") return VT_FLOAT;
        if (s == "double" || s == "f64") return VT_DOUBLE;
    }
    if (t.is_number_integer()) {
        int v = t.get<int>();
        if (v >= VT_BYTE && v <= VT_DOUBLE) return v;
    }
    return VT_DWORD;
}

int Session::norm_region(const json& r) {
    if (r.is_string()) {
        const std::string s = r.get<std::string>();
        if (s == "anonymous" || s == "anon") return REGION_ANONYMOUS;
        if (s == "writable") return REGION_WRITABLE;
        if (s == "all") return REGION_ALL;
    }
    if (r.is_number_integer()) {
        int v = r.get<int>();
        if (v >= REGION_ANONYMOUS && v <= REGION_ALL) return v;
    }
    return REGION_ANONYMOUS;
}

int Session::norm_op(const json& o) {
    if (o.is_string()) {
        const std::string s = o.get<std::string>();
        if (s == "increased" || s == "inc") return REFINE_INCREASED;
        if (s == "decreased" || s == "dec") return REFINE_DECREASED;
        if (s == "unchanged" || s == "same") return REFINE_UNCHANGED;
        if (s == "changed" || s == "diff") return REFINE_CHANGED;
    }
    if (o.is_number_integer()) {
        int v = o.get<int>();
        if (v >= REFINE_INCREASED && v <= REFINE_CHANGED) return v;
    }
    return REFINE_CHANGED;
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

json Session::ida_resolve(const std::string& offset_hex) {
    if (!cfg_.ida.enabled) {
        return {
            {"configured", false},
            {"hint", "set configs/ggmcp.json \"ida\":{\"enabled\":true,\"mcp_url\":\"http://127.0.0.1:8000\"}"},
        };
    }
    json call = ida_call(cfg_.ida.mcp_url, cfg_.ida.mcp_path,
                         cfg_.ida.tool_name, cfg_.ida.arg_name, offset_hex,
                         cfg_.ida.timeout_ms);
    json out;
    out["configured"] = true;
    out["mcp_url"] = cfg_.ida.mcp_url;
    out["tool"] = cfg_.ida.tool_name;
    if (call.value("ok", false)) {
        std::string text = ida_extract_text(call["result"]);
        if (!text.empty()) out["result_text"] = text;
        out["raw"] = call["result"];
    } else {
        out["error"] = call.value("error", "ida mcp unavailable");
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
    c.params["type"] = c.norm_type(a.value("type", json()));
    c.params["value"] = a["value"];
    c.params["region"] = c.norm_region(a.value("region", json()));
    return c.send(GGCMD_SEARCH);
}
json tool_search_unknown_init(Session::CallCtx& c, const json& a) {
    c.params["type"] = c.norm_type(a.value("type", json()));
    c.params["region"] = c.norm_region(a.value("region", json()));
    return c.send(GGCMD_SEARCH_UNKNOWN_INIT);
}
json tool_search_refine(Session::CallCtx& c, const json& a) {
    c.params["op"] = c.norm_op(a.value("op", json()));
    return c.send(GGCMD_SEARCH_REFINE);
}
json tool_search_range(Session::CallCtx& c, const json& a) {
    if (!a.contains("min") || !a.contains("max"))
        return c.fail(GGERR_BAD_REQUEST, "min and max required");
    c.params["type"] = c.norm_type(a.value("type", json()));
    c.params["min"] = a["min"];
    c.params["max"] = a["max"];
    c.params["region"] = c.norm_region(a.value("region", json()));
    return c.send(GGCMD_SEARCH_RANGE);
}
json tool_result_count(Session::CallCtx& c, const json&) { return c.send(GGCMD_RESULT_COUNT); }
json tool_get_results(Session::CallCtx& c, const json& a) {
    c.params["offset"] = a.value("offset", 0);
    c.params["limit"] = a.value("limit", 100);
    c.params["type"] = c.norm_type(a.value("type", json()));
    return c.send(GGCMD_GET_RESULTS);
}
json tool_clear_results(Session::CallCtx& c, const json&) { return c.send(GGCMD_CLEAR_RESULTS); }

// ---- 内存读/写 ----
json tool_read(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    c.params["addr"] = a["addr"];
    c.params["type"] = c.norm_type(a.value("type", json()));
    if (a.contains("force")) c.params["force"] = a["force"].get<int>();
    return c.send(GGCMD_READ);
}
json tool_read_bytes(Session::CallCtx& c, const json& a) {   // gg_read_bytes / gg_hex_dump
    if (!a.contains("addr")) return c.fail(GGERR_BAD_REQUEST, "addr required");
    c.params["addr"] = a["addr"];
    c.params["len"] = a.value("len", 64);
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
    c.params["addr"] = a["addr"];
    c.params["value"] = a["value"];
    c.params["type"] = c.norm_type(a.value("type", json()));
    return c.send(GGCMD_WRITE);
}
json tool_set_results(Session::CallCtx& c, const json& a) {
    if (!a.contains("values")) return c.fail(GGERR_BAD_REQUEST, "values required");
    c.params["values"] = a["values"];
    c.params["type"] = c.norm_type(a.value("type", json()));
    return c.send(GGCMD_SET_RESULTS);
}
json tool_freeze(Session::CallCtx& c, const json& a) {
    if (!a.contains("addr") || !a.contains("value"))
        return c.fail(GGERR_BAD_REQUEST, "addr and value required");
    c.params["addr"] = a["addr"];
    c.params["value"] = a["value"];
    c.params["type"] = c.norm_type(a.value("type", json()));
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
    c.params["type"] = c.norm_type(a.value("type", json()));
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
    // IDA 联动：本地静态解析完成后，把模块内偏移发给 IDA MCP 取函数名/伪代码
    std::string off = out.value("offset_in_module", "");
    if (!off.empty()) {
        out["ida"] = c.ida_resolve(off);
    } else {
        out["ida"] = {{"configured", c.cfg().ida.enabled},
                      {"hint", "addr not in a file-backed module (anonymous region)"}};
    }
    return c.ok(out);
}

// ---- 进程控制 ----
json tool_pause(Session::CallCtx& c, const json&) { return c.send(GGCMD_PAUSE); }
json tool_resume(Session::CallCtx& c, const json&) { return c.send(GGCMD_RESUME); }

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
