// ida_client.cpp — IDA Pro MCP HTTP 客户端实现（fail-soft）
#include "ida_client.h"

#include <cstdio>
#include <string>

#include "../third_party/httplib.h"

namespace gg {

namespace {

// 解析 MCP HTTP 响应：兼容纯 JSON 与 SSE（text/event-stream，取 data: 行）。
// 返回 JSON 对象；失败返回空对象。
json parse_mcp_response(const httplib::Result& res) {
    if (!res || res->body.empty()) return json();
    const std::string& body = res->body;
    if (body.empty()) return json();
    try {
        // SSE: 可能整段是 event-stream
        if (res->has_header("Content-Type") &&
            res->get_header_value("Content-Type").find("text/event-stream") != std::string::npos) {
            std::string last;
            size_t pos = 0;
            while (pos < body.size()) {
                size_t nl = body.find('\n', pos);
                std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                pos = nl == std::string::npos ? body.size() : nl + 1;
                if (line.rfind("data:", 0) == 0) {
                    std::string d = line.substr(5);
                    while (!d.empty() && (d[0] == ' ' || d[0] == '\t')) d.erase(d.begin());
                    if (!d.empty()) last = d;
                }
            }
            if (!last.empty()) return json::parse(last);
            return json();
        }
        return json::parse(body);
    } catch (...) {
        return json();
    }
}

// POST JSON-RPC 请求；返回解析后的 JSON 响应对象（可能为空）。
json post_jsonrpc(httplib::Client& cli, const std::string& path, const json& req) {
    auto res = cli.Post(path.c_str(), req.dump(), "application/json");
    return parse_mcp_response(res);
}

}  // namespace

json ida_call(const std::string& mcp_url, const std::string& mcp_path,
              const std::string& tool_name, const std::string& arg_name,
              const std::string& arg_value, int timeout_ms) {
    json out;
    if (mcp_url.empty() || mcp_path.empty() || tool_name.empty()) {
        out["ok"] = false;
        out["error"] = "ida config incomplete (mcp_url/mcp_path/tool_name)";
        return out;
    }
    try {
        httplib::Client cli(mcp_url);
        cli.set_connection_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
        cli.set_read_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
        cli.set_write_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);

        // 1) initialize（MCP 握手，尽力而为——非标准 MCP 服务器可能不支持）
        json init_req;
        init_req["jsonrpc"] = "2.0";
        init_req["id"] = 1;
        init_req["method"] = "initialize";
        init_req["params"] = {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", json::object()},
            {"clientInfo", {{"name", "gg-mcp-server"}, {"version", "1.0.0"}}},
        };
        json init_resp = post_jsonrpc(cli, mcp_path, init_req);
        // 握手失败不阻断——非标准 MCP 服务器（如 ida-mcp-pro）直接跳过
        bool skip_handshake = false;
        if (init_resp.empty()) {
            skip_handshake = true;
        } else if (init_resp.contains("error") && !init_resp["error"].is_null()) {
            int err_code = init_resp["error"].value("code", 0);
            if (err_code == -32601) {
                // Method not found → 非标准服务器，跳过握手
                skip_handshake = true;
            } else {
                out["ok"] = false;
                out["error"] = "ida mcp initialize error: " + init_resp["error"].dump();
                return out;
            }
        }

        // 2) notifications/initialized（无 id 通知，尽力而为）
        if (!skip_handshake) {
            json note;
            note["jsonrpc"] = "2.0";
            note["method"] = "notifications/initialized";
            note["params"] = json::object();
            post_jsonrpc(cli, mcp_path, note);
        }

        // 3) tools/call
        json call;
        call["jsonrpc"] = "2.0";
        call["id"] = 2;
        call["method"] = "tools/call";
        call["params"] = {
            {"name", tool_name},
            {"arguments", {{arg_name, arg_value}}},
        };
        json call_resp = post_jsonrpc(cli, mcp_path, call);
        if (call_resp.empty()) {
            out["ok"] = false;
            out["error"] = "ida mcp tools/call failed (no response)";
            return out;
        }
        if (call_resp.contains("error") && !call_resp["error"].is_null()) {
            out["ok"] = false;
            out["error"] = "ida mcp tools/call error: " + call_resp["error"].dump();
            return out;
        }
        if (!call_resp.contains("result")) {
            out["ok"] = false;
            out["error"] = "ida mcp tools/call: no result field";
            return out;
        }
        out["ok"] = true;
        out["result"] = call_resp["result"];
        return out;
    } catch (const std::exception& e) {
        out["ok"] = false;
        out["error"] = std::string("ida mcp exception: ") + e.what();
        return out;
    } catch (...) {
        out["ok"] = false;
        out["error"] = "ida mcp unknown exception";
        return out;
    }
}

std::string ida_extract_text(const json& call_result) {
    if (call_result.is_string()) return call_result.get<std::string>();
    if (!call_result.is_object()) return "";
    // MCP 标准：result.content[] = [{type:"text", text:"..."}]
    if (call_result.contains("content") && call_result["content"].is_array()) {
        for (const auto& c : call_result["content"]) {
            if (c.is_object() && c.contains("text") && c["text"].is_string()) {
                return c["text"].get<std::string>();
            }
            if (c.is_object() && c.contains("content") && c["content"].is_string()) {
                return c["content"].get<std::string>();
            }
        }
    }
    return "";
}

}  // namespace gg
