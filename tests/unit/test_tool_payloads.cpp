// test_tool_payloads.cpp — PC 端工具参数映射端到端用例（本地假 agentd，无设备）
//
// 为什么需要这一层：itest 直接调 agentd 引擎，走不到 PC 侧；test_session 只覆盖
// 「非法枚举必须在 device_call 之前被拒」，合法参数的**转发**从没被验证过。
// 结果是「schema 声明了某参数、agentd 也实现了，但 session.cpp 里忘了塞进
// params」这类缺陷完全静默 —— AI 传了没反应，还以为自己改到了行为。
// 2026-11-10 的静态护栏 check_tool_params.py 抓到了 gg_search_range.aligned，
// 但静态解析只能看「读了哪些键」，看不到「读出来的值到底怎么写的」。
//
// 做法：本机 127.0.0.1 起一个假 agentd（fake_agentd.h —— 真 socket、真
// AgentClient、真 Session::call_tool），把 Session 发出去的每帧记下来，
// 断言 params 里的键名与**规范化后的值**。整条链路不走 adb、不需要设备。
//
// 覆盖重点（都是历史上出过事或容易漂移的映射）：
//   addr 数字 → "0x..." 字符串（agentd 按 valuestring 取，原样透传会 NULL）
//   type/region/op 字符串 → 枚举整数
//   aligned 布尔 → 0/1，且 null 不许把默认 true 悄悄翻成 false
//   watch_inject 的 pc/fp/sp/lr 循环动态键
//   落盘类工具（region_dump / freeze_save / results_save）的 name 透传
#include "fake_agentd.h"

#include <cstdio>
#include <string>

namespace {

int g_fails = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        g_fails++;
    } else {
        std::printf("ok  : %s\n", what);
    }
}

/* call_tool 的失败判定：MCP 结果里的 isError。 */
bool is_error(const gg::json& r) {
    return r.value("isError", false);
}

}  // namespace

