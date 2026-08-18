// wire.h — PC↔设备通信：二进制帧编解码 + AgentClient
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "../../shared/wire_protocol.h"
#include "../../third_party/json.hpp"

namespace gg {

using json = nlohmann::json;

// 与 gg-agentd 的单连接客户端（含 adb forward 管理）
class AgentClient {
public:
    struct Config {
        std::string adb = "adb";        // adb 可执行路径
        std::string serial;             // 设备序列号（空=单设备）
        int agent_port = 34127;         // 设备端 agentd 端口（hide 改造 2026-08-18）
        int connect_timeout_ms = 5000;
        int io_timeout_ms = 300000;     // 搜索可能较久
        bool print_events = false;      // 订阅并打印 agentd 事件帧（调试/监控用）
    };

    explicit AgentClient(Config cfg) : cfg_(std::move(cfg)) {}

    // 确保连接可用（必要时重跑 adb forward + 重连）。返回错误码（GGERR_*）或 0。
    int ensure_connected();
    void close();

    // 发请求并等待响应。返回 0 成功（out 为 {"r":...}）；负值为 GGERR_*。
    int call(uint16_t cmd, const json& params, json* out);

    std::string last_error;

private:
    int ensure_connected_locked();   // 前提: mtx_ 已持有
    int tcp_connect();               // 单次 TCP 连接（不跑 adb）
    int read_frame(int fd, gg_wire_header_t* h, std::string* payload);
    int write_frame(int fd, uint16_t cmd, uint32_t seq, const std::string& payload);

    Config cfg_;
    int fd_ = -1;
    uint32_t seq_ = 0;
    std::mutex mtx_;
};

}  // namespace gg
