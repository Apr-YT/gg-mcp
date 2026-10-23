// shared_ctx.h — UI 与 AI（MCP 服务器）之间的共享工作上下文
//
// 【为什么需要】
// agentd 的 g_eng 是全局单例，所以搜索结果集、attach 目标**本来就是同一份**：
// UI 和 MCP 服务器各开一条连接，看到的是同一个 g_eng。缺的不是数据，是"谁在干什么"
// —— AI 不知道人刚才手动搜了什么值、选中了哪一行；人也不知道 AI 搜过什么。
//
// 【怎么解决】
// 两个进程通过 build/gg-shared/ 下的两个文件对话，不用端口、不用依赖：
//
//   context.json   UI 写（每次 attach / 搜索 / 选中变化），AI 读
//                  内容：目标进程、搜索条件、结果条数、选中行、最近动作
//   cmd.json       AI 写，UI 读完执行后删除；执行结果写 cmd_result.json
//
// 【为什么用文件而不是本地 socket】
// - 崩溃安全：进程挂了文件还在，下次启动能读到上次的上下文
// - 零依赖：不用 winsock 手写 HTTP，不用改端口占用
// - 可调试：出问题 cat 一下就知道谁写了什么
// 代价是轮询（UI 200ms 一次、开销可忽略），以及要靠 ts 判断对端是否还活着。
//
// 【header-only 的理由】
// build_pc.bat 和 build_ui.bat 都要编它，做成 .h 就不用同时改两个构建脚本。
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../third_party/json.hpp"

namespace gg {
namespace ctx {

using nlohmann::json;

// 对端超过这个秒数没更新 context，就认为 UI 已经关了。
// 3 秒：UI 每 200ms 刷新一次 ts，留 15 倍的余量，避免偶尔卡帧误判。
constexpr int kStaleSecs = 3;

inline std::string dir() {
    const char* env = getenv("GG_SHARED_DIR");
    if (env && *env) return std::string(env);
    return std::string("build/gg-shared");
}

inline std::string context_path() { return dir() + "/context.json"; }
inline std::string cmd_path()     { return dir() + "/cmd.json"; }
inline std::string result_path()  { return dir() + "/cmd_result.json"; }

inline void ensure_dir() {
#ifdef _WIN32
    _mkdir("build");
    _mkdir(dir().c_str());
#else
    ::mkdir("build", 0755);
    ::mkdir(dir().c_str(), 0755);
#endif
}

inline std::string read_all(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// 原子写：先写 .tmp 再 rename。中途崩了也不会留下半个 JSON
// （读的一方要么看到旧文件，要么看到新的完整文件）。
// Windows 的 rename 不能覆盖已存在的目标，所以先删。
inline bool write_atomic(const std::string& path, const std::string& body) {
    ensure_dir();
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const size_t n = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    if (n != body.size()) return false;
#ifdef _WIN32
    _unlink(path.c_str());
#endif
    return rename(tmp.c_str(), path.c_str()) == 0;
}

inline long long now_secs() {
    return (long long)time(nullptr);
}

inline bool remove_file(const std::string& path) {
#ifdef _WIN32
    return _unlink(path.c_str()) == 0;
#else
    return ::unlink(path.c_str()) == 0;
#endif
}

// ---------------------------------------------------------------------------
// 上下文（UI → AI）
// ---------------------------------------------------------------------------

// 读上下文；文件不存在或坏掉返回 false（AI 侧据此回答"UI 未运行"）。
inline bool load(json* out) {
    const std::string body = read_all(context_path());
    if (body.empty()) return false;
    try {
        *out = json::parse(body);
    } catch (...) {
        return false;   // 半截文件 / 被手改坏，按"没有上下文"处理
    }
    return out->is_object();
}

// UI 是否还活着。判据是 ts 新鲜度而不是进程探测 —— 跨平台、无额外依赖。
// closed:true 是 UI 正常退出时写的：文件留着（AI 仍能看到人最后在搜什么），
// 但明确标出"已经关了"，比等 ts 过期准确。
inline bool ui_alive(const json& c) {
    if (c.contains("closed") && c["closed"].is_boolean() && c["closed"].get<bool>())
        return false;
    if (!c.contains("ts") || !c["ts"].is_number()) return false;
    const long long ts = c["ts"].get<long long>();
    const long long d = now_secs() - ts;
    return d >= -2 && d <= kStaleSecs;   // 容忍时钟小幅回拨
}

inline bool save(const json& c) {
    return write_atomic(context_path(), c.dump(1, ' ', false, json::error_handler_t::replace));
}

// ---------------------------------------------------------------------------
// 指令（AI → UI）
// ---------------------------------------------------------------------------

inline bool load_cmd(json* out) {
    const std::string body = read_all(cmd_path());
    if (body.empty()) return false;
    try {
        *out = json::parse(body);
    } catch (...) {
        return false;
    }
    return out->is_object() && out->contains("id");
}

inline bool save_cmd(const json& c) {
    return write_atomic(cmd_path(), c.dump(1, ' ', false, json::error_handler_t::replace));
}

inline bool load_result(json* out) {
    const std::string body = read_all(result_path());
    if (body.empty()) return false;
    try {
        *out = json::parse(body);
    } catch (...) {
        return false;
    }
    return out->is_object();
}

inline bool save_result(const json& c) {
    return write_atomic(result_path(), c.dump(1, ' ', false, json::error_handler_t::replace));
}

}  // namespace ctx
}  // namespace gg
