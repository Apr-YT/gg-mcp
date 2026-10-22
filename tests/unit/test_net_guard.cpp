// test_net_guard.cpp — HTTP 暴露面判定的回归用例
//
// 被测对象：server/net_guard.h（host_is_loopback / origin_allowed）。
// 这两个判定决定「谁可以驱动设备内存读写」，写错的后果是无 token 对外暴露；
// 所以每种绕过写法都要有断言钉住。
#include "../../server/net_guard.h"

#include <cstdio>
#include <string>

namespace {

int fails = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("ok  : %s\n", what);
    } else {
        std::printf("FAIL: %s\n", what);
        ++fails;
    }
}

}  // namespace

int main() {
    using gg::host_is_loopback;
    using gg::origin_allowed;

    // ---- 监听地址 ----
    check(host_is_loopback(""), "empty host is loopback (caller defaults to 127.0.0.1)");
    check(host_is_loopback("127.0.0.1"), "127.0.0.1 loopback");
    check(host_is_loopback("127.1.2.3"), "any 127/8 is loopback");
    check(host_is_loopback("localhost"), "localhost loopback");
    check(host_is_loopback("::1"), "IPv6 ::1 loopback");
    check(host_is_loopback("[::1]"), "bracketed [::1] loopback");

    check(!host_is_loopback("0.0.0.0"), "0.0.0.0 is not loopback");
    check(!host_is_loopback("::"), ":: is not loopback");
    check(!host_is_loopback("[::]"), "bracketed [::] is not loopback");
    check(!host_is_loopback("192.168.1.10"), "LAN address is not loopback");
    check(!host_is_loopback("127.0.0.1.evil.com"), "hostname spoof is not loopback");
    check(!host_is_loopback("example.com"), "unresolvable name is treated as exposed");

    // ---- 浏览器 Origin ----
    check(origin_allowed(""), "absent Origin allowed (non-browser MCP client)");
    check(origin_allowed("http://127.0.0.1"), "http://127.0.0.1 allowed");
    check(origin_allowed("http://127.0.0.1:8788"), "with port allowed");
    check(origin_allowed("https://localhost:443/"), "https localhost allowed");

    check(!origin_allowed("null"), "Origin: null rejected (file:// / sandboxed iframe)");
    check(!origin_allowed("http://127.0.0.1.evil.com"), "suffix spoof rejected");
    check(!origin_allowed("http://localhost.evil.com"), "localhost suffix spoof rejected");
    check(!origin_allowed("http://evil.com"), "foreign origin rejected");
    check(!origin_allowed("https://127.0.0.1@evil.com"), "userinfo trick rejected");

    if (fails == 0) {
        std::printf("== ALL PASS ==\n");
        return 0;
    }
    std::printf("== %d FAILED ==\n", fails);
    return 1;
}
