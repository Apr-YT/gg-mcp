// test_protocol.cpp — JSON-RPC / MCP 协议层回归用例
//
// 被测对象：McpProtocol::handle（server/mcp/protocol.cpp）。
//
// 覆盖点（每条都对应一次真实缺陷或规范要求）：
//   1. 单条请求 —— 基线，确认重构 handle() 时没有破坏单条语义。
//   2. 批量请求（JSON-RPC 2.0 数组）—— 旧实现把数组一律回 -32600，使客户端把
//      并发 tools/call 合并成 batch 时拿到一批无法配对的 id:null 错误。
//   3. 批内混通知 —— 通知不得出现在响应数组里。
//   4. 整批都是通知 —— 必须返回空串（不写任何响应）。
//   5. 空数组 —— 规范要求视为 Invalid Request。
//   6. 批内非法元素 —— 该元素单独回 -32600，同批其它请求照常成功。
//
// 用例只走不依赖设备的路径（initialize / ping / 通知），不触发 agentd 连接。
#include "../../server/mcp/protocol.h"

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
    using json = nlohmann::json;

    gg::ServerConfig cfg;
    gg::Session session(cfg);
    gg::McpProtocol proto(session);

    // 1) 单条请求
    {
        std::string r = proto.handle(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
        json j = json::parse(r);
        check(j.is_object(), "single: response is an object");
        check(j["id"] == 1, "single: id echoed");
        check(j.contains("result") && j["result"].contains("protocolVersion"),
              "single: initialize carries protocolVersion");
    }

    // 2) 批量请求：两条都要有响应，且 id 各自配对
    {
        std::string r = proto.handle(
            R"([{"jsonrpc":"2.0","id":1,"method":"initialize"},)"
            R"({"jsonrpc":"2.0","id":2,"method":"ping"}])");
        json j = json::parse(r);
        check(j.is_array(), "batch: response is an array");
        check(j.size() == 2, "batch: two responses for two requests");
        if (j.size() == 2) {
            check(j[0]["id"] == 1 && j[0].contains("result"), "batch: first id paired");
            check(j[1]["id"] == 2 && j[1].contains("result"), "batch: second id paired");
        }
    }

    // 3) 批内混通知：通知不产生响应
    {
        std::string r = proto.handle(
            R"([{"jsonrpc":"2.0","method":"notifications/initialized"},)"
            R"({"jsonrpc":"2.0","id":9,"method":"ping"}])");
        json j = json::parse(r);
        check(j.is_array() && j.size() == 1, "batch+notification: only the request answers");
        if (j.is_array() && j.size() == 1) check(j[0]["id"] == 9, "batch+notification: id 9 only");
    }

    // 4) 整批都是通知：不回复
    {
        std::string r = proto.handle(
            R"([{"jsonrpc":"2.0","method":"notifications/initialized"},)"
            R"({"jsonrpc":"2.0","method":"notifications/cancelled"}])");
        check(r.empty(), "batch of notifications: no response written");
    }

    // 5) 空数组：Invalid Request
    {
        std::string r = proto.handle("[]");
        json j = json::parse(r);
        check(j.contains("error") && j["error"]["code"] == -32600, "empty batch: -32600");
    }

    // 6) 批内非法元素：只该元素报错，其它正常
    {
        std::string r = proto.handle(R"([1,{"jsonrpc":"2.0","id":7,"method":"ping"}])");
        json j = json::parse(r);
        check(j.is_array() && j.size() == 2, "batch with junk: two entries preserved");
        if (j.is_array() && j.size() == 2) {
            check(j[0].contains("error") && j[0]["error"]["code"] == -32600,
                  "batch with junk: junk entry rejected");
            check(j[1]["id"] == 7 && j[1].contains("result"),
                  "batch with junk: valid sibling still succeeds");
        }
    }

    // 7) 单帧上限判定：超限的响应必须由调用方转成错误帧，而不是交给发送层
    //    （发送层会拒发，客户端只能等到连接超时，看不出是"结果太大"）
    {
        check(gg_payload_fits(0), "payload limit: 0 fits");
        check(gg_payload_fits(1), "payload limit: 1 fits");
        check(gg_payload_fits(GGWM_MAX_PAYLOAD - 1), "payload limit: max-1 fits");
        check(gg_payload_fits(GGWM_MAX_PAYLOAD), "payload limit: max fits (inclusive)");
        check(!gg_payload_fits((size_t)GGWM_MAX_PAYLOAD + 1), "payload limit: max+1 rejected");
        check(!gg_payload_fits((size_t)64 * 1024 * 1024), "payload limit: 64MiB rejected");
    }

    if (fails == 0) {
        std::printf("== ALL PASS ==\n");
        return 0;
    }
    std::printf("== %d FAILED ==\n", fails);
    return 1;
}
