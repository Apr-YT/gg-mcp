// net_guard.h — HTTP 暴露面的两个纯判定（监听地址 / 浏览器 Origin）
//
// 抽成头文件的唯一原因是可测：这两个判定决定「谁可以驱动设备内存读写」，
// 而 main.cpp 里有 main() 无法被测试链接。实现保持 inline，不引入额外编译单元。
//
// 平台依赖：inet_pton。Windows 用 ws2tcpip.h 的 InetPtonA（由 winsock2 提供），
// POSIX 用 arpa/inet.h。调用方须保证 winsock2 已在本头文件之前被包含
// （main.cpp 里 httplib.h 已做到，见其 "先于 windows.h" 注释）。
#pragma once

#include <string>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace gg {

// 监听地址是否只对本机可见。
// 不能只比较 "0.0.0.0" 字面量：::、[::]、直接绑网卡地址（192.168.x.x）都对外可达，
// 却一个都命中不了字符串相等 —— 那些配置会在「要求 token」的检查里被当成安全放行。
inline bool host_is_loopback(const std::string& host) {
    if (host.empty()) return true;          // 空 → 调用方按 127.0.0.1 处理
    if (host == "localhost") return true;
    std::string h = host;
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    unsigned char buf[16] = {0};
    if (inet_pton(AF_INET, h.c_str(), buf) == 1) return buf[0] == 127;
    if (inet_pton(AF_INET6, h.c_str(), buf) == 1) {
        static const unsigned char lo6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return std::memcmp(buf, lo6, sizeof(lo6)) == 0;
    }
    return false;                            // 无法解析成 IP → 保守当作对外，要求 token
}

// 浏览器跨源防线（DNS rebinding / CSRF）。
// MCP 客户端（stdio / SDK）不发 Origin；发 Origin 的一定是网页。
// Origin: null（file:// 与 sandbox iframe）同样拒绝 —— 那正是攻击页面的常用形态。
//
// 不能只做前缀比较：`http://127.0.0.1.evil.com` 也以 `http://127.0.0.1` 开头。
// 前缀之后必须是端口分隔、路径或串尾，才算同一个源。
inline bool origin_allowed(const std::string& origin) {
    if (origin.empty()) return true;
    if (origin == "null") return false;
    auto host_is = [&](const char* scheme, const char* host) {
        const std::string prefix = std::string(scheme) + "://" + host;
        if (origin.rfind(prefix, 0) != 0) return false;
        if (origin.size() == prefix.size()) return true;
        const char c = origin[prefix.size()];
        return c == ':' || c == '/';
    };
    return host_is("http", "127.0.0.1") || host_is("https", "127.0.0.1") ||
           host_is("http", "localhost") || host_is("https", "localhost");
}

}  // namespace gg
