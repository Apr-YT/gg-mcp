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
#include <cstdlib>

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

// 配置加载结果：文件缺失可以用默认值继续跑，但字段存在却类型/范围不合法
// 必须直接失败 —— 静默回落默认值等于把「配错了」变成「配了个别的」。
enum LoadResult { LOAD_OK = 0, LOAD_MISSING = 1, LOAD_INVALID = 2 };

static LoadResult load_config(const std::string& path, ServerConfig* cfg) {
    std::ifstream f(path);
    if (!f.is_open()) return LOAD_MISSING;
    json j;
    try {
        f >> j;
    } catch (...) {
        std::cerr << "[GG-GHOST-MCP][FATAL] config is not valid JSON: " << path << "\n";
        return LOAD_INVALID;
    }
    if (!j.is_object()) {
        std::cerr << "[GG-GHOST-MCP][FATAL] config root must be an object: " << path << "\n";
        return LOAD_INVALID;
    }
    try {
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
        cfg->export_dir = j.value("export_dir", cfg->export_dir);
        cfg->max_save_bytes = j.value("max_save_bytes", (int64_t)cfg->max_save_bytes);
        if (j.contains("gg")) {
            if (!j["gg"].is_object()) {
                std::cerr << "[GG-GHOST-MCP][FATAL] config.gg must be an object\n";
                return LOAD_INVALID;
            }
            cfg->gg_version_pattern = j["gg"].value("version_pattern", cfg->gg_version_pattern);
            cfg->gg_strict = j["gg"].value("strict", cfg->gg_strict);
        }
    } catch (const std::exception& e) {
        std::cerr << "[GG-GHOST-MCP][FATAL] bad config field: " << e.what() << "\n";
        return LOAD_INVALID;
    }
    if (cfg->http_port <= 0 || cfg->http_port > 65535) {
        std::cerr << "[GG-GHOST-MCP][FATAL] http_port out of range: " << cfg->http_port << "\n";
        return LOAD_INVALID;
    }
    if (cfg->agent_port <= 0 || cfg->agent_port > 65535) {
        std::cerr << "[GG-GHOST-MCP][FATAL] agent_port out of range: " << cfg->agent_port << "\n";
        return LOAD_INVALID;
    }
    if (cfg->max_save_bytes == 0) {
        std::cerr << "[GG-GHOST-MCP][FATAL] max_save_bytes must be > 0\n";
        return LOAD_INVALID;
    }
    return LOAD_OK;
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
    switch (load_config(config_path, &cfg)) {
        case LOAD_OK:
            std::cerr << "[GG-GHOST-MCP] loaded config: " << config_path << "\n";
            break;
        case LOAD_MISSING:
            std::cerr << "[GG-GHOST-MCP] no config at " << config_path
                      << " (using defaults)\n";
            break;
        case LOAD_INVALID:
        default:
            return 1;   // 已在 load_config 里打印具体原因
    }

    // 第二遍：应用其余命令行覆盖（跳过已处理的 --config）
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) { ++i; continue; }
        else if (a == "--transport" && i + 1 < argc) cfg.transport = argv[++i];
        else if (a == "--port" && i + 1 < argc) {
            const char* v = argv[++i];
            char* end = nullptr;
            long p = strtol(v, &end, 10);
            if (end == v || *end != '\0' || p <= 0 || p > 65535) {
                std::cerr << "[GG-GHOST-MCP][FATAL] --port must be 1..65535, got: " << v << "\n";
                return 1;
            }
            cfg.http_port = static_cast<int>(p);
        }
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
