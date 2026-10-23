// test_session.cpp — session 层枚举校验回归用例
//
// 被测对象：ServerConfig 新增的 export_dir / max_save_bytes，以及
// Session::CallCtx 的 put_type / put_region / put_op。
//
// 语义要求（与实现注释一致）：
//   - 显式传入但非法的枚举值 → 返回 false，且不得把任何默认值写进 params
//     （调用方据此 fail(GGERR_BAD_REQUEST)，绝不能静默变成另一种合法操作）
//   - 字段缺失（null）→ 返回 true，落到默认值 dword / anonymous / changed
//
// 用例全部走真实实现，不做 mock：默认值路径直接调 CallCtx（不触发网络），
// 端到端路径调 Session::call_tool，非法枚举在 device_call 之前就该被拒。
#include "../../server/session.h"

#include <cstdio>
#include <string>

namespace {

int error_code_of(const nlohmann::json& result) {
    if (!result.contains("content") || !result["content"].is_array() ||
        result["content"].empty())
        return -1;
    const nlohmann::json& cell = result["content"][0];
    if (!cell.contains("text") || !cell["text"].is_string()) return -1;
    try {
        nlohmann::json inner = nlohmann::json::parse(cell["text"].get<std::string>());
        if (inner.contains("error") && inner["error"].contains("code"))
            return inner["error"]["code"].get<int>();
    } catch (...) {
        return -1;
    }
    return -1;
}

}  // namespace

