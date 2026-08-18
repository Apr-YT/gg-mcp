// test_wire_posix_syntax.cpp
// 仅用于在本机(MinGW/Windows)验证 X1 的 POSIX 超时分支语法正确性。
// 内容逐字复制自 server/device/wire.cpp 的 #else (POSIX) 分支逻辑，
// 配合 tests/unit/posix_stub/ 下的极简 POSIX 头 stub 做 -fsyntax-only 编译。
// 注：本机跑 server 走 _WIN32 分支，POSIX 分支无法在 Windows 完整编译，
//     故用此副本 + stub 验证 API 用法；最终以 Linux/macOS 部署实测为准。
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>

static int test_connect_timeout(int fd, int connect_timeout_ms, int io_timeout_ms) {
#ifndef EINPROGRESS
#define EINPROGRESS 115
#endif
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in sa{};
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (rc != 0 && errno != EINPROGRESS) {
        rc = -1;
    } else {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int pr = poll(&pfd, 1, connect_timeout_ms);
        if (pr <= 0) {
            rc = -1;
        } else {
            int sockerr = 0;
            socklen_t sl = sizeof(sockerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &sl) != 0 || sockerr != 0)
                rc = -1;
            else
                rc = 0;
        }
        fcntl(fd, F_SETFL, flags);
        if (rc == 0) {
            struct timeval tv{};
            tv.tv_sec = io_timeout_ms / 1000;
            tv.tv_usec = (io_timeout_ms % 1000) * 1000;
            fprintf(stderr, "[wire] posix io=%dms\n", io_timeout_ms);
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
    }
    return rc;
}

int main() { return test_connect_timeout(-1, 5000, 300000); }
