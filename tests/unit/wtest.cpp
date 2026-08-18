// wtest.cpp — PC 端 wire 协议测试工具（诊断用）
// 用法: wtest.exe <cmd1> <cmd2> ...   （cmd 为数字, 如 1 29）
// 每个命令发一帧并读响应, 全部走同一条 TCP 连接。
#include "../../shared/wire_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK ::close
#endif

static bool read_full(int fd, void* buf, size_t len) {
    size_t off = 0;
    char* p = (char*)buf;
    while (off < len) {
        int n = ::recv(fd, p + off, (int)(len - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: wtest <cmd> [cmd...]\n"); return 1; }
#ifdef _WIN32
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
#endif
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(19100);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (::connect(fd, (sockaddr*)&sa, sizeof(sa)) != 0) {
        std::fprintf(stderr, "connect failed\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        uint16_t cmd = (uint16_t)atoi(argv[i]);
        std::string payload = "{\"p\":{}}";
        gg_wire_header_t h{};
        h.magic = GGWM_MAGIC;
        h.ver = GGWM_VERSION;
        h.cmd = cmd;
        h.seq = (uint32_t)i;
        h.flags = GGWF_REQ;
        h.payload_len = (uint32_t)payload.size();
        ::send(fd, (const char*)&h, sizeof(h), 0);
        ::send(fd, payload.data(), (int)payload.size(), 0);

        gg_wire_header_t rh{};
        if (!read_full(fd, &rh, sizeof(rh))) {
            std::printf("cmd %u: EOF/no response\n", cmd);
            break;
        }
        std::string body(rh.payload_len, '\0');
        if (rh.payload_len && !read_full(fd, &body[0], rh.payload_len)) {
            std::printf("cmd %u: partial body\n", cmd);
            break;
        }
        std::printf("cmd %u -> flags=0x%x: %s\n", cmd, rh.flags, body.c_str());
    }
    CLOSESOCK(fd);
    return 0;
}
