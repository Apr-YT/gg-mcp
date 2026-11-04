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

#include <atomic>
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

    /* 关闭监听 socket：让阻塞在 accept_one() 上的线程拿到 INVALID_SOCKET 退出。
     * 用例里客户端若不重试，第二次 accept 就永远没人来 —— 不关的话 join 卡死。
     * 先 shutdown 再 close：Windows 上单靠 closesocket 不保证打断阻塞中的 accept。 */
    void stop_listen() {
        if (lfd != TSOCK_INVALID) {
            ::shutdown(lfd,
#ifdef _WIN32
                       SD_BOTH
#else
                       SHUT_RDWR
#endif
            );
            TSOCK_CLOSE(lfd);
            lfd = TSOCK_INVALID;
        }
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

    // ---- d1) agentd 重启窗口：connect 成功但 AUTH 应答前连接断开 ----
    //
    // 时序：TCP 三次握手完成（旧 agentd 的 listen/accept 队列还在）→ agentd
    // 进程死掉 → 客户端 AUTH 读失败。这不是"密钥不匹配"（配置错误），是传输
    // 中断 —— 必须退避重试并在 agentd 回来后自动恢复，而不是报 GGERR_PERM
    // 把"断线"伪装成"权限被拒"。
    {
        Peer peer;
        check(peer.listen_loopback(), "d1) fake peer listening on 127.0.0.1");
        gg::AgentClient cli(peer_cfg(peer.port, 3000));
        std::thread th([&] {
            // 第一次连接：读完 AUTH 请求就断（agentd 重启瞬间的经典时序）
            tsock_t c1 = peer.accept_one();
            if (c1 != TSOCK_INVALID) {
                gg_wire_header_t req{};
                std::string pl;
                read_request(c1, &req, &pl);
                TSOCK_CLOSE(c1);
            }
            // 第二次连接（agentd 已回来）：完整握手 + 应答业务帧
            tsock_t c2 = peer.accept_one();
            if (c2 != TSOCK_INVALID) {
                if (serve_auth(c2)) {
                    gg_wire_header_t req{};
                    std::string pl;
                    if (read_request(c2, &req, &pl)) {
                        std::string body = "{\"r\":{\"pong\":1}}";
                        gg_wire_header_t rh{};
                        rh.magic = GGWM_MAGIC;
                        rh.ver = GGWM_VERSION;
                        rh.cmd = req.cmd;
                        rh.seq = req.seq;
                        rh.flags = GGWF_RESP;
                        rh.payload_len = static_cast<uint32_t>(body.size());
                        if (send_all(c2, &rh, sizeof(rh)))
                            send_all(c2, body.data(), body.size());
                    }
                }
                TSOCK_CLOSE(c2);
            }
        });
        auto t0 = std::chrono::steady_clock::now();
        json out;
        int rc = cli.call(GGCMD_PING, json::object(), &out);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0).count();
        std::printf("     [d1] rc=%d elapsed=%lldms outcome=%d\n", rc, (long long)elapsed_ms,
                    static_cast<int>(cli.last_outcome));
        check(rc == 0 && out.is_object() && out.value("pong", 0) == 1,
              "d1) auth-phase disconnect -> retry succeeds after restart");
        check(cli.last_outcome == gg::CallOutcome::Ok,
              "d1) auth-phase disconnect -> last_outcome == Ok (not Perm)");
        check(elapsed_ms < 2900, "d1) retries bounded by request deadline");
        peer.stop_listen();          // 若客户端未重试，第二个 accept 无人来，别让 join 卡死
        if (th.joinable()) th.join();
    }

    // ---- d2) AUTH 被明确拒绝（wire_key 不匹配）：不重试 ----
    //
    // 密钥不匹配是配置问题，重试只会把错误掩盖成 30 秒卡顿（ensure_connected_retry
    // 的设计约束）。实证断言：错误码 GGERR_PERM、快速失败、观察窗口内没有第二次连接。
    {
        Peer peer;
        check(peer.listen_loopback(), "d2) fake peer listening on 127.0.0.1");
        gg::AgentClient cli(peer_cfg(peer.port, 3000));
        std::atomic<int> connections{0};
        std::thread th([&] {
            tsock_t c1 = peer.accept_one();
            if (c1 != TSOCK_INVALID) {
                connections = 1;
                gg_wire_header_t req{};
                std::string pl;
                if (read_request(c1, &req, &pl) && req.cmd == GGCMD_AUTH)
                    send_response(c1, req.cmd, req.seq,
                                  "{\"e\":{\"code\":4,\"message\":\"key mismatch\"}}",
                                  GGWF_RESP | GGWF_ERR);
                TSOCK_CLOSE(c1);
            }
            tsock_t c2 = peer.accept_one();   // 阻塞：客户端若错误地重试，这里才有人来
            if (c2 != TSOCK_INVALID) {
                connections = 2;
                TSOCK_CLOSE(c2);
            }
        });
        auto t0 = std::chrono::steady_clock::now();
        json out;
        int rc = cli.call(GGCMD_PING, json::object(), &out);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0).count();
        std::printf("     [d2] rc=%d elapsed=%lldms outcome=%d conns=%d\n", rc,
                    (long long)elapsed_ms, static_cast<int>(cli.last_outcome),
                    connections.load());
        check(rc == GGERR_PERM, "d2) auth rejected -> call() == GGERR_PERM(4)");
        check(cli.last_outcome == gg::CallOutcome::RemoteError,
              "d2) auth rejected -> last_outcome == RemoteError");
        check(elapsed_ms < 400, "d2) auth rejected fails fast (no 200ms+ backoff)");
        /* call() 已返回：客户端此刻空闲，若要重试只会在 200ms 退避后发生。
         * 等 300ms 观察窗口过了再关监听 —— 提前关会把"没人来"误判成证据不足。 */
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        peer.stop_listen();
        if (th.joinable()) th.join();
        check(connections.load() == 1, "d2) auth rejected -> exactly 1 connection (no retry)");
    }

    // ---- d3) 帧发送中途断连（RST）：整帧没发完 → 安全重连重放一次 ----
    //
    // SO_LINGER{1,0} 关闭产生 RST 而非 FIN：客户端下一次 send 直接 ECONNRESET。
    // write_frame 未完整交付 → 对端不可能执行该请求 → 重放安全（call() 注释的
    // 安全边界），重连后同请求应被正常应答。
    {
        Peer peer;
        check(peer.listen_loopback(), "d3) fake peer listening on 127.0.0.1");
        gg::AgentClient cli(peer_cfg(peer.port, 3000));
        std::thread th([&] {
            tsock_t c1 = peer.accept_one();
            if (c1 != TSOCK_INVALID) {
                serve_auth(c1);
                // 不读后续请求，直接 RST：客户端 write PING 时撞 ECONNRESET
                linger l{1, 0};
                setsockopt(c1, SOL_SOCKET, SO_LINGER,
                           reinterpret_cast<const char*>(&l), sizeof(l));
                TSOCK_CLOSE(c1);
            }
            tsock_t c2 = peer.accept_one();
            if (c2 != TSOCK_INVALID) {
                if (serve_auth(c2)) {
                    gg_wire_header_t req{};
                    std::string pl;
                    if (read_request(c2, &req, &pl)) {
                        std::string body = "{\"r\":{\"pong\":2}}";
                        gg_wire_header_t rh{};
                        rh.magic = GGWM_MAGIC;
                        rh.ver = GGWM_VERSION;
                        rh.cmd = req.cmd;
                        rh.seq = req.seq;
                        rh.flags = GGWF_RESP;
                        rh.payload_len = static_cast<uint32_t>(body.size());
                        if (send_all(c2, &rh, sizeof(rh)))
                            send_all(c2, body.data(), body.size());
                    }
                }
                TSOCK_CLOSE(c2);
            }
        });
        json out;
        int rc = cli.call(GGCMD_PING, json::object(), &out);
        std::printf("     [d3] rc=%d outcome=%d\n", rc, static_cast<int>(cli.last_outcome));
        check(rc == 0 && out.is_object() && out.value("pong", 0) == 2,
              "d3) mid-frame reset -> reconnect & replay succeeds");
        check(cli.last_outcome == gg::CallOutcome::Ok,
              "d3) mid-frame reset -> last_outcome == Ok");
        peer.stop_listen();
        if (th.joinable()) th.join();
    }

    std::printf(fails ? "\n== FAILED: %d ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
