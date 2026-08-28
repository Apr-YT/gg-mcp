// wire.cpp — 帧编解码 + AgentClient 实现
#include "wire.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace gg {

#ifdef _WIN32
using sock_t = SOCKET;
#define GG_CLOSE_SOCK(s) closesocket(s)
static bool g_wsa = false;
static bool wsa_init() {
    if (g_wsa) return true;
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
    g_wsa = true;
    return true;
}
#else
using sock_t = int;
#define GG_CLOSE_SOCK(s) ::close(s)
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif
static bool wsa_init() { return true; }
#endif

// EINTR 安全判断：recv/send 被信号中断时应重试，而非误判断连
#ifdef _WIN32
static inline bool gg_is_eintr() { return WSAGetLastError() == WSAEINTR; }
#else
static inline bool gg_is_eintr() { return errno == EINTR; }
#endif

// ---- 日志级别（O1）：默认 INFO(2)，GG_LOG_LEVEL 环境变量可调 ----
// 0=ERROR 1=WARN 2=INFO 3=DEBUG；高频 hex dump 默认不打印，DEBUG 模式才全量
static int g_gg_log_level = 2;
static void gg_log_init() {
    static bool inited = false;
    if (inited) return;
    inited = true;
    const char* e = getenv("GG_LOG_LEVEL");
    if (e) {
        int v = atoi(e);
        if (v >= 0 && v <= 3) g_gg_log_level = v;
    }
}
#define GG_LOG(level, ...) do { \
    gg_log_init(); \
    if ((level) <= g_gg_log_level) { fprintf(stderr, __VA_ARGS__); } \
} while (0)

// ---- 小工具 ----
static std::string run_cmd(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

int AgentClient::ensure_connected() {
    std::lock_guard<std::mutex> lk(mtx_);
    return ensure_connected_locked();
}

// 调用方必须已持有 mtx_
int AgentClient::ensure_connected_locked() {
    if (fd_ >= 0) return 0;

    // 先尝试直连（forward 通常已由部署脚本/上次会话建立）。
    // 只有直连失败才执行 adb forward（Windows 下反复 popen adb forward 会打断现有监听）。
    for (int attempt = 0; attempt < 2; attempt++) {
        int rc = tcp_connect();
        if (rc == 0) {
            seq_ = 0;
            if (cfg_.print_events) {
                // 订阅事件流（手机悬浮窗同款）；事件帧由 call() 读取时跳过并打印
                json req{{GG_J_REQ, json::object()}};
                std::string pl = req.dump();
                write_frame(fd_, GGCMD_SUBSCRIBE_EVENTS, ++seq_, pl);
            }
            return 0;
        }
        if (attempt == 0) {
            // 注入防御: adb 路径与 serial 拼入 popen 命令行，白名单校验。
            // adb 含空格时须带引号；serial 只放行安全字符集（拦截 ; & | 等元字符）。
            auto adb_safe = [](const std::string& s) {
                return !s.empty() && s.find_first_of(";&|`$<>()\"'\\") == std::string::npos;
            };
            auto serial_safe = [](const std::string& s) {
                for (char ch : s)
                    if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == ':' || ch == '_'))
                        return false;
                return !s.empty();
            };
            std::string adb_bin = cfg_.adb;
            if (!adb_safe(adb_bin)) { last_error = "adb path rejected (unsafe chars): " + adb_bin; return GGERR_NO_DEVICE; }
            if (!serial_safe(cfg_.serial)) { last_error = "serial rejected (unsafe chars): " + cfg_.serial; return GGERR_NO_DEVICE; }
            std::string cmd = adb_bin + " forward tcp:" + std::to_string(cfg_.agent_port) +
                              " tcp:" + std::to_string(cfg_.agent_port);
            if (!cfg_.serial.empty()) cmd = adb_bin + " -s " + cfg_.serial +
                                           " forward tcp:" + std::to_string(cfg_.agent_port) +
                                           " tcp:" + std::to_string(cfg_.agent_port);
            run_cmd(cmd);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
    }
    last_error = "connect 127.0.0.1:" + std::to_string(cfg_.agent_port) +
                 " failed (is gg-agentd running? adb forward ok?)";
    return GGERR_NO_DEVICE;
}

// 建立一次 TCP 连接（不跑 adb）。成功返回 0 并置 fd_。
int AgentClient::tcp_connect() {
    if (!wsa_init()) { last_error = "WSAStartup failed"; return -1; }
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) { last_error = "socket() failed"; return -1; }

