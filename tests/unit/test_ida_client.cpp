// test_ida_client.cpp — IDA MCP 客户端单测（需 mock_ida_mcp.ps1 在 18099 运行）
#include "../../server/ida_client.h"

#include <cstdio>
#include <string>

using gg::json;

static int fails = 0;
static void check(bool c, const char *m) {
    std::printf("%s %s\n", c ? "ok  :" : "FAIL:", m);
    if (!c) fails++;
}

int main() {
    // 1) SSE 端点（/mcp）
    json r1 = gg::ida_call("http://127.0.0.1:18099", "/mcp",
                           "decompile_function", "address", "0x1234", 3000);
    check(r1.value("ok", false), "SSE: initialize+tools/call ok");
    std::string t1 = gg::ida_extract_text(r1.value("result", json::object()));
    check(t1.find("sub_1234") != std::string::npos && t1.find("0x1234") != std::string::npos,
          "SSE: result_text 含函数名与参数");
    std::printf("    text: %s\n", t1.c_str());

    // 2) 纯 JSON 端点（/mcpjson）
    json r2 = gg::ida_call("http://127.0.0.1:18099", "/mcpjson",
                           "decompile_function", "address", "0xdead", 3000);
    check(r2.value("ok", false), "JSON: initialize+tools/call ok");
    std::string t2 = gg::ida_extract_text(r2.value("result", json::object()));
    check(t2.find("0xdead") != std::string::npos, "JSON: result_text 含参数");
    std::printf("    text: %s\n", t2.c_str());

    // 3) 服务器不可达 → fail-soft
    json r3 = gg::ida_call("http://127.0.0.1:18098", "/mcp",
                           "decompile_function", "address", "0x1", 1500);
    check(!r3.value("ok", true) && !r3.value("error", "").empty(),
          "server down -> ok=false + error");

    // 4) 配置缺失
    json r4 = gg::ida_call("", "", "", "address", "0x1", 1000);
    check(!r4.value("ok", true), "empty config -> ok=false");

    std::printf(fails ? "\n== FAILED: %d ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
