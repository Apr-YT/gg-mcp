// wire.h — PC↔设备通信：二进制帧编解码 + AgentClient
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "../../shared/wire_protocol.h"
#include "../../third_party/json.hpp"

namespace gg {

using json = nlohmann::json;

// 单次调用的失败语义。GGERR_NO_DEVICE 把「连不上 / 发不出去 / 等不到响应 /
// 收到烂帧」四件事压成了一个码，上层无法判断重放是否安全，因此这里分开记录。
// 仅用于 PC 端内部判定与日志，不改变对外公开的 GGERR_* 数值。
enum class CallOutcome {
    Ok = 0,
    ConnectFailed,    // 连接未建立，请求一个字节都没发出去 —— 重放无副作用
    SendFailed,       // 发送中途失败，远端可能已收到完整请求
    ResponseTimeout,  // 请求已完整发出，截止时间内没有拿到匹配响应
    OutcomeUnknown,   // 读中途断连/对端 RST，无法判断远端是否执行
    ProtocolError,    // 收到非法帧（magic/ver/长度），连接状态已不可信
    RemoteError,      // 远端明确返回失败码
};

// 与 gg-agentd 的单连接客户端（含 adb forward 管理）
class AgentClient {
public:
    struct Config {
        std::string adb = "adb";        // adb 可执行路径
        std::string serial;             // 设备序列号（空=单设备）
        int agent_port = 34127;         // 设备端 agentd 端口（hide 改造 2026-08-18）
        int connect_timeout_ms = 5000;
        int io_timeout_ms = 300000;     // 搜索可能较久
        // wire 握手密钥：必须与 agentd 的 --wire-key / --wire-key-file 一致。
        // 为空时仍会发起握手，由 agentd 决定放行（无密钥的 agentd 会回 auth_disabled）。
        std::string wire_key;
    };

    explicit AgentClient(Config cfg) : cfg_(std::move(cfg)) {}

    // 确保连接可用（必要时重跑 adb forward + 重连）。返回错误码（GGERR_*）或 0。
    int ensure_connected();
    void close();

    // 发请求并等待响应。返回 0 成功（out 为 {"r":...}），失败返回**正的** GGERR_*（枚举本身即 1..14）。
    // 注意不是负值：调用方一律用 rc != 0 判定，若照旧注释写成 rc < 0 会恒假。
    // 失败时 last_outcome 给出细分语义：只有 ConnectFailed 允许上层安全重放。
    int call(uint16_t cmd, const json& params, json* out);

    std::string last_error;
    CallOutcome last_outcome = CallOutcome::Ok;

private:
    using Clock = std::chrono::steady_clock;

    int ensure_connected_locked();   // 前提: mtx_ 已持有
    int tcp_connect();               // 单次 TCP 连接（不跑 adb）
    int auth_locked(Clock::time_point deadline);   // 连接后握手（GGCMD_AUTH）
    // 返回值：0 成功；-1 IO 错误/对端关闭；-2 截止时间到期；-3 协议帧非法
    int read_frame(int fd, gg_wire_header_t* h, std::string* payload, Clock::time_point deadline);
    /* 读一个完整响应：自动拼装 GGWF_MORE 分片。非分片帧行为与 read_frame 一致。 */
    int read_full_frame(int fd, gg_wire_header_t* h, std::string* payload, Clock::time_point deadline);
    int write_frame(int fd, uint16_t cmd, uint32_t seq, const std::string& payload, Clock::time_point deadline);

    Config cfg_;
    int fd_ = -1;
    uint32_t seq_ = 0;
    std::mutex mtx_;
    bool enc_on_ = false;   /* 自适应混淆: 收到 GGWF_ENC 响应后, 请求也加密 (兼容旧 agentd) */
};

}  // namespace gg
