#include <iostream>
#include <string>
#include <mutex>
#include "httplib.h"

using namespace httplib;

struct Stats {
    std::mutex mutex;
    int read_count;
    int write_count;
    int search_count;
    
    Stats() : read_count(0), write_count(0), search_count(0) {}
    
    std::string toJson() {
        std::lock_guard<std::mutex> lock(mutex);
        return "{\"read_count\":" + std::to_string(read_count) + 
               ",\"write_count\":" + std::to_string(write_count) + 
               ",\"search_count\":" + std::to_string(search_count) + "}";
    }
};

Stats g_stats;

void setupRoutes(Server& svr) {
    svr.Get("/health", [](const Request&, Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });
    
    svr.Get("/api/tools", [](const Request&, Response& res) {
        std::string tools = R"({"tools":[)";
        tools += R"({"name":"read_memory","description":"读取内存"},)";
        tools += R"({"name":"write_memory","description":"写入内存"},)";
        tools += R"({"name":"search_memory","description":"搜索内存"})";
        tools += "]}";
        res.set_content(tools, "application/json");
    });
    
    svr.Post("/api/tools/call", [](const Request&, Response& res) {
        res.set_content(R"({"success":true,"data":"simulated"})", "application/json");
    });
    
    svr.Get("/api/stats", [](const Request&, Response& res) {
        res.set_content("{" + g_stats.toJson() + "}", "application/json");
    });
    
    svr.set_mount_point("/", "../web");
    svr.Get("/", [](const Request&, Response& res) {
        res.set_redirect("/index.html");
    });
}

int main() {
    Server svr;
    setupRoutes(svr);
    
    std::cout << "GG-MCP Proxy Server: http://127.0.0.1:8789" << std::endl;
    svr.listen("127.0.0.1", 8789);
    return 0;
}