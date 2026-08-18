// protocol.h — MCP JSON-RPC 协议处理（initialize/tools/list/tools/call/ping）
#pragma once

#include <string>

#include "schema.h"
#include "../session.h"

namespace gg {

class McpProtocol {
public:
    explicit McpProtocol(Session& session) : session_(session) {}

    // 处理一条 JSON-RPC 消息。返回响应 JSON 字符串（通知/未知返回空串）。
    std::string handle(const std::string& raw);

private:
    json handle_request(const json& msg);
    Session& session_;
};

}  // namespace gg