int main() {
    /* 行缓冲：断言失败要能立刻看到（被管道/重定向捕获时默认是全缓冲，
     * 一旦用例中途挂住，缓冲区里的 ok 行会全部丢失，看起来像"什么都没跑"）。 */
    setvbuf(stdout, nullptr, _IOLBF, 0);

    fake::FakeAgentd fake;
    if (!fake.start()) {
        std::printf("FAIL: fake agentd cannot listen on 127.0.0.1\n");
        return 1;
    }

    gg::ServerConfig cfg;
    cfg.agent_port = fake.port();
    cfg.wire_key = "test-key";
    cfg.annotations_file = "build/pc/_test_tool_payloads_annotations.json";
    cfg.export_dir = "build/pc/_test_tool_payloads_exports";
    gg::Session s(cfg);
    using gg::json;

    /* 1) 区间搜索全参：type/region 转枚举、module 透传、aligned 转 0/1。
     * aligned 是 2026-11-10 静态护栏抓到的真缺陷（此前 PC 侧从不转发）。 */
    {
        fake.clear();
        json r = s.call_tool("gg_search_range",
                              json{{"min", 1}, {"max", 100}, {"type", "qword"},
                                   {"region", "writable"}, {"module", "libil2cpp.so"},
                                   {"aligned", false}});
        check(!is_error(r), "sr) call accepted");
        auto q = fake.last();
        check(q.cmd == GGCMD_SEARCH_RANGE, "sr) cmd == GGCMD_SEARCH_RANGE");
        check(q.params.value("type", -1) == VT_QWORD, "sr) type qword -> VT_QWORD");
        check(q.params.value("region", -1) == REGION_WRITABLE, "sr) region writable -> REGION_WRITABLE");
        check(q.params.value("module", std::string()) == "libil2cpp.so", "sr) module forwarded");
        check(q.params.contains("aligned") && q.params["aligned"] == 0,
              "sr) aligned=false -> 0 (not silently dropped)");
        check(q.params.value("min", 0.0) == 1.0 && q.params.value("max", 0.0) == 100.0,
              "sr) min/max forwarded");
    }

    /* 2) aligned 缺省与 null 都必须"不传"，让 agentd 落默认 1（只扫对齐位置）。
     * 若把 null 当 0 转发，默认语义被悄悄翻成逐字节 —— 结果数虚高且看不出原因。 */
    {
        fake.clear();
        s.call_tool("gg_search_range", json{{"min", 1}, {"max", 2}});
        check(!fake.last().params.contains("aligned"), "sr) missing aligned -> not sent");

        fake.clear();
        s.call_tool("gg_search_range", json{{"min", 1}, {"max", 2}, {"aligned", nullptr}});
        check(!fake.last().params.contains("aligned"), "sr) aligned=null -> not sent");
    }

    /* 3) aligned 显式非法必须在本地拒（不能静默当 0/1 发下去）。 */
    {
        fake.clear();
        json r = s.call_tool("gg_search_range",
                             json{{"min", 1}, {"max", 2}, {"aligned", "yes"}});
        check(is_error(r), "sr) aligned=\"yes\" -> isError");
        check(fake.count() == 0, "sr) aligned=\"yes\" sends nothing to device");
    }

    /* 4) addr 规范化：JSON 数字 4096 必须变成 "0x1000"。
     * agentd 按 ->valuestring 取地址，数字原样透传会拿到 NULL。 */
    {
        fake.clear();
        json r = s.call_tool("gg_read", json{{"addr", 4096}, {"type", "qword"}});
        check(!is_error(r), "read) call accepted");
        auto q = fake.last();
        check(q.cmd == GGCMD_READ, "read) cmd == GGCMD_READ");
        check(q.params.value("addr", std::string()) == "0x1000", "read) numeric addr -> \"0x1000\"");
        check(q.params.value("type", -1) == VT_QWORD, "read) type qword -> VT_QWORD");
    }

    /* 5) 结果分页：offset/limit 有默认值，且 2026-11-10 起不再带已废弃的 type。 */
    {
        fake.clear();
        s.call_tool("gg_get_results", json::object());
        auto q = fake.last();
        check(q.cmd == GGCMD_GET_RESULTS, "gr) cmd == GGCMD_GET_RESULTS");
        check(q.params.value("offset", -1) == 0 && q.params.value("limit", -1) == 100,
              "gr) offset/limit defaults 0/100");
        check(!q.params.contains("type"), "gr) no stale type= in payload");
    }

    /* 6) watch_inject 的动态键循环：pc/fp/sp/lr 一律规范成 hex 字符串。
     * 这条同时护住 check_tool_params.py 的 for-list 解析。 */
    {
        fake.clear();
        json r = s.call_tool("gg_watch_inject",
                             json{{"pc", 4096}, {"sp", "0x7ff000"}, {"lr", 16}});
        check(!is_error(r), "wi) call accepted");
        auto q = fake.last();
        check(q.cmd == GGCMD_WATCH, "wi) cmd == GGCMD_WATCH");
        check(q.params.value("pc", std::string()) == "0x1000", "wi) pc numeric -> \"0x1000\"");
        check(q.params.value("sp", std::string()) == "0x7ff000", "wi) sp string kept");
        check(q.params.value("lr", std::string()) == "0x10", "wi) lr numeric -> \"0x10\"");
        check(!q.params.contains("fp"), "wi) absent fp not invented");
    }

    /* 7) 区域差异：可选参数只在给出时下发，gap/limit 走显式值。 */
    {
        fake.clear();
        s.call_tool("gg_region_diff",
                    json{{"name", "hp1"}, {"other", "hp0"}, {"gap", 0}, {"limit", 10}});
        auto q = fake.last();
        check(q.cmd == GGCMD_REGION_DIFF, "rd) cmd == GGCMD_REGION_DIFF");
        check(q.params.value("name", std::string()) == "hp1", "rd) name forwarded");
        check(q.params.value("other", std::string()) == "hp0", "rd) other forwarded");
        check(q.params.value("gap", -1) == 0, "rd) gap=0 forwarded (0 是有意义的值)");
        check(q.params.value("limit", -1) == 10, "rd) limit forwarded");

        fake.clear();
        s.call_tool("gg_region_diff", json{{"name", "hp1"}});
        check(!fake.last().params.contains("other"), "rd) absent other not sent");
    }

    /* 8) 落盘类工具的 name 透传（设备端拼路径，PC 侧只做字符集校验）。 */
    {
        fake.clear();
        s.call_tool("gg_freeze_save", json{{"name", "boss_fight"}});
        check(fake.last().cmd == GGCMD_FREEZE_SAVE, "fs) cmd == GGCMD_FREEZE_SAVE");
        check(fake.last().params.value("name", std::string()) == "boss_fight", "fs) name forwarded");

        fake.clear();
        s.call_tool("gg_freeze_load", json{{"name", "boss_fight"}});
        check(fake.last().cmd == GGCMD_FREEZE_LOAD, "fl) cmd == GGCMD_FREEZE_LOAD");
        check(fake.last().params.value("name", std::string()) == "boss_fight", "fl) name forwarded");

        fake.clear();
        s.call_tool("gg_region_dump", json{{"name", "hp"}, {"addr", "0x1000"}, {"len", 4096}});
        auto q = fake.last();
        check(q.cmd == GGCMD_REGION_DUMP, "dump) cmd == GGCMD_REGION_DUMP");
        check(q.params.value("addr", std::string()) == "0x1000", "dump) addr kept");
        check(q.params.value("len", -1) == 4096, "dump) len forwarded");
    }

    /* 9) 指针扫描：lookback/cap/aligned 三个可调项都是"给了才下发"。 */
    {
        fake.clear();
        s.call_tool("gg_pointer_scan",
                    json{{"type", "qword"}, {"addr", "0x2000"}, {"lookback", 256},
                         {"cap", 10}, {"aligned", true}});
        auto q = fake.last();
        check(q.cmd == GGCMD_POINTER_SCAN, "ps) cmd == GGCMD_POINTER_SCAN");
        check(q.params.value("addr", std::string()) == "0x2000", "ps) addr forwarded");
        check(q.params.value("lookback", -1) == 256, "ps) lookback forwarded");
        check(q.params.value("cap", -1) == 10, "ps) cap forwarded");
        check(q.params.contains("aligned") && q.params["aligned"] == 1,
              "ps) aligned=true -> 1");
    }

    /* 10) 恢复：addr 缺省不能编出 0（0 会被当成"显式写回地址 0"）。 */
    {
        fake.clear();
        s.call_tool("gg_region_restore", json{{"name", "hp"}});
        auto q = fake.last();
        check(q.cmd == GGCMD_REGION_RESTORE, "rs) cmd == GGCMD_REGION_RESTORE");
        check(!q.params.contains("addr"), "rs) absent addr not sent (0 means override)");
    }

    /* 11) 反面锚点：确认用例真的驱动了网络路径 —— 若假端一帧都没收到，
     * 上面所有 "cmd == ..." 断言都会因为 cmd=0 失败，但这条把根因说清楚。 */
    check(fake.count() > 0 || g_fails > 0, "meta) fake agentd actually served requests");

    fake.stop();
    if (g_fails) {
        std::printf("%d assertion(s) failed\n", g_fails);
        return 1;
    }
    std::printf("all tool payload assertions passed\n");
    return 0;
}