#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);   // 非阻塞 + select 实现超时
#endif

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(cfg_.agent_port));
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
#ifdef _WIN32
    if (rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        timeval tv{};
        tv.tv_sec = cfg_.connect_timeout_ms / 1000;
        tv.tv_usec = (cfg_.connect_timeout_ms % 1000) * 1000;
        rc = select(0, nullptr, &w, nullptr, &tv);
        if (rc > 0) rc = 0;
        else rc = -1;
    }
    if (rc == 0) {
        u_long mode0 = 0;
        ioctlsocket(fd, FIONBIO, &mode0);   // 恢复阻塞
        // ⚠️ Winsock 的 SO_RCVTIMEO/SO_SNDTIMEO 是 DWORD 毫秒值，不是 timeval！
        // 之前传 timeval{300, 0} → Winsock 只取前 4 字节 = 300 → 实际 300ms 超时，
        // 慢调用（如 gg_core_versions ~600ms）读响应必超时 WSAETIMEDOUT(10060)。
        DWORD io_ms = static_cast<DWORD>(cfg_.io_timeout_ms);
        GG_LOG(2, "[wire] timeout cfg: io=%dms (winsock DWORD ms)\n", (int)io_ms);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&io_ms), sizeof(io_ms));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&io_ms), sizeof(io_ms));
    }
#else
    // POSIX：非阻塞 connect + poll 超时；之后 SO_RCVTIMEO/SO_SNDTIMEO 控制 recv/send
#ifndef EINPROGRESS
#define EINPROGRESS 115
#endif
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (rc != 0 && errno != EINPROGRESS) {
        rc = -1;
    } else {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int pr = poll(&pfd, 1, cfg_.connect_timeout_ms);
        if (pr <= 0) {
            rc = -1;   // 超时或 poll 错误
        } else {
            int sockerr = 0;
            socklen_t sl = sizeof(sockerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &sl) != 0 || sockerr != 0)
                rc = -1;   // 连接被拒/失败
            else
                rc = 0;
        }
        fcntl(fd, F_SETFL, flags);   // 恢复阻塞模式
        if (rc == 0) {
            // POSIX 的 SO_RCVTIMEO/SO_SNDTIMEO 用 timeval（秒+微秒）
            struct timeval tv{};
            tv.tv_sec = cfg_.io_timeout_ms / 1000;
            tv.tv_usec = (cfg_.io_timeout_ms % 1000) * 1000;
            GG_LOG(2, "[wire] timeout cfg: io=%dms (posix timeval)\n", (int)cfg_.io_timeout_ms);
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
    }
#endif

    if (rc != 0) {
        GG_LOG(1, "[wire] connect failed: rc=%d err=%d\n", rc,
#ifdef _WIN32
                (int)WSAGetLastError()
#else
                errno
#endif
        );
        GG_CLOSE_SOCK(fd);
        return -1;
    }
    fd_ = static_cast<int>(fd);
    return 0;
}

void AgentClient::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fd_ >= 0) { GG_CLOSE_SOCK(static_cast<sock_t>(fd_)); fd_ = -1; }
}

int AgentClient::write_frame(int fd, uint16_t cmd, uint32_t seq, const std::string& payload) {
    gg_wire_header_t h{};
    h.magic = GGWM_MAGIC;
    h.ver = GGWM_VERSION;
    h.cmd = cmd;
    h.seq = seq;
    /* 请求负载混淆: 收到过 GGWF_ENC 响应后启用（自适应, 兼容旧 agentd 明文请求）。
     * 加密在发头之前完成——头里的 flags/payload_len 必须与实际发送内容一致。 */
    h.flags = GGWF_REQ;
    h.payload_len = static_cast<uint32_t>(payload.size());
    std::string enc = payload;
    if (enc_on_) {
        gg_xor_payload(reinterpret_cast<unsigned char*>(&enc[0]), enc.size());
        h.flags |= GGWF_ENC;
        h.payload_len = static_cast<uint32_t>(enc.size());
    }
    size_t off = 0;
    const char* p = reinterpret_cast<const char*>(&h);
    while (off < sizeof(h)) {
        int n = ::send(fd, p + off, static_cast<int>(sizeof(h) - off), 0);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && gg_is_eintr()) continue;   // 信号中断重试
        return -1;
    }
    {
        char hex[64] = {0};
        for (size_t i = 0; i < sizeof(h); i++) snprintf(hex + i * 2, 3, "%02x", (unsigned char)p[i]);
        GG_LOG(3, "[wire] sent hdr[%zu]: %s cmd=%d seq=%u flags=%u plen=%zu\n",
                sizeof(h), hex, h.cmd, h.seq, h.flags, enc.size());
    }
    off = 0;
    while (off < enc.size()) {
        int n = ::send(fd, enc.data() + off, static_cast<int>(enc.size() - off), 0);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && gg_is_eintr()) continue;
        return -1;
    }
    return 0;
}

