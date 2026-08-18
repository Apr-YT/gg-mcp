// protocol.cpp — MCP JSON-RPC 处理
#include "protocol.h"

namespace gg {

std::string McpProtocol::handle(const std::string& raw) {
    json msg;
    try {
        msg = json::parse(raw);
    } catch (...) {
        json err;
        err["jsonrpc"] = "2.0";
        err["id"] = nullptr;
        err["error"] = {{"code", -32700}, {"message", "Parse error"}};
        return err.dump();
    }
    if (!msg.is_object()) return "";

    // 通知（无 id）：不回复
    if (!msg.contains("id")) return "";

    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = msg["id"];

    try {
        resp["result"] = handle_request(msg);
    } catch (const std::exception& e) {
        resp["error"] = {{"code", -32603}, {"message", std::string("Internal error: ") + e.what()}};
        resp.erase("result");
    }
    return resp.dump();
}

json McpProtocol::handle_request(const json& msg) {
    std::string method = msg.value("method", "");
    json params = msg.contains("params") ? msg["params"] : json::object();

    if (method == "initialize") {
        json result;
        result["protocolVersion"] = "2024-11-05";
        result["capabilities"] = {{"tools", {{"listChanged", false}}}};
        result["serverInfo"] = {{"name", "gg-mcp-server"}, {"version", "1.0.0"}};
        return result;
    }
    if (method == "notifications/initialized" || method == "initialized") {
        return json::object();
    }
    if (method == "ping") {
        return json::object();
    }
    if (method == "tools/list") {
        json result;
        result["tools"] = json::array();
        for (const auto& t : tool_defs()) {
            json tj;
            tj["name"] = t.name;
            tj["description"] = t.description;
            tj["inputSchema"] = t.input_schema;
            result["tools"].push_back(tj);
        }
        return result;
    }
    if (method == "tools/call") {
        std::string name = params.value("name", "");
        json args = params.contains("arguments") ? params["arguments"] : json::object();
        json call_result = session_.call_tool(name, args);
        json result;
        result["content"] = call_result.value("content", json::array());
        result["isError"] = call_result.value("isError", false);
        return result;
    }
    if (method == "resources/list" || method == "prompts/list") {
        json result;
        result[method == "resources/list" ? "resources" : "prompts"] = json::array();
        return result;
    }

    throw std::runtime_error("unknown method: " + method);
}

}  // namespace gg
