// test_wire_io.cpp — AgentClient 失败语义回归用例（真 socket，不 mock）
//
// 做法：在本机 127.0.0.1 起一个临时监听端口当作假 agentd，让真实的
// AgentClient::call() 走完整的 write_frame/read_frame 路径；假对端按用例
// 制造「不响应 / 半帧 / 非法帧」。断言只看 call() 的返回值和 last_outcome，
// 这两项正是上层判断「能否安全重放」的唯一依据。
//
// 覆盖：
//   0) 正常响应 → 0 / Ok（保证用例不是恒真：假对端确实驱动了真实收发路径）
//   a) 对端不响应 → 非 0 / ResponseTimeout，且整体耗时受截止时间约束
//   b) 半帧（头部不足 / 头部完整但负载不足）后对端关闭 → 非 0 / OutcomeUnknown
//   c) 非法帧（magic 错 / ver 错）→ 非 0 / ProtocolError
//
// 注意：call() 的失败返回值是正的 GGERR_*（GGERR_TIMEOUT=7 等），不是负值
// （负值只存在于内部的 read_frame/write_frame）。所以这里断言具体 GGERR_* 码，
// 不写 rc < 0 —— 那会恒假。
#include "../../server/device/wire.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET tsock_t;
#define TSOCK_INVALID INVALID_SOCKET
#define TSOCK_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int tsock_t;
#define TSOCK_INVALID (-1)
#define TSOCK_CLOSE ::close
#endif

namespace {

bool net_init() {
#ifdef _WIN32
    static bool inited = false;
    if (inited) return true;
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
    inited = true;
#endif
    return true;
}

// 只监听 127.0.0.1 的临时端口，端口号由内核分配。
struct Peer {
    tsock_t lfd = TSOCK_INVALID;
    int port = 0;

    bool listen_loopback() {
        if (!net_init()) return false;
        lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (lfd == TSOCK_INVALID) return false;
        int on = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = 0;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(lfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) return false;
        if (::listen(lfd, 1) != 0) return false;
        sockaddr_in got{};
        socklen_t gl = sizeof(got);
        if (getsockname(lfd, reinterpret_cast<sockaddr*>(&got), &gl) != 0) return false;
        port = ntohs(got.sin_port);
        return port > 0;
    }

    tsock_t accept_one() {
        tsock_t c = ::accept(lfd, nullptr, nullptr);
        return c;
    }

    ~Peer() {
        if (lfd != TSOCK_INVALID) TSOCK_CLOSE(lfd);
    }
};

bool recv_exact(tsock_t c, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    size_t off = 0;
    while (off < n) {
        int r = ::recv(c, p + off, static_cast<int>(n - off), 0);
        if (r <= 0) return false;
        off += static_cast<size_t>(r);
    }
    return true;
}

bool send_all(tsock_t c, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    size_t off = 0;
    while (off < n) {
        int r = ::send(c, p + off, static_cast<int>(n - off), 0);
        if (r <= 0) return false;
        off += static_cast<size_t>(r);
    }
    return true;
}

// 读完整个请求帧（头 + 负载），主要是为了拿到 seq 并且让客户端 send 干净收尾。
bool read_request(tsock_t c, gg_wire_header_t* h, std::string* payload) {
    if (!recv_exact(c, h, sizeof(*h))) return false;
    if (h->payload_len > GGWM_MAX_PAYLOAD) return false;
    payload->resize(h->payload_len);
    if (h->payload_len == 0) return true;
    return recv_exact(c, &(*payload)[0], payload->size());
}

bool send_response(tsock_t c, uint16_t cmd, uint32_t seq, const std::string& body, uint32_t flags) {
    gg_wire_header_t rh{};
    rh.magic = GGWM_MAGIC;
    rh.ver = GGWM_VERSION;
    rh.cmd = cmd;
    rh.seq = seq;
    rh.flags = flags;
    rh.payload_len = static_cast<uint32_t>(body.size());
    if (!send_all(c, &rh, sizeof(rh))) return false;
    return body.empty() || send_all(c, body.data(), body.size());
}

/* 握手：客户端建立连接后第一帧必然是 GGCMD_AUTH（未认证只放行 PING/AUTH）。
 * 每个用例必须先把它应答掉，否则业务帧还没轮到就已经 auth 超时。
 * 协议加鉴权后这是硬前提，漏掉会让整个文件恒失败。 */
bool serve_auth(tsock_t c) {
    gg_wire_header_t req{};
    std::string pl;
    if (!read_request(c, &req, &pl)) return false;
    if (req.cmd != GGCMD_AUTH) return false;
    return send_response(c, req.cmd, req.seq, "{\"r\":{\"authed\":true}}", GGWF_RESP);
}

// 假对端的客户端参数：指向临时端口，io 预算调小以便用例快速收敛。
gg::AgentClient::Config peer_cfg(int port, int io_ms) {
    gg::AgentClient::Config cfg;
    cfg.agent_port = port;
    cfg.io_timeout_ms = io_ms;
    cfg.adb = "__no_adb__";   // 直连能成功时根本不会用到 adb
    return cfg;
}

}  // namespace

