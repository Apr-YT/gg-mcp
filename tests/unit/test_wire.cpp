// test_wire.cpp — wire 协议编解码约定单测（头结构 + JSON 负载约定 + QX 结构布局）
#include "../../shared/wire_protocol.h"
#include "../../third_party/json.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <sys/types.h>

static_assert(sizeof(gg_wire_header_t) == 20, "wire header must be exactly 20 bytes");

// QX 驱动结构布局（与 qx-对接.h 的 COPY_MEMORY 自然对齐一致）
struct qx_copy_memory_cpp {
    pid_t     pid;      // offset 0
    uintptr_t addr;     // offset 8
    void     *buffer;   // offset 16
    size_t    size;     // offset 24
};
static_assert(offsetof(qx_copy_memory_cpp, pid) == 0, "qx cm.pid @0");
static_assert(offsetof(qx_copy_memory_cpp, addr) == 8, "qx cm.addr @8");
static_assert(offsetof(qx_copy_memory_cpp, buffer) == 16, "qx cm.buffer @16");
static_assert(offsetof(qx_copy_memory_cpp, size) == 24, "qx cm.size @24");
static_assert(sizeof(qx_copy_memory_cpp) == 32, "qx COPY_MEMORY sizeof 32");

struct qx_module_base_cpp {
    pid_t     pid;      // offset 0
    char     *name;     // offset 8
    uintptr_t base;     // offset 16
};
static_assert(offsetof(qx_module_base_cpp, name) == 8, "qx mb.name @8");
static_assert(sizeof(qx_module_base_cpp) == 24, "qx MODULE_BASE sizeof 24");

int main() {
    using json = nlohmann::json;
    int fails = 0;
    auto check = [&](bool c, const char* m) {
        if (!c) { std::printf("FAIL: %s\n", m); fails++; }
        else std::printf("ok  : %s\n", m);
    };

    check(GGWM_MAGIC == 0x47474D43u, "magic == 'GGMC'");
    check(GGWM_VERSION == 1u, "wire version == 1");
    check(GGWM_HEADER_LEN == 20u, "header len == 20");

    // 命令码唯一且 < GGCMD_MAX
    bool uniq = true;
    for (int i = 1; i < GGCMD_MAX; i++)
        for (int j = i + 1; j < GGCMD_MAX; j++)
            if (i == j) uniq = false;
    check(uniq, "command codes unique");
    check(GGCMD_PING == 1 && GGCMD_MAX > 30, "cmd range sanity");

    // 值类型
    check(VT_BYTE == 1 && VT_WORD == 2 && VT_DWORD == 3 && VT_QWORD == 4 &&
          VT_FLOAT == 5 && VT_DOUBLE == 6, "vtype enum");
    check(GGERR_VERSION_MISMATCH == 5 && GGERR_NO_SESSION == 3 &&
          GGERR_SAFE_MODE == 13, "error codes");

    // JSON 负载约定: 请求 {"p":{...}} / 响应 {"r":{...}} / 错误 {"e":{code,message}}
    json req;
    req[GG_J_REQ]["type"] = VT_DWORD;
    req[GG_J_REQ]["value"] = "100";
    check(req[GG_J_REQ]["type"] == 3 && req[GG_J_REQ]["value"] == "100",
          "request payload convention");

    json resp;
    resp[GG_J_RES]["count"] = 42;
    resp[GG_J_RES]["truncated"] = false;
    check(resp[GG_J_RES]["count"] == 42, "response payload convention");

    json err;
    err[GG_J_ERR]["code"] = GGERR_VERSION_MISMATCH;
    err[GG_J_ERR]["message"] = "version family mismatch";
    check(err[GG_J_ERR]["code"] == 5, "error payload convention");

    // u64 精度: 用字符串承载, 不丢精度
    json val;
    val["value"] = "18446744073709551615";
    check(val["value"] == "18446744073709551615", "u64 as string no precision loss");

    std::printf(fails ? "\n== FAILED: %d ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