int main() {
    using json = nlohmann::json;
    int fails = 0;
    auto check = [&](bool c, const char* m) {
        if (!c) { std::printf("FAIL: %s\n", m); fails++; }
        else std::printf("ok  : %s\n", m);
    };

    gg::ServerConfig cfg;
    cfg.annotations_file = "build/pc/_test_session_annotations.json";   // 不碰真实标注库
    check(cfg.export_dir == "exports", "config: export_dir defaults to exports");
    check(cfg.max_save_bytes == 256u * 1024u * 1024u, "config: max_save_bytes defaults to 256MiB");

    gg::Session s(cfg);

    // ---- d1) put_type ----
    {
        gg::Session::CallCtx c{&s, json::object()};
        check(c.put_type(json("nope")) == false, "d1) put_type(\"nope\") == false");
        check(!c.params.contains("type"), "d1) put_type(\"nope\") writes no default into params");
        check(c.put_type(json(999)) == false, "d1) put_type(999) == false (out of range)");
        check(c.put_type(json(0)) == false, "d1) put_type(0) == false (below VT_BYTE)");
        check(!c.params.contains("type"), "d1) rejected values leave params untouched");

        check(c.put_type(json()) == true, "d1) put_type(missing) == true");
        check(c.params.value("type", -1) == VT_DWORD, "d1) put_type(missing) -> VT_DWORD");
        check(c.put_type(json("qword")) == true, "d1) put_type(\"qword\") == true");
        check(c.params.value("type", -1) == VT_QWORD, "d1) put_type(\"qword\") -> VT_QWORD");
    }

    // ---- d2) put_region ----
    {
        gg::Session::CallCtx c{&s, json::object()};
        check(c.put_region(json("writablee")) == false, "d2) put_region(\"writablee\") == false");
        check(!c.params.contains("region"), "d2) put_region invalid writes no default into params");
        check(c.put_region(json(0)) == false, "d2) put_region(0) == false (out of range)");
        check(c.put_region(json()) == true, "d2) put_region(missing) == true");
        check(c.params.value("region", -1) == REGION_ANONYMOUS,
              "d2) put_region(missing) -> REGION_ANONYMOUS");
        check(c.put_region(json("writable")) == true, "d2) put_region(\"writable\") == true");
        check(c.params.value("region", -1) == REGION_WRITABLE, "d2) put_region(\"writable\") -> REGION_WRITABLE");
    }

    // ---- d3) put_op ----
    {
        gg::Session::CallCtx c{&s, json::object()};
        check(c.put_op(json("up")) == false, "d3) put_op(\"up\") == false");
        check(!c.params.contains("op"), "d3) put_op invalid writes no default into params");
        check(c.put_op(json(99)) == false, "d3) put_op(99) == false (out of range)");
        check(c.put_op(json()) == true, "d3) put_op(missing) == true");
        check(c.params.value("op", -1) == REFINE_CHANGED, "d3) put_op(missing) -> REFINE_CHANGED");
        check(c.put_op(json("increased")) == true, "d3) put_op(\"increased\") == true");
        check(c.params.value("op", -1) == REFINE_INCREASED, "d3) put_op(\"increased\") -> REFINE_INCREASED");
    }

    // ---- d4) 端到端：非法枚举必须变成 GGERR_BAD_REQUEST，且不碰设备 ----
    {
        json r = s.call_tool("gg_search", json{{"value", "100"}, {"type", "nope"}});
        check(r.value("isError", false) == true, "d4) gg_search bad type -> isError");
        check(error_code_of(r) == GGERR_BAD_REQUEST, "d4) gg_search bad type -> GGERR_BAD_REQUEST");

        r = s.call_tool("gg_search", json{{"value", "100"}, {"region", "everywhere"}});
        check(r.value("isError", false) == true, "d4) gg_search bad region -> isError");
        check(error_code_of(r) == GGERR_BAD_REQUEST, "d4) gg_search bad region -> GGERR_BAD_REQUEST");

        r = s.call_tool("gg_search_refine", json{{"op", "sideways"}});
        check(r.value("isError", false) == true, "d4) gg_search_refine bad op -> isError");
        check(error_code_of(r) == GGERR_BAD_REQUEST, "d4) gg_search_refine bad op -> GGERR_BAD_REQUEST");
    }

    // ---- f1) 负数字符串地址必须被拒 ----
    // strtoull 接受前导 '-' 并做模运算："-1" → 0xFFFFFFFFFFFFFFFF，且不设 ERANGE。
    // 旧实现在 dispatch 的集中校验里只看 endptr/ERANGE，负地址会漏过去。
    {
        json r = s.call_tool("gg_read", json{{"addr", "-1"}, {"type", "dword"}});
        check(r.value("isError", false) == true, "f1) addr=\"-1\" -> isError");
        check(error_code_of(r) == GGERR_BAD_REQUEST, "f1) addr=\"-1\" -> GGERR_BAD_REQUEST");

        r = s.call_tool("gg_read", json{{"addr", "-0x10"}, {"type", "dword"}});
        check(error_code_of(r) == GGERR_BAD_REQUEST, "f1) addr=\"-0x10\" -> GGERR_BAD_REQUEST");

        r = s.call_tool("gg_read_bytes", json{{"addr", "-1"}, {"len", 4}});
        check(error_code_of(r) == GGERR_BAD_REQUEST, "f1) gg_read_bytes addr=\"-1\" rejected");
    }

    // ---- f2) force 参数已永久禁用 ----
    // 历史：旧写法 a["force"].get<int>() 对 "abc" 抛 json::type_error，被协议层
    // 统一吞成 -32603（参数错报成服务端故障）。
    // 现在 schema 不再声明 force，且传 1 必须**明确拒绝**：rwproc33 的
    // is_force_read 在本机内核上会 panic，静默忽略会让调用方以为强制读生效了。
    {
        json r = s.call_tool("gg_read", json{{"addr", "0x1000"}, {"force", "abc"}});
        check(r.value("isError", false) == true, "f2) force=\"abc\" -> isError");
        check(error_code_of(r) == GGERR_BAD_REQUEST, "f2) force=\"abc\" -> GGERR_BAD_REQUEST");

        r = s.call_tool("gg_read", json{{"addr", "0x1000"}, {"force", 2}});
        check(error_code_of(r) == GGERR_BAD_REQUEST, "f2) force=2 -> GGERR_BAD_REQUEST");

        // 传 1 现在是显式拒绝（不是"放行后由设备端拒"）
        r = s.call_tool("gg_read", json{{"addr", "0x1000"}, {"force", 1}});
        check(error_code_of(r) == GGERR_BAD_REQUEST, "f2) force=1 -> GGERR_BAD_REQUEST (disabled)");

        // force=0 视同未传：必须继续走 device_call（无设备 → NO_DEVICE，不是 BAD_REQUEST）
        r = s.call_tool("gg_read", json{{"addr", "0x1000"}, {"force", 0}});
        check(error_code_of(r) != GGERR_BAD_REQUEST, "f2) force=0 still accepted");
    }

    std::printf(fails ? "\n== FAILED: %d ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
