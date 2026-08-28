// main.cpp — gg-ghost-mcp：PC 端 C++ MCP 服务器入口
//
// 用法:
//   gg-ghost-mcp --config configs/ggmcp.json --transport stdio
//   gg-ghost-mcp --config configs/ggmcp.json --transport http --port 8788
//
// stdio 传输：供 Claude Desktop / Claude Code 等本地 MCP 客户端。
// http 传输：POST /mcp（Streamable HTTP 风格），供 Cline 等远程客户端。
#include "mcp/protocol.h"
#include "session.h"

#include <fstream>
#include <iostream>
#include <string>

#include "../third_party/httplib.h"   // 先于 windows.h（winsock2 顺序）
#include "../third_party/json.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace gg {

static void print_banner(const ServerConfig& cfg) {
    std::cerr <<
        "==============================================\n"
        " GG-GHOST-MCP v1.0.0 (GameGuardian MCP)\n"
        " transport : " << cfg.transport << "\n"
        " agentd    : 127.0.0.1:" << cfg.agent_port << " (adb forward)\n"
        " GG 版本族 : " << cfg.gg_version_pattern
        << (cfg.gg_strict ? " (strict)" : " (lenient)") << "\n"
        " safe-mode : " << (cfg.safe_mode ? "ON (read-only)" : "off") << "\n"
        "==============================================\n";
}

static bool load_config(const std::string& path, ServerConfig* cfg) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    json j;
    try {
        f >> j;
    } catch (...) {
        return false;
    }
    cfg->transport = j.value("transport", cfg->transport);
    cfg->http_port = j.value("http_port", cfg->http_port);
    cfg->http_host = j.value("http_host", cfg->http_host);
    cfg->http_token = j.value("http_token", cfg->http_token);
    cfg->agent_port = j.value("agent_port", cfg->agent_port);
    cfg->adb = j.value("adb", cfg->adb);
    cfg->serial = j.value("serial", cfg->serial);
    cfg->safe_mode = j.value("safe_mode", cfg->safe_mode);
    cfg->shell_enabled = j.value("shell_enabled", cfg->shell_enabled);
    cfg->print_events = j.value("print_events", cfg->print_events);
    cfg->result_cap = j.value("result_cap", (int64_t)cfg->result_cap);
    cfg->annotations_file = j.value("annotations_file", cfg->annotations_file);
    if (j.contains("gg")) {
        cfg->gg_version_pattern = j["gg"].value("version_pattern", cfg->gg_version_pattern);
        cfg->gg_strict = j["gg"].value("strict", cfg->gg_strict);
    }
    if (j.contains("ida")) {
        cfg->ida.enabled = j["ida"].value("enabled", cfg->ida.enabled);
        cfg->ida.mcp_url = j["ida"].value("mcp_url", cfg->ida.mcp_url);
        cfg->ida.mcp_path = j["ida"].value("mcp_path", cfg->ida.mcp_path);
        cfg->ida.tool_name = j["ida"].value("tool_name", cfg->ida.tool_name);
        cfg->ida.arg_name = j["ida"].value("arg_name", cfg->ida.arg_name);
        cfg->ida.timeout_ms = j["ida"].value("timeout_ms", cfg->ida.timeout_ms);
    }
    return true;
}

static int run_stdio(Session& session, McpProtocol& proto) {
    (void)session;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::string resp = proto.handle(line);
        if (!resp.empty()) {
            std::cout << resp << "\n";
            std::cout.flush();
        }
    }
    return 0;
}

static int run_http(Session& session, McpProtocol& proto, const ServerConfig& cfg) {
    (void)session;
    /* 远程暴露(0.0.0.0)时必须启用 token, 否则同网段任意主机可读写设备内存/执行 shell/加载 GG。
     * 这是加能力不加限制: 远程能力保留, 但强制 bearer 鉴权。 */
    if (cfg.http_host == "0.0.0.0" && cfg.http_token.empty()) {
        std::cerr << "[GG-GHOST-MCP][FATAL] http_host=0.0.0.0 但 http_token 为空: "
                     "拒绝在不可信网络暴露。请设置 --token <secret>/http_token 配置, "
                     "或将 http_host 改为 127.0.0.1(仅本机)\n";
        return 1;
    }
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        if (!cfg.http_token.empty()) {
            std::string auth = req.get_header_value("Authorization");
            std::string expect = "Bearer " + cfg.http_token;
            if (auth != expect) {
                res.status = 401;
                res.set_content("{\"error\":\"unauthorized\"}", "application/json");
                return;
            }
        }
        std::string resp = proto.handle(req.body);
        res.set_content(resp, "application/json");
    });
    svr.Get("/healthz", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
    const std::string& host = cfg.http_host.empty() ? std::string("127.0.0.1") : cfg.http_host;
    std::cerr << "[GG-GHOST-MCP] listening on " << host << ":" << cfg.http_port
              << "/mcp (POST)"
              << (cfg.http_token.empty() ? " [no auth]" : " [bearer auth]") << "\n";
    return svr.listen(host, cfg.http_port) ? 0 : 1;
}

static void usage(const char* argv0) {
    std::cerr <<
        "GG-GHOST-MCP — PC 端 GameGuardian MCP 服务器\n"
        "用法: " << argv0 << " [--config configs/ggmcp.json] [--transport stdio|http]\n"
        "          [--port 8788] [--http-host 127.0.0.1] [--token <secret>]\n"
        "          [--adb <adb路径>] [--serial <设备序列号>]\n"
        "          [--print-events]  # 订阅 agentd 事件流并打印（悬浮窗监控调试）\n"
        "  http 模式: 默认仅绑 127.0.0.1; 绑 0.0.0.0 必须配 --token(bearer 鉴权)\n";
}

}  // namespace gg

int main(int argc, char** argv) {
    using namespace gg;

#ifdef _WIN32
    SetConsoleOutputCP(65001);   // UTF-8 控制台输出
#endif

    std::string config_path = "configs/ggmcp.json";
    ServerConfig cfg;

    // 第一遍：仅解析 --config，确定实际配置文件路径
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) { config_path = argv[++i]; break; }
    }

    // 读配置文件，命令行参数随后覆盖（参数优先）
    if (load_config(config_path, &cfg)) {
        std::cerr << "[GG-GHOST-MCP] loaded config: " << config_path << "\n";
    } else {
        std::cerr << "[GG-GHOST-MCP] no config at " << config_path
                  << " (using defaults)\n";
    }

    // 第二遍：应用其余命令行覆盖（跳过已处理的 --config）
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) { ++i; continue; }
        else if (a == "--transport" && i + 1 < argc) cfg.transport = argv[++i];
        else if (a == "--port" && i + 1 < argc) cfg.http_port = atoi(argv[++i]);
        else if (a == "--http-host" && i + 1 < argc) cfg.http_host = argv[++i];
        else if (a == "--token" && i + 1 < argc) cfg.http_token = argv[++i];
        else if (a == "--adb" && i + 1 < argc) cfg.adb = argv[++i];
        else if (a == "--serial" && i + 1 < argc) cfg.serial = argv[++i];
        else if (a == "--safe") cfg.safe_mode = true;
        else if (a == "--print-events") cfg.print_events = true;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 1; }
    }

    Session session(cfg);
    McpProtocol proto(session);
    print_banner(cfg);

    if (cfg.transport == "http") return run_http(session, proto, cfg);
    return run_stdio(session, proto);
}