int AgentClient::read_frame(int fd, gg_wire_header_t* h, std::string* payload) {
    size_t off = 0;
    char* p = reinterpret_cast<char*>(h);
    while (off < sizeof(gg_wire_header_t)) {
        int n = ::recv(fd, p + off, static_cast<int>(sizeof(gg_wire_header_t) - off), 0);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && gg_is_eintr()) continue;
        return -1;
    }
    if (h->magic != GGWM_MAGIC || h->ver != GGWM_VERSION) return -1;
    if (h->payload_len > GGWM_MAX_PAYLOAD) return -1;
    payload->resize(h->payload_len);
    off = 0;
    while (off < payload->size()) {
        int n = ::recv(fd, payload->data() + off, static_cast<int>(payload->size() - off), 0);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && gg_is_eintr()) continue;
        return -1;
    }
    /* 响应负载解密（agentd 统一置 GGWF_ENC; 兼容未加密的旧 agentd） */
    if ((h->flags & GGWF_ENC) && h->payload_len) {
        gg_xor_payload(reinterpret_cast<unsigned char*>(&(*payload)[0]), h->payload_len);
        enc_on_ = true;   /* 握手: agentd 支持混淆 → 后续请求也加密 */
    }
    return 0;
}

int AgentClient::call(uint16_t cmd, const json& params, json* out) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fd_ < 0) {
        if (ensure_connected_locked() != 0) return GGERR_NO_DEVICE;
    }
    json req;
    req[GG_J_REQ] = params;
    std::string payload = req.dump();

    uint32_t seq = ++seq_;
    if (write_frame(fd_, cmd, seq, payload) < 0) {
        GG_CLOSE_SOCK(static_cast<sock_t>(fd_));
        fd_ = -1;
        last_error = "write to agentd failed (device offline?)";
        return GGERR_NO_DEVICE;
    }
    gg_wire_header_t h{};
    std::string resp;
    for (;;) {
        if (read_frame(fd_, &h, &resp) < 0) {
            GG_LOG(1, "[wire] read_frame failed: fd=%d err=%d t=%lldms\n", fd_,
#ifdef _WIN32
                    (int)WSAGetLastError()
#else
                    errno
#endif
                    , (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count());
            GG_CLOSE_SOCK(static_cast<sock_t>(fd_));
            fd_ = -1;
            last_error = "read from agentd failed (device offline?)";
            return GGERR_NO_DEVICE;
        }
        if (h.flags & GGWF_EVENT) {
            // 订阅后事件帧会穿插在响应流里：打印并跳过，继续等本请求的响应
            GG_LOG(2, "[event] %s\n", resp.c_str());
            continue;
        }
        if (h.seq != seq) {
            // 订阅响应等杂散帧（seq 不匹配）：跳过，避免错配
            GG_LOG(3, "[wire] skip stray frame cmd=%u seq=%u (want %u)\n",
                    (unsigned)h.cmd, (unsigned)h.seq, (unsigned)seq);
            continue;
        }
        break;
    }
    if (h.flags & GGWF_ERR) {
        try {
            json e = json::parse(resp);
            int code = GGERR_INTERNAL;
            std::string msg = "error";
            if (e.contains("e")) {
                if (e["e"].contains("code")) code = e["e"]["code"].get<int>();
                if (e["e"].contains("message")) msg = e["e"]["message"].get<std::string>();
            }
            last_error = msg;
            return code;
        } catch (...) {
            return GGERR_INTERNAL;
        }
    }
    try {
        json j = json::parse(resp);
        if (j.contains("r")) {
            *out = j["r"];
            return 0;
        }
        *out = j;
        return 0;
    } catch (...) {
        last_error = "bad response from agentd";
        return GGERR_INTERNAL;
    }
}

}  // namespace gg
