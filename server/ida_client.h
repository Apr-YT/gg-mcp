// ida_client.h — IDA Pro MCP 联动客户端（预留接口）
//
// gg_resolve_symbol 的静态增强层：把"模块内偏移"发给外部 IDA Pro MCP 服务器
// （JSON-RPC 2.0 over HTTP，Streamable HTTP / 传统 POST 均可容错解析），
// 取回函数名 / 反编译伪代码。纯运行时（agentd）+ 纯静态（IDA）结合。
//
// 任何失败都 fail-soft：不抛异常、不阻断主流程，由调用方把 error 写进响应。
#pragma once

#include <string>

#include "../third_party/json.hpp"

namespace gg {

using json = nlohmann::json;

// 对 IDA MCP 端点做一次 tools/call（自动先 initialize + initialized 通知）。
//   mcp_url:   基址，如 "http://127.0.0.1:8000"
//   mcp_path:  端点路径，默认 "/mcp"
//   tool_name: MCP 工具名，如 "decompile_function"（按所接服务器配置）
//   arg_name:  工具参数名，如 "address"
//   arg_value: 参数值（模块内偏移 hex，如 "0x1234"）
//   timeout_ms: 单请求超时
// 返回 {"ok":true,"result":<工具原始结果>} 或 {"ok":false,"error":"..."}。
json ida_call(const std::string& mcp_url, const std::string& mcp_path,
              const std::string& tool_name, const std::string& arg_name,
              const std::string& arg_value, int timeout_ms);

// 从 tools/call 响应里尽力提取可读文本（content[].text / content[].content /
// result 字符串），失败返回空串。
std::string ida_extract_text(const json& call_result);

}  // namespace gg