int main() {
    using json = nlohmann::json;
    int fails = 0;
    auto check = [&](bool c, const char* m) {
        if (!c) { std::printf("FAIL: %s\n", m); fails++; }
        else std::printf("ok  : %s\n", m);
    };

    // ---- 0) 正常响应：证明假对端确实驱动了真实收发路径 ----
    {
        Peer peer;
        check(peer.listen_loopback(), "0) fake peer listening on 127.0.0.1");
        gg::AgentClient cli(peer_cfg(peer.port, 1500));
        std::thread th([&] {
            tsock_t c = peer.accept_one();
            if (c == TSOCK_INVALID) return;
            if (!serve_auth(c)) { TSOCK_CLOSE(c); return; }   // 协议要求：先握手才轮到业务帧
            gg_wire_header_t req{};
            std::string pl;
            if (read_request(c, &req, &pl)) {
                std::string body = "{\"r\":{\"pong\":1}}";
                gg_wire_header_t rh{};
                rh.magic = GGWM_MAGIC;
                rh.ver = GGWM_VERSION;
                rh.cmd = req.cmd;
                rh.seq = req.seq;
                rh.flags = GGWF_RESP;
                rh.payload_len = static_cast<uint32_t>(body.size());
                if (send_all(c, &rh, sizeof(rh))) send_all(c, body.data(), body.size());
            }
            TSOCK_CLOSE(c);
        });
        json out;
        int rc = cli.call(GGCMD_PING, json::object(), &out);
        check(rc == 0, "0) valid response -> call() == 0");
        check(out.is_object() && out.value("pong", 0) == 1, "0) valid response -> payload decoded");
        check(cli.last_outcome == gg::CallOutcome::Ok, "0) valid response -> last_outcome == Ok");
        if (th.joinable()) th.join();
    }

    // ---- a) 对端读完请求后不返回任何字节：整体截止时间到期 ----
    {
        const int kIoMs = 300;
        Peer peer;
        check(peer.listen_loopback(), "a) fake peer listening on 127.0.0.1");
        gg::AgentClient cli(peer_cfg(peer.port, kIoMs));
        std::thread th([&] {
            tsock_t c = peer.accept_one();
            if (c == TSOCK_INVALID) return;
            if (!serve_auth(c)) { TSOCK_CLOSE(c); return; }   // 协议要求：先握手才轮到业务帧
            gg_wire_header_t req{};
            std::string pl;
            read_request(c, &req, &pl);          // 请求收全，但一个响应字节都不发
            std::this_thread::sleep_for(std::chrono::milliseconds(900));
            TSOCK_CLOSE(c);
        });
        auto t0 = std::chrono::steady_clock::now();
        json out;
        int rc = cli.call(GGCMD_PING, json::object(), &out);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0).count();
        std::printf("     [a] rc=%d elapsed=%lldms outcome=%d\n", rc, (long long)elapsed_ms,
                    static_cast<int>(cli.last_outcome));
        check(rc == GGERR_TIMEOUT, "a) no response -> call() == GGERR_TIMEOUT(7)");
        check(cli.last_outcome == gg::CallOutcome::ResponseTimeout,
              "a) no response -> last_outcome == ResponseTimeout");
        check(elapsed_ms < 5000, "a) deadline bounds wall time (< 5s)");
        if (th.joinable()) th.join();
    }

    // ---- b1) 半帧：只发出头部的一部分就关闭 ----
    {
        Peer peer;
        check(peer.listen_loopback(), "b1) fake peer listening on 127.0.0.1");
        gg::AgentClient cli(peer_cfg(peer.port, 1500));
        std::thread th([&] {
            tsock_t c = peer.accept_one();
            if (c == TSOCK_INVALID) return;
            if (!serve_auth(c)) { TSOCK_CLOSE(c); return; }   // 协议要求：先握手才轮到业务帧
            gg_wire_header_t req{};
            std::string pl;
            read_request(c, &req, &pl);
            unsigned char part[6];
            std::memset(part, 0, sizeof(part));
            part[0] = 0x43; part[1] = 0x4D; part[2] = 0x47; part[3] = 0x47;   // "GGMC" 低字节序
            send_all(c, part, sizeof(part));   // 20 字节头只给了 6 字节
            TSOCK_CLOSE(c);
        });
        json out;
        int rc = cli.call(GGCMD_PING, json::object(), &out);
        std::printf("     [b1] rc=%d outcome=%d\n", rc, static_cast<int>(cli.last_outcome));
        check(rc == GGERR_NO_DEVICE, "b1) truncated header -> call() == GGERR_NO_DEVICE(2)");
        check(cli.last_outcome == gg::CallOutcome::OutcomeUnknown,
              "b1) truncated header -> last_outcome == OutcomeUnknown (peer closed mid-frame)");
        if (th.joinable()) th.join();
    }

    // ---- b2) 半帧：头部完整但负载不足就关闭 ----
    {
        Peer peer;
        check(peer.listen_loopback(), "b2) fake peer listening on 127.0.0.1");
        gg::AgentClient cli(peer_cfg(peer.port, 1500));
        std::thread th([&] {
            tsock_t c = peer.accept_one();
            if (c == TSOCK_INVALID) return;
            if (!serve_auth(c)) { TSOCK_CLOSE(c); return; }   // 协议要求：先握手才轮到业务帧
            gg_wire_header_t req{};
            std::string pl;
            read_request(c, &req, &pl);
            gg_wire_header_t rh{};
            rh.magic = GGWM_MAGIC;
            rh.ver = GGWM_VERSION;
            rh.cmd = req.cmd;
            rh.seq = req.seq;
            rh.flags = GGWF_RESP;
            rh.payload_len = 64;                            // 声称 64 字节
            send_all(c, &rh, sizeof(rh));
            send_all(c, "{\"r\":{\"a\"", 8);                 // 只给 8 字节
            TSOCK_CLOSE(c);
        });
        json out;
        int rc = cli.call(GGCMD_PING, json::object(), &out);
        std::printf("     [b2] rc=%d outcome=%d\n", rc, static_cast<int>(cli.last_outcome));
        check(rc == GGERR_NO_DEVICE, "b2) short payload -> call() == GGERR_NO_DEVICE(2)");
        check(cli.last_outcome == gg::CallOutcome::OutcomeUnknown,
              "b2) short payload -> last_outcome == OutcomeUnknown (peer closed mid-frame)");
        if (th.joinable()) th.join();
    }

    // ---- c1) 非法帧：magic 错误 ----
    {
        Peer peer;
        check(peer.listen_loopback(), "c1) fake peer listening on 127.0.0.1");
        gg::AgentClient cli(peer_cfg(peer.port, 1500));
        std::thread th([&] {
            tsock_t c = peer.accept_one();
            if (c == TSOCK_INVALID) return;
            if (!serve_auth(c)) { TSOCK_CLOSE(c); return; }   // 协议要求：先握手才轮到业务帧
            gg_wire_header_t req{};
            std::string pl;
            read_request(c, &req, &pl);
            gg_wire_header_t rh{};
            rh.magic = 0xDEADBEEFu;   // 非法 magic
            rh.ver = GGWM_VERSION;
            rh.cmd = req.cmd;
            rh.seq = req.seq;
            rh.flags = GGWF_RESP;
            rh.payload_len = 0;
            send_all(c, &rh, sizeof(rh));
            TSOCK_CLOSE(c);
        });
        json out;
        int rc = cli.call(GGCMD_PING, json::object(), &out);
        std::printf("     [c1] rc=%d outcome=%d\n", rc, static_cast<int>(cli.last_outcome));
        check(rc == GGERR_INTERNAL, "c1) bad magic -> call() == GGERR_INTERNAL(12)");
        check(cli.last_outcome == gg::CallOutcome::ProtocolError,
              "c1) bad magic -> last_outcome == ProtocolError");
        if (th.joinable()) th.join();
    }

    // ---- c2) 非法帧：版本号错误 ----
    {
        Peer peer;
        check(peer.listen_loopback(), "c2) fake peer listening on 127.0.0.1");
        gg::AgentClient cli(peer_cfg(peer.port, 1500));
        std::thread th([&] {
            tsock_t c = peer.accept_one();
            if (c == TSOCK_INVALID) return;
            if (!serve_auth(c)) { TSOCK_CLOSE(c); return; }   // 协议要求：先握手才轮到业务帧
            gg_wire_header_t req{};
            std::string pl;
            read_request(c, &req, &pl);
            gg_wire_header_t rh{};
            rh.magic = GGWM_MAGIC;
            rh.ver = static_cast<uint16_t>(GGWM_VERSION + 1);   // 非法版本
            rh.cmd = req.cmd;
            rh.seq = req.seq;
            rh.flags = GGWF_RESP;
            rh.payload_len = 0;
            send_all(c, &rh, sizeof(rh));
            TSOCK_CLOSE(c);
        });
        json out;
        int rc = cli.call(GGCMD_PING, json::object(), &out);
        std::printf("     [c2] rc=%d outcome=%d\n", rc, static_cast<int>(cli.last_outcome));
        check(rc == GGERR_INTERNAL, "c2) bad version -> call() == GGERR_INTERNAL(12)");
        check(cli.last_outcome == gg::CallOutcome::ProtocolError,
              "c2) bad version -> last_outcome == ProtocolError");
        if (th.joinable()) th.join();
    }

    std::printf(fails ? "\n== FAILED: %d ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
