// protocol.cpp — MCP JSON-RPC 处理
//
// 错误分层约定（对外只暴露标准 JSON-RPC 码，内部细节进 stderr）：
//   -32700  Parse error        —— 根本不是 JSON
//   -32600  Invalid Request    —— 是 JSON 但缺必需字段（method 缺失/类型错）
//   -32601  Method not found   —— 方法名不认识
//   -32602  Invalid params     —— 方法认识但参数不合法
//   -32603  Internal error     —— 服务端自身异常，不回传堆栈或路径
// 设备通信类错误仍走 tools/call 的 isError + 业务错误码，不与协议层混淆。
#include "protocol.h"

#include <iostream>

namespace gg {

namespace {

// 带 JSON-RPC 错误码的异常：让 handle() 能把「方法不存在」和「内部炸了」分开。
struct RpcError : std::runtime_error {
    int code;
    RpcError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
};

std::string make_error(const json& id, int code, const std::string& msg) {
    json err;
    err["jsonrpc"] = "2.0";
    err["id"] = id;
    err["error"] = {{"code", code}, {"message", msg}};
    return err.dump();
}

}  // namespace

std::string McpProtocol::handle(const std::string& raw) {
    json msg;
    try {
        msg = json::parse(raw);
    } catch (...) {
        return make_error(nullptr, -32700, "Parse error");
    }
    if (!msg.is_object()) return make_error(nullptr, -32600, "Invalid Request");

    // 通知（无 id）：即使方法不认识也不能回复
    if (!msg.contains("id")) return "";

    if (!msg.contains("method") || !msg["method"].is_string())
        return make_error(msg["id"], -32600, "Invalid Request");

    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = msg["id"];

    try {
        resp["result"] = handle_request(msg);
    } catch (const RpcError& e) {
        resp.erase("result");
        resp["error"] = {{"code", e.code}, {"message", e.what()}};
    } catch (const std::exception& e) {
        // 详细原因只进服务端日志：不回传内部路径、文件名或异常原文
        std::cerr << "[gg-mcp] internal error handling " << msg["method"].get<std::string>()
                  << ": " << e.what() << "\n";
        resp.erase("result");
        resp["error"] = {{"code", -32603}, {"message", "Internal error"}};
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
        result["serverInfo"] = {{"name", "gg-ghost-mcp"}, {"version", "1.0.0"}};
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
        if (!params.is_object()) throw RpcError(-32602, "Invalid params: params must be an object");
        if (!params.contains("name") || !params["name"].is_string())
            throw RpcError(-32602, "Invalid params: name is required");
        json args = json::object();
        if (params.contains("arguments")) {
            if (!params["arguments"].is_object())
                throw RpcError(-32602, "Invalid params: arguments must be an object");
            args = params["arguments"];
        }
        json call_result = session_.call_tool(params["name"].get<std::string>(), args);
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

    throw RpcError(-32601, "Method not found: " + method);
}

}  // namespace gg
