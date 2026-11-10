// fake_agentd.h — 本机假 agentd（127.0.0.1 随机端口），供 PC 侧测试驱动真实
// Session / AgentClient，无需设备、无需 adb。
//
// 两处用途：
//   test_tool_payloads.cpp  截下真正发出去的帧，断言参数映射
//   test_session.cpp        让「应当真的发到设备」的用例在本地拿到快速应答，
//                           而不是去连 34127 然后卡在 20s 的重连退避里
//
// 只实现测试需要的子集：20 字节小端头 + JSON 负载，不分片、不做 GGWF_ENC 混淆
// （客户端收到不带 ENC 的响应就不会启用混淆，链路行为与真设备一致）。
#pragma once

#include "../../server/session.h"
#include "../../shared/wire_protocol.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET fake_sock_t;
#define FAKE_INVALID INVALID_SOCKET
#define FAKE_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int fake_sock_t;
#define FAKE_INVALID (-1)
#define FAKE_CLOSE ::close
#endif

namespace fake {

inline unsigned rd32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

inline void wr16(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

inline void wr32(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

inline bool recv_all(fake_sock_t fd, char* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = ::recv(fd, buf + got, (int)(len - got), 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

inline bool send_all(fake_sock_t fd, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = ::send(fd, buf + sent, (int)(len - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

class FakeAgentd {
public:
    struct Req {
        uint16_t cmd = 0;
        gg::json params;
    };

    bool start() {
#ifdef _WIN32
        WSADATA wd;
        static bool wsa = false;
        if (!wsa && WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
        wsa = true;
#endif
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ == FAKE_INVALID) return false;
        int yes = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(0);   // 任意空闲端口：避开本机真实 agentd 的 forward
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_, (sockaddr*)&sa, sizeof(sa)) != 0) return false;
        if (::listen(listen_, 4) != 0) return false;
        sockaddr_in got{};
#ifdef _WIN32
        int glen = (int)sizeof(got);
#else
        socklen_t glen = sizeof(got);
#endif
        if (::getsockname(listen_, (sockaddr*)&got, &glen) != 0) return false;
        port_ = ntohs(got.sin_port);
        th_ = std::thread([this] { serve(); });
        return true;
    }

    void stop() {
        stop_ = true;
        /* Windows 上 closesocket(listen) 不会让阻塞中的 accept 返回（Linux 会），
         * 阻塞的 recv 同样不会自己醒 —— 两处都得主动掐断，否则 join() 永久挂住。
         * 现象很容易误判：所有断言都打印了 ok，进程却不退出。 */
        fake_sock_t cur = cur_.load();
        if (cur != FAKE_INVALID) FAKE_CLOSE(cur);
        if (listen_ != FAKE_INVALID) {
            FAKE_CLOSE(listen_);
            listen_ = FAKE_INVALID;
        }
        if (th_.joinable()) th_.join();
    }

    int port() const { return port_; }

    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        seen_.clear();
    }

    size_t count() {
        std::lock_guard<std::mutex> lk(mtx_);
        return seen_.size();
    }

    Req last() {
        std::lock_guard<std::mutex> lk(mtx_);
        return seen_.empty() ? Req{} : seen_.back();
    }

private:
    void serve() {
        while (!stop_) {
            /* select 带 50ms 超时轮询 stop_：accept 在 Windows 上无法被
             * closesocket 打断，靠它退出会卡死在 join。 */
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(listen_, &rs);
            timeval tv{0, 50 * 1000};
            if (::select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
            fake_sock_t c = ::accept(listen_, nullptr, nullptr);
            if (c == FAKE_INVALID) continue;
            cur_ = c;
            handle(c);
            cur_ = FAKE_INVALID;
            FAKE_CLOSE(c);
        }
    }

    void handle(fake_sock_t c) {
        for (;;) {
            unsigned char hdr[GGWM_HEADER_LEN];
            if (!recv_all(c, (char*)hdr, sizeof(hdr))) return;
            if (rd32(hdr) != GGWM_MAGIC) return;
            const uint16_t cmd = (uint16_t)(hdr[6] | (hdr[7] << 8));
            const uint32_t seq = rd32(hdr + 8);
            const uint32_t plen = rd32(hdr + 16);
            if (plen > GGWM_MAX_PAYLOAD) return;
            std::string payload(plen, '\0');
            if (plen && !recv_all(c, &payload[0], plen)) return;

            if (cmd != GGCMD_AUTH) {
                std::lock_guard<std::mutex> lk(mtx_);
                Req r;
                r.cmd = cmd;
                try {
                    gg::json p = gg::json::parse(payload);
                    r.params = p.contains(GG_J_REQ) ? p[GG_J_REQ] : p;
                } catch (...) {
                    r.params = gg::json::object();
                }
                seen_.push_back(r);
            }
            if (getenv("GG_TEST_PAYLOAD_TRACE"))
                std::fprintf(stderr, "[fake] cmd=%u seq=%u flags=0x%x len=%u\n",
                             (unsigned)cmd, seq, rd32(hdr + 12), plen);

            /* 万能成功负载：调用方只关心"发出去了什么"，回包只要让 handler
             * 不炸即可（各 handler 用 value() 取字段，缺失走默认值）。 */
            static const char* kReply =
                "{\"ok\":true,\"count\":0,\"remaining\":0,\"saved\":0,\"bytes\":0,"
                "\"addr\":\"0x0\",\"hits\":[],\"regions\":[],\"diffs\":[],\"dumps\":[],"
                "\"results\":[],\"modules\":[],\"processes\":[],\"value\":\"0x0\"}";
            unsigned char out[GGWM_HEADER_LEN];
            wr32(out, GGWM_MAGIC);
            wr16(out + 4, GGWM_VERSION);
            wr16(out + 6, cmd);
            wr32(out + 8, seq);
            wr32(out + 12, GGWF_RESP);
            wr32(out + 16, (unsigned)strlen(kReply));
            if (!send_all(c, (char*)out, sizeof(out))) return;
            if (!send_all(c, kReply, strlen(kReply))) return;
        }
    }

    fake_sock_t listen_ = FAKE_INVALID;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<fake_sock_t> cur_{FAKE_INVALID};
    std::thread th_;
    std::mutex mtx_;
    std::vector<Req> seen_;
};

}  // namespace fake
