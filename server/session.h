// session.h — MCP 工具会话：持有 AgentClient，实现全部工具
#pragma once

#include <mutex>
#include <string>

#include "annotations.h"
#include "device/wire.h"
#include "../third_party/json.hpp"

namespace gg {

using json = nlohmann::json;

struct ServerConfig {
    std::string transport = "stdio";   // stdio | http
    int http_port = 8788;
    std::string http_host = "127.0.0.1";   // http 绑定地址（默认仅本机）
    std::string http_token;                // http 鉴权 bearer token（空=不鉴权；绑 0.0.0.0 时强制要求）
    std::string adb = "adb";
    std::string serial;
    int agent_port = 34127;      // 设备端 agentd 端口（hide 改造 2026-08-18）
    std::string wire_key;        // wire 握手密钥：必须与 agentd --wire-key 一致
    std::string gg_version_pattern = "101.*";   // 实际包: 宇航员修改器 = GG 101.1
    bool gg_strict = true;
    bool safe_mode = false;
    bool shell_enabled = false;
    size_t result_cap = 100000;
    std::string annotations_file = "annotations.json";   // AI 标注库持久化路径
    std::string export_dir = "exports";                  // save_to 落盘根目录：工具只能写到这里面
    size_t max_save_bytes = 256u * 1024u * 1024u;        // 单次 save_to 读取总量上限
    bool print_events = false;      // 订阅 agentd 事件流并在 stderr 打印（悬浮窗监控调试）
};

class Session {
public:
    explicit Session(ServerConfig cfg);

    // 执行 MCP 工具调用（name + args）→ MCP 结果对象（含 content/isError）
    json call_tool(const std::string& name, const json& args);

    // gg_status 用：本地会话概览
    json local_status();

    // 表驱动 handler 上下文（M1）：C++11 嵌套类是外层隐式 friend，
    // 可访问 Session 的 private 成员；handler 只经 ctx 与 Session 交互。
    // 注意：自由函数 handler 不能直接 c.s->private（访问控制按静态上下文），
    // 因此 private 能力统一经以下封装方法暴露。
    struct CallCtx {
        Session* s;
        json params;                          // handler 填充的 agentd 参数
        json send(uint16_t cmd);              // 请求 agentd 并包装成功/失败
        json fail(int code, const std::string& msg) { return s->fail(code, msg); }

        int norm_type(const json& t) { return s->norm_type(t); }
        int norm_region(const json& r) { return s->norm_region(r); }
        int norm_op(const json& o) { return s->norm_op(o); }

        // 显式传入但非法的枚举值不能静默变成另一种合法操作：返回 false，
        // 调用方必须 fail(GGERR_BAD_REQUEST)。字段缺失仍走默认值（兼容旧客户端）。
        bool put_type(const json& t) {
            int v = norm_type(t);
            if (v < 0) return false;
            params["type"] = v;
            return true;
        }
        bool put_region(const json& r) {
            int v = norm_region(r);
            if (v < 0) return false;
            params["region"] = v;
            return true;
        }
        bool put_op(const json& o) {
            int v = norm_op(o);
            if (v < 0) return false;
            params["op"] = v;
            return true;
        }
        json ok(const json& data) { return s->ok(data); }
        int device_call(uint16_t cmd, const json& params, json* out) {
            return s->device_call(cmd, params, out);
        }
        void relay_annotate(const std::string& op, const json& ann) {
            s->relay_annotate(op, ann);
        }
        AnnotationStore& store() { return s->store_; }
        const ServerConfig& cfg() { return s->cfg_; }
        const std::string& last_error() { return s->client_.last_error; }
    };

private:
    int device_call(uint16_t cmd, const json& params, json* out);

    json ok(const json& data);                 // 成功结果包装
    json fail(int code, const std::string& msg);

    static int norm_type(const json& t);       // "dword"→3 等；缺省 dword
    static int norm_region(const json& r);     // "anonymous"→1 等；缺省 anonymous
    static int norm_op(const json& o);         // "increased"→1 等；缺省 changed

    // 向 agentd 中继事件（标注变更 → 手机悬浮窗实时显示）。best-effort，失败忽略。
    void relay_annotate(const std::string& op, const json& ann);

    ServerConfig cfg_;
    AgentClient client_;
    AnnotationStore store_;
    std::mutex mtx_;
};

}  // namespace gg
