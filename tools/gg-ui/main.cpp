// gg-ui — PC 侧内存搜索界面（imgui + Win32 + D3D11）
//
// 设计取向：
//   - **零第三方运行时依赖**：imgui 源码内联，窗口/D3D11 全用系统库。
//   - **不碰 agentd 协议**：全部操作走 server 的 Session::call_tool，与 MCP
//     客户端同一条路径 —— 协议改了这里不用跟着改。
//   - 配置读 configs/gg-ghost.json，与 gg-ghost-mcp.exe 同源（含 adb/serial/wire_key）。
//
// 用法：在项目根目录运行 build\pc\gg-ui.exe（配置是相对路径）。
#include <windows.h>
#include <d3d11.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "session.h"

// UI <-> AI 的共享工作上下文（文件通道，见该头文件的说明）
#include "shared_ctx.h"

// ===========================================================================
// D3D11 设备
// ===========================================================================
static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_mainRenderTargetView);
        back->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL got;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                      D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice,
                                      &got, &g_pd3dDeviceContext) != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static bool g_resize = false;
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) g_resize = true;
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ===========================================================================
// 状态
// ===========================================================================
struct ProcEntry {
    int         pid = 0;
    std::string comm;
    std::string name;
};

struct HitRow {
    uint64_t    addr = 0;
    std::string value;
    std::string value_hex;
};

struct App {
    gg::Session* sess = nullptr;

    bool        connected = false;
    int         driver_state = -1;
    std::string status;

    int         target_pid = 0;
    std::string target_name;

    std::vector<ProcEntry> procs;
    char        proc_filter[64] = "";
    int         proc_sel = -1;

    char        value_input[128] = "100";
    int         type_idx = 2;          // 0=b 1=w 2=dw 3=qw 4=float 5=double 6=bytes
    int         region_idx = 0;        // all / anonymous / writable
    int         page_limit = 1000;

    std::vector<HitRow> hits;
    int64_t     hit_total = 0;
    int         hit_sel = -1;
    // 结果分页：几万条时不可能都看，翻页比"调大显示上限再卡一下"实用。
    // 所有"重新搜索"的入口都要把 page_offset 归零，否则会停在第 N 页看不到新结果。
    int64_t     page_offset = 0;

    char        write_input[128] = "";
    int         freeze_limit = 200;

    std::vector<std::string> log;

    // ---- 与 AI 联动（协议见 server/shared_ctx.h）----
    // search_mode 是给人看的语义标签：手动搜索时 AI 只有知道"这是精确搜索还是
    // 区间搜索"，才能正确解释 value_input 里那个 "90-110" 到底是区间还是字面量。
    std::string search_mode = "none";      // none / exact / range / fuzzy
    std::string last_action = "";
    std::string applied_cmd_id;            // 已执行的指令 id，防重复执行
    double      t_pub = 0.0;               // 上次写 context.json
    double      t_cmd = 0.0;               // 上次轮询 cmd.json
    double      t_follow = 0.0;            // 上次比对设备侧结果数
    int64_t     total_seen = -1;           // 本地已知的 result_count，用于识别外部改动
    bool        ai_context = true;         // 是否对外提供上下文（关掉就不再写文件）
    int         ai_cmds_done = 0;          // 已响应的 AI 指令数（顶栏显示）
    std::string ai_note;                   // 顶栏提示：AI 刚做了什么
    double      ai_note_t = 0.0;           // 提示的过期时间
};

static App g_app;

// 定义在下方"与 AI 的联动"一节；业务函数（do_connect / attach_pid / ...）
// 都在它之前，所以先声明。必须包在匿名 namespace 里 —— 定义在匿名 namespace
// 内，全局作用域的声明会是另一个实体（匿名 namespace 里所有同名块属于同一个）。
namespace { void ui_publish(const char* action); }

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_app.log.push_back(buf);
    printf("%s\n", buf);
    if (g_app.log.size() > 400)
        g_app.log.erase(g_app.log.begin(), g_app.log.begin() + 200);
}

// ===========================================================================
// 工具层：Session::call_tool 返回 MCP 包装 {content:[{text:"<json>"}],isError}
// ===========================================================================
using nlohmann::json;

struct ToolResult {
    bool        ok = false;
    json        data;
    std::string err;
};

static ToolResult call_tool(gg::Session* sess, const char* name, const json& args) {
    ToolResult tr;
    json r = sess->call_tool(name, args);
    if (r.contains("isError") && r["isError"].is_boolean() && r["isError"].get<bool>()) {
        tr.err = r.contains("content") && r["content"].is_array() && !r["content"].empty()
                     ? r["content"][0].value("text", "unknown error")
                     : "unknown error";
        return tr;
    }
    if (!r.contains("content") || !r["content"].is_array() || r["content"].empty()) {
        tr.err = "empty response";
        return tr;
    }
    const std::string text = r["content"][0].value("text", "");
    try {
        tr.data = json::parse(text);
        if (tr.data.contains("error") && tr.data["error"].is_object()) {
            tr.err = tr.data["error"].value("message", "tool error");
            return tr;
        }
        tr.ok = true;
    } catch (const std::exception& e) {
        tr.err = std::string("bad json: ") + e.what();
    }
    return tr;
}

static const char* kTypeNames[] = {"byte", "word", "dword", "qword", "float", "double", "bytes"};
static const char* kRegionNames[] = {"all", "anonymous", "writable"};

static bool parse_num(const char* s, uint64_t* out) {
    if (!s) return false;
    while (*s == ' ') s++;
    if (!*s) return false;
    char* end = nullptr;
    unsigned long long v;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = strtoull(s + 2, &end, 16);
    else v = strtoull(s, &end, 10);
    if (!end || *end != 0) return false;
    *out = v;
    return true;
}

// ===========================================================================
// 业务
// ===========================================================================
static bool load_config(gg::ServerConfig* cfg) {
    std::ifstream f("configs/gg-ghost.json");
    if (!f) { logf("[配置] 找不到 configs/gg-ghost.json —— 请在项目根目录运行本程序"); return false; }
    json j;
    try { f >> j; } catch (const std::exception& e) { logf("[配置] 解析失败: %s", e.what()); return false; }
    auto rd = [&](const char* k, std::string& dst) {
        if (j.contains(k) && j[k].is_string()) dst = j[k].get<std::string>();
    };
    rd("adb", cfg->adb);
    rd("serial", cfg->serial);
    rd("wire_key", cfg->wire_key);
    if (j.contains("agent_port") && j["agent_port"].is_number())
        cfg->agent_port = j["agent_port"].get<int>();
    cfg->transport = "none";
    return true;
}

static void refresh_procs() {
    if (!g_app.sess) return;
    ToolResult r = call_tool(g_app.sess, "gg_list_processes", json::object());
    if (!r.ok) { logf("[进程] 获取失败: %s", r.err.c_str()); return; }
    g_app.procs.clear();
    if (r.data.contains("processes") && r.data["processes"].is_array()) {
        for (const auto& p : r.data["processes"]) {
            ProcEntry e;
            e.pid = p.value("pid", 0);
            e.comm = p.value("comm", std::string());
            e.name = p.value("name", std::string());
            if (e.name.empty()) e.name = e.comm;
            g_app.procs.push_back(e);
        }
    }
    logf("[进程] 共 %zu 个", g_app.procs.size());
}

static void do_connect() {
    gg::ServerConfig cfg;
    if (!load_config(&cfg)) return;
    if (g_app.sess) { delete g_app.sess; g_app.sess = nullptr; }
    g_app.sess = new gg::Session(cfg);

    ToolResult r = call_tool(g_app.sess, "gg_device_info", json::object());
    if (!r.ok) {
        g_app.connected = false;
        g_app.status = "连接失败";
        logf("[连接] 失败: %s", r.err.c_str());
        return;
    }
    g_app.connected = true;
    g_app.driver_state = r.data.value("driver_state", -1);
    g_app.status = r.data.value("driver_state_str", std::string("?"));
    logf("[连接] 成功 driver_state=%d (%s)", g_app.driver_state, g_app.status.c_str());
    refresh_procs();
    ui_publish("connect");
}

static void attach_pid(int pid) {
    if (!g_app.sess || pid <= 0) return;
    ToolResult r = call_tool(g_app.sess, "gg_attach", json{{"pid", pid}});
    if (!r.ok) { logf("[attach] pid=%d 失败: %s", pid, r.err.c_str()); return; }
    g_app.target_pid = pid;
    g_app.target_name = r.data.value("name", std::string());
    g_app.hits.clear();
    g_app.hit_total = 0;
    g_app.hit_sel = -1;
    g_app.search_mode = "none";     // 换进程后旧搜索条件不再成立
    g_app.total_seen = 0;
    g_app.page_offset = 0;
    logf("[attach] pid=%d %s", pid, g_app.target_name.c_str());
    ui_publish("attach");
}

static void refresh_results(int limit) {
    if (!g_app.sess) return;
    ToolResult c = call_tool(g_app.sess, "gg_result_count", json::object());
    if (c.ok) g_app.hit_total = c.data.value("count", (int64_t)0);

    // 结果数可能变小（refine / undo / 换搜索），offset 要夹回合法范围；
    // 否则会拉到空页，看起来像"结果没了"。
    if (limit <= 0) limit = 1000;
    if (g_app.page_offset >= g_app.hit_total)
        g_app.page_offset = (g_app.hit_total > 0)
                          ? ((g_app.hit_total - 1) / limit) * limit : 0;
    if (g_app.page_offset < 0) g_app.page_offset = 0;

    ToolResult q = call_tool(g_app.sess, "gg_get_results",
                             json{{"offset", g_app.page_offset}, {"limit", limit}});
    if (!q.ok) { logf("[结果] 拉取失败: %s", q.err.c_str()); return; }
    g_app.hits.clear();
    if (q.data.contains("results") && q.data["results"].is_array()) {
        for (const auto& it : q.data["results"]) {
            HitRow h;
            const std::string a = it.value("addr", std::string("0"));
            h.addr = strtoull(a.c_str(), nullptr, 0);
            h.value = it.value("value", std::string());
            h.value_hex = it.value("value_hex", std::string());
            g_app.hits.push_back(h);
        }
    }
    g_app.hit_sel = -1;
    g_app.total_seen = g_app.hit_total;   // 本地刚刷过，这个数字就是基准
    logf("[结果] 总数 %lld，本页 %zu 条（offset=%lld）", (long long)g_app.hit_total,
         g_app.hits.size(), (long long)g_app.page_offset);
}

static void do_exact_search() {
    if (!g_app.sess || g_app.target_pid == 0) { logf("[搜索] 先 attach 进程"); return; }
    uint64_t v = 0;
    if (!parse_num(g_app.value_input, &v)) { logf("[搜索] 值非法: %s", g_app.value_input); return; }
    g_app.page_offset = 0;      // 新结果从第一页看
    ToolResult r = call_tool(g_app.sess, "gg_search",
                             json{{"value", g_app.value_input},
                                  {"type", kTypeNames[g_app.type_idx]},
                                  {"region", kRegionNames[g_app.region_idx]}});
    if (!r.ok) { logf("[搜索] 失败: %s", r.err.c_str()); return; }
    g_app.search_mode = "exact";
    logf("[搜索] %s=%s -> %lld 条%s", kTypeNames[g_app.type_idx], g_app.value_input,
         (long long)r.data.value("count", (int64_t)0),
         r.data.value("truncated", false) ? "（已截断）" : "");
    refresh_results(g_app.page_limit);
    ui_publish("exact_search");
}

static void do_range_search() {
    if (!g_app.sess || g_app.target_pid == 0) { logf("[区间] 先 attach 进程"); return; }
    g_app.page_offset = 0;
    uint64_t lo = 0, hi = 0;
    // 值输入支持 "90-110" 形式
    std::string s = g_app.value_input;
    size_t dash = s.find('-');
    if (dash == std::string::npos) { logf("[区间] 需要 \"min-max\"，例如 90-110"); return; }
    std::string a = s.substr(0, dash), b = s.substr(dash + 1);
    if (!parse_num(a.c_str(), &lo) || !parse_num(b.c_str(), &hi)) {
        logf("[区间] 数值非法: %s", s.c_str());
        return;
    }
    ToolResult r = call_tool(g_app.sess, "gg_search_range",
                             json{{"min", a}, {"max", b},
                                  {"type", kTypeNames[g_app.type_idx]},
                                  {"region", kRegionNames[g_app.region_idx]}});
    if (!r.ok) { logf("[区间] 失败: %s", r.err.c_str()); return; }
    g_app.search_mode = "range";
    logf("[区间] %s-%s -> %lld 条", a.c_str(), b.c_str(),
         (long long)r.data.value("count", (int64_t)0));
    refresh_results(g_app.page_limit);
    ui_publish("range_search");
}

static void do_unknown_init() {
    if (!g_app.sess || g_app.target_pid == 0) { logf("[模糊] 先 attach 进程"); return; }
    g_app.page_offset = 0;
    ToolResult r = call_tool(g_app.sess, "gg_search_unknown_init",
                             json{{"type", kTypeNames[g_app.type_idx]},
                                  {"region", kRegionNames[g_app.region_idx]}});
    if (!r.ok) { logf("[模糊] 首次扫描失败: %s", r.err.c_str()); return; }
    logf("[模糊] 首次扫描完成，%lld 个元素 —— 改数值后点\"再次扫描\"",
         (long long)r.data.value("elements", (int64_t)0));
    g_app.hits.clear();
    g_app.hit_total = 0;
    ui_publish("fuzzy_init");
}

static void do_refine(const char* op) {
    if (!g_app.sess) return;
    ToolResult r = call_tool(g_app.sess, "gg_search_refine", json{{"op", op}});
    if (!r.ok) { logf("[再次扫描 %s] 失败: %s", op, r.err.c_str()); return; }
    int64_t remain = r.data.value("remaining", (int64_t)0);
    logf("[再次扫描 %s] 剩余 %lld", op, (long long)remain);
    ui_publish("refine");
    if (remain > 0 && remain <= 100000) refresh_results(g_app.page_limit);
    else { g_app.hits.clear(); g_app.hit_total = remain; }
}

static void do_undo() {
    if (!g_app.sess) return;
    ToolResult r = call_tool(g_app.sess, "gg_search_undo", json::object());
    if (!r.ok) { logf("[撤销] 失败: %s", r.err.c_str()); return; }
    logf("[撤销] 恢复到 %lld", (long long)r.data.value("remaining", (int64_t)0));
    ui_publish("undo");
}

static void do_clear() {
    if (!g_app.sess) return;
    call_tool(g_app.sess, "gg_clear_results", json::object());
    g_app.hits.clear();
    g_app.hit_total = 0;
    g_app.total_seen = 0;
    g_app.page_offset = 0;
    g_app.search_mode = "none";
    logf("[清空]");
    ui_publish("clear");
}

static void do_read_one(uint64_t addr) {
    if (!g_app.sess) return;
    char a[32];
    snprintf(a, sizeof(a), "0x%llx", (unsigned long long)addr);
    ToolResult r = call_tool(g_app.sess, "gg_read",
                             json{{"addr", a}, {"type", kTypeNames[g_app.type_idx]}});
    if (!r.ok) { logf("[读] %s 失败: %s", a, r.err.c_str()); return; }
    logf("[读] %s = %lld (0x%s)", a, (long long)r.data.value("value", (int64_t)0),
         r.data.value("value_hex", std::string("?")).c_str());
}

static void do_write(uint64_t addr) {
    if (!g_app.sess) return;
    uint64_t v = 0;
    if (!parse_num(g_app.write_input, &v)) { logf("[写] 值非法: %s", g_app.write_input); return; }
    char a[32];
    snprintf(a, sizeof(a), "0x%llx", (unsigned long long)addr);
    ToolResult r = call_tool(g_app.sess, "gg_write",
                             json{{"addr", a}, {"type", kTypeNames[g_app.type_idx]},
                                  {"value", g_app.write_input}});
    if (!r.ok) { logf("[写] %s 失败: %s", a, r.err.c_str()); return; }
    logf("[写] %s <- %s（%d 字节）", a, g_app.write_input, r.data.value("bytes", 0));
}

static void do_freeze(uint64_t addr, const char* val) {
    if (!g_app.sess) return;
    char a[32];
    snprintf(a, sizeof(a), "0x%llx", (unsigned long long)addr);
    json args = {{"addr", a}, {"type", kTypeNames[g_app.type_idx]}};
    if (val && *val) args["value"] = val;
    ToolResult r = call_tool(g_app.sess, "gg_freeze", args);
    if (!r.ok) { logf("[冻结] %s 失败: %s", a, r.err.c_str()); return; }
    logf("[冻结] %s = %s", a, (val && *val) ? val : "(当前值)");
}

static void do_unfreeze(uint64_t addr) {
    if (!g_app.sess) return;
    char a[32];
    snprintf(a, sizeof(a), "0x%llx", (unsigned long long)addr);
    ToolResult r = call_tool(g_app.sess, "gg_unfreeze", json{{"addr", a}});
    if (!r.ok) { logf("[解冻] %s 失败: %s", a, r.err.c_str()); return; }
    logf("[解冻] %s", a);
}

// 批量冻结：搜索结果往往是"同一个值的多个副本"，真正要冻的是全部。
// 逐个 freeze（驱动侧一条命令一次），带上限防止误点把设备冻死。
static void do_freeze_all() {
    if (!g_app.sess || g_app.hits.empty()) { logf("[批量冻结] 没有结果"); return; }
    int n = (int)g_app.hits.size();
    if (n > g_app.freeze_limit) n = g_app.freeze_limit;
    if (!g_app.write_input[0]) { logf("[批量冻结] 先在\"写入值\"里填目标值"); return; }
    int okn = 0;
    for (int i = 0; i < n; i++) {
        char a[32];
        snprintf(a, sizeof(a), "0x%llx", (unsigned long long)g_app.hits[i].addr);
        ToolResult r = call_tool(g_app.sess, "gg_freeze",
                                 json{{"addr", a}, {"type", kTypeNames[g_app.type_idx]},
                                      {"value", g_app.write_input}});
        if (r.ok) okn++;
    }
    logf("[批量冻结] %d/%d 成功（值=%s）", okn, n, g_app.write_input);
}

// 导出结果：搜到地址之后通常要拿去别处用（脚本、CE、自己的分析工具）。
// 两种格式：
//   csv —— 地址,值,hex（带表头，Excel 直接开）
//   纯地址 —— 每行一个 0x...（喂给批量读/批量写脚本最方便）
static void do_export(bool csv) {
    if (g_app.hits.empty()) { logf("[导出] 没有结果"); return; }
    const char* path = csv ? "gg-results.csv" : "gg-results.txt";
    FILE* f = fopen(path, "w");
    if (!f) { logf("[导出] 打不开 %s", path); return; }
    if (csv) fprintf(f, "addr,value,value_hex\n");
    for (const auto& h : g_app.hits)
        fprintf(f, csv ? "0x%llx,%s,%s\n" : "0x%llx\n",
                (unsigned long long)h.addr, h.value.c_str(), h.value_hex.c_str());
    fclose(f);
    logf("[导出] %zu 条 -> %s", g_app.hits.size(), path);
}

// 复制全部地址到剪贴板（imgui 的 backend 已接管剪贴板，跨进程可用）
static void do_copy_addrs() {
    if (g_app.hits.empty()) { logf("[复制] 没有结果"); return; }
    std::string out;
    out.reserve(g_app.hits.size() * 14);
    char buf[32];
    for (const auto& h : g_app.hits) {
        snprintf(buf, sizeof(buf), "0x%llx\n", (unsigned long long)h.addr);
        out += buf;
    }
    ImGui::SetClipboardText(out.c_str());
    logf("[复制] %zu 个地址已进剪贴板", g_app.hits.size());
}

// ===========================================================================
// 与 AI 的联动
//
// agentd 的 g_eng 是全局的，所以「我在这边搜」和「AI 在那边搜」用的是同一份
// 结果集。要补的只是"对方在干什么"，走 build/gg-shared/ 下的两个文件：
//
//   context.json  ← 我写（attach / 搜索 / 选中 / 每 400ms 心跳）
//   cmd.json      → 我读（AI 下发的动作，执行完删除并回 cmd_result.json）
// ===========================================================================
namespace {

static int type_from_name(const std::string& s) {
    for (int i = 0; i < (int)(sizeof(kTypeNames) / sizeof(kTypeNames[0])); i++)
        if (s == kTypeNames[i]) return i;
    return 2;   // dword
}

static int region_from_name(const std::string& s) {
    for (int i = 0; i < (int)(sizeof(kRegionNames) / sizeof(kRegionNames[0])); i++)
        if (s == kRegionNames[i]) return i;
    return 0;   // all
}

// 把当前状态写出去。action 非空时同时更新 last_action（AI 靠它判断人刚做了什么）。
void ui_publish(const char* action) {
    if (!g_app.ai_context) return;
    if (action) g_app.last_action = action;

    json c;
    c["schema"] = 1;
    c["ts"] = gg::ctx::now_secs();
    c["ui_pid"] = (int)GetCurrentProcessId();
    c["connected"] = g_app.connected;
    c["driver_state"] = g_app.driver_state;
    c["last_action"] = g_app.last_action;

    c["target"] = json{{"pid", g_app.target_pid}, {"name", g_app.target_name}};

    // search.value 是用户输入框里的原文；mode 说明怎么解释它
    // （range 时它是 "90-110" 这样的区间写法，不是字面量）。
    c["search"] = json{{"mode", g_app.search_mode},
                       {"value", std::string(g_app.value_input)},
                       {"type", kTypeNames[g_app.type_idx]},
                       {"region", kRegionNames[g_app.region_idx]}};

    c["results"] = json{{"total", g_app.hit_total}, {"loaded", (int64_t)g_app.hits.size()}};

    if (g_app.hit_sel >= 0 && g_app.hit_sel < (int)g_app.hits.size()) {
        const HitRow& h = g_app.hits[g_app.hit_sel];
        char a[32];
        snprintf(a, sizeof(a), "0x%llx", (unsigned long long)h.addr);
        c["selected"] = json{{"addr", std::string(a)},
                             {"value", h.value},
                             {"value_hex", h.value_hex}};
    } else {
        c["selected"] = nullptr;
    }

    // 日志尾部：AI 读这几行就知道人遇到过什么报错，比自己复现快。
    json lt = json::array();
    const size_t keep = 8;
    for (size_t i = g_app.log.size() > keep ? g_app.log.size() - keep : 0; i < g_app.log.size(); ++i)
        lt.push_back(g_app.log[i]);
    c["log_tail"] = lt;

    gg::ctx::save(c);
}

// 执行一条 AI 下发的指令。返回的 json 会写进 cmd_result.json。
static json ui_run_cmd(const json& cmd) {
    const std::string op = cmd.value("op", "");
    const json a = cmd.contains("args") && cmd["args"].is_object() ? cmd["args"] : json::object();
    json out;
    out["id"] = cmd.value("id", std::string());
    out["op"] = op;

    auto need_session = [&]() -> bool {
        if (!g_app.sess || !g_app.connected) {
            out["ok"] = false;
            out["err"] = "gg-ui 尚未连接设备";
            return false;
        }
        return true;
    };

    if (op == "attach") {
        if (!need_session()) return out;
        const int pid = a.value("pid", 0);
        if (pid <= 0) { out["ok"] = false; out["err"] = "args.pid 必须是正整数"; return out; }
        attach_pid(pid);
        out["ok"] = g_app.target_pid == pid;
        out["data"] = json{{"pid", g_app.target_pid}, {"name", g_app.target_name}};
        if (!out["ok"].get<bool>()) out["err"] = "attach 失败（见日志）";
        ui_publish("ai_attach");
        return out;
    }

    if (op == "search" || op == "search_range") {
        if (!need_session()) return out;
        if (g_app.target_pid == 0) { out["ok"] = false; out["err"] = "还没 attach 目标进程"; return out; }
        // 参数写进输入框再走已有函数：这样界面上的值也跟着变，人能看到 AI 搜了什么。
        const std::string v = a.value("value", a.contains("min")
                                      ? (a.value("min", std::string()) + "-" + a.value("max", std::string()))
                                      : std::string());
        if (v.empty()) { out["ok"] = false; out["err"] = "args.value 不能为空"; return out; }
        snprintf(g_app.value_input, sizeof(g_app.value_input), "%s", v.c_str());
        g_app.type_idx = type_from_name(a.value("type", std::string("dword")));
        g_app.region_idx = region_from_name(a.value("region", std::string("all")));
        if (op == "search_range" || a.contains("min")) do_range_search();
        else do_exact_search();
        out["ok"] = true;
        out["data"] = json{{"total", g_app.hit_total}, {"loaded", (int64_t)g_app.hits.size()}};
        ui_publish(op == "search_range" ? "ai_range_search" : "ai_exact_search");
        return out;
    }

    if (op == "fuzzy_init") {
        if (!need_session()) return out;
        if (g_app.target_pid == 0) { out["ok"] = false; out["err"] = "还没 attach 目标进程"; return out; }
        g_app.type_idx = type_from_name(a.value("type", std::string("dword")));
        g_app.region_idx = region_from_name(a.value("region", std::string("all")));
        do_unknown_init();
        out["ok"] = true;
        out["data"] = json{{"total", g_app.hit_total}};
        ui_publish("ai_fuzzy_init");
        return out;
    }

    if (op == "refine") {
        if (!need_session()) return out;
        const std::string r = a.value("op", std::string("unchanged"));
        do_refine(r.c_str());
        out["ok"] = true;
        out["data"] = json{{"total", g_app.hit_total}};
        ui_publish("ai_refine");
        return out;
    }

    if (op == "select") {
        const std::string as = a.value("addr", std::string());
        uint64_t want = 0;
        if (!parse_num(as.c_str(), &want)) {
            out["ok"] = false; out["err"] = "args.addr 必须是数值或 0x 字符串"; return out;
        }
        int found = -1;
        for (int i = 0; i < (int)g_app.hits.size(); ++i)
            if (g_app.hits[i].addr == want) { found = i; break; }
        if (found < 0) {
            out["ok"] = false;
            out["err"] = "该地址不在当前已加载的结果页里（可以先用 gg_ui_command op=search 让 UI 重新搜）";
            return out;
        }
        g_app.hit_sel = found;
        out["ok"] = true;
        out["data"] = json{{"addr", as}, {"value", g_app.hits[found].value}};
        ui_publish("ai_select");
        return out;
    }

    if (op == "read") {
        if (!need_session()) return out;
        const std::string as = a.value("addr", std::string());
        uint64_t addr = 0;
        if (!parse_num(as.c_str(), &addr)) {
            out["ok"] = false; out["err"] = "args.addr 必须是数值或 0x 字符串"; return out;
        }
        do_read_one(addr);
        out["ok"] = true;
        out["data"] = json{{"addr", as}};
        ui_publish("ai_read");
        return out;
    }

    if (op == "export") {
        do_export(a.value("csv", true));
        out["ok"] = true;
        out["data"] = json{{"file", a.value("csv", true) ? "gg-results.csv" : "gg-results.txt"}};
        ui_publish("ai_export");
        return out;
    }

    if (op == "refresh") {
        if (!need_session()) return out;
        refresh_procs();
        if (g_app.target_pid) refresh_results(g_app.page_limit);
        out["ok"] = true;
        ui_publish("ai_refresh");
        return out;
    }

    out["ok"] = false;
    out["err"] = "未知 op: " + op;
    return out;
}

// 主循环每帧调用。三个节拍分开，避免一次做三件重活：
//   400ms 心跳（写 ts，让 AI 知道我还活着）
//   200ms 取指令（人等着看结果，要紧一点）
//   900ms 跟随（比对设备侧结果数，发现 AI 改过就自动刷新）
static void ui_tick() {
    const double now = ImGui::GetTime();

    if (now - g_app.t_pub > 0.4) {
        g_app.t_pub = now;
        ui_publish(nullptr);
    }

    if (now - g_app.t_cmd > 0.2) {
        g_app.t_cmd = now;
        json cmd;
        if (gg::ctx::load_cmd(&cmd)) {
            const std::string id = cmd.value("id", std::string());
            // 30 秒前的指令不执行：UI 可能刚被打开，队列里躺着上一次会话的遗留。
            const long long age = gg::ctx::now_secs() - cmd.value("ts", (long long)0);
            if (id != g_app.applied_cmd_id && age <= 30) {
                g_app.applied_cmd_id = id;
                json res = ui_run_cmd(cmd);
                gg::ctx::save_result(res);
                g_app.ai_cmds_done++;
                g_app.ai_note = "AI 下发了 " + cmd.value("op", std::string("?")) +
                                (res.value("ok", false) ? "（已执行）" : "（失败）");
                g_app.ai_note_t = now;
                logf("[AI] 执行 %s%s", cmd.value("op", "?").c_str(),
                     res.value("ok", false) ? "" : " —— 失败");
            }
            gg::ctx::remove_file(gg::ctx::cmd_path());
        }
    }

    // 自动跟随：AI 那边搜过之后设备侧结果集会变，我这边不刷新就还是旧表格 ——
    // 人会以为 AI 没干活。只比对数字，不主动拉全量（拉 1000 条要几十毫秒）。
    if (g_app.connected && g_app.sess && now - g_app.t_follow > 0.9) {
        g_app.t_follow = now;
        ToolResult r = call_tool(g_app.sess, "gg_result_count", json::object());
        if (r.ok) {
            const int64_t remote = r.data.value("count", (int64_t)0);
            if (g_app.total_seen >= 0 && remote != g_app.total_seen) {
                g_app.ai_note = "结果集被外部改动（" + std::to_string(g_app.total_seen) +
                                " → " + std::to_string(remote) + "），已同步";
                g_app.ai_note_t = now;
                logf("[同步] 结果集外部变化 %lld -> %lld，自动刷新",
                     (long long)g_app.total_seen, (long long)remote);
                refresh_results(g_app.page_limit);
            }
            g_app.total_seen = remote;
            g_app.hit_total = remote;   // 顶上显示的数字也跟着对
        }
    }
}

}  // namespace

// ===========================================================================
// 界面
// ===========================================================================
static void draw_top_bar() {
    ImGui::TextUnformatted("GG-UI");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button("连接 / 重连")) do_connect();
    ImGui::SameLine();
    if (g_app.connected) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.4f, 1.0f), "● %s", g_app.status.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "● 未连接");
    }
    ImGui::SameLine();
    if (g_app.target_pid) {
        ImGui::TextDisabled("目标: %s (%d)", g_app.target_name.c_str(), g_app.target_pid);
    } else {
        ImGui::TextDisabled("目标: 未选择");
    }

    // AI 联动开关。开着的时候每次搜索/选中都会写进 build/gg-shared/context.json，
    // AI 那边调 gg_context 就能读到"人搜的是什么"。关掉会删文件，AI 立刻看不到。
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (g_app.ai_context) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.4f, 1.0f), "AI 联动 开");
    } else {
        ImGui::TextDisabled("AI 联动 关");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("共享当前目标 / 搜索值 / 选中行给 AI（build/gg-shared/context.json），"
                          "AI 用 gg_context 读、gg_ui_command 反向驱动本窗口。\n点击切换。");
    if (ImGui::IsItemClicked()) {
        g_app.ai_context = !g_app.ai_context;
        if (g_app.ai_context) {
            ui_publish("toggle_on");
            logf("[AI] 联动已开启 —— AI 现在能读到你搜的值和选中的行");
        } else {
            gg::ctx::remove_file(gg::ctx::context_path());
            logf("[AI] 联动已关闭（context.json 已删除，AI 读到的是\"未运行\"）");
        }
    }
    if (g_app.ai_cmds_done > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(已响应 %d 条)", g_app.ai_cmds_done);
    }

    ImGui::SameLine(ImGui::GetWindowWidth() - 130);
    if (ImGui::Button("刷新进程列表")) refresh_procs();
    ImGui::SameLine();
    ImGui::TextDisabled("F1");
}

static void draw_proc_panel() {
    ImGui::BeginChild("procs", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::InputTextWithHint("##filter", "过滤进程名 / pid", g_app.proc_filter,
                             sizeof(g_app.proc_filter));
    ImGui::Separator();
    const std::string filt = g_app.proc_filter;
    if (ImGui::BeginTable("proc_tbl", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("pid", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("进程", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        int shown = 0;
        for (int i = 0; i < (int)g_app.procs.size() && shown < 4000; i++) {
            const ProcEntry& e = g_app.procs[i];
            char pidbuf[16];
            snprintf(pidbuf, sizeof(pidbuf), "%d", e.pid);
            bool match = filt.empty() || e.comm.find(filt) != std::string::npos ||
                         e.name.find(filt) != std::string::npos || std::string(pidbuf) == filt;
            if (!match) continue;
            shown++;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(pidbuf);
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(e.pid);
            bool sel = (g_app.proc_sel == e.pid);
            if (ImGui::Selectable(e.comm.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
                g_app.proc_sel = e.pid;
                attach_pid(e.pid);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

static void draw_search_panel() {
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##value", "值（支持 0x 前缀；区间用 90-110）", g_app.value_input,
                             sizeof(g_app.value_input));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##type", &g_app.type_idx, kTypeNames, IM_ARRAYSIZE(kTypeNames));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##region", &g_app.region_idx, kRegionNames, IM_ARRAYSIZE(kRegionNames));
    ImGui::SameLine();
    if (ImGui::Button("精确搜索")) do_exact_search();
    ImGui::SameLine();
    if (ImGui::Button("区间搜索")) do_range_search();

    if (ImGui::Button("模糊首次扫描")) do_unknown_init();
    ImGui::SameLine();
    ImGui::TextDisabled("然后改数值，再点：");
    ImGui::SameLine();
    if (ImGui::Button("变")) do_refine("changed");
    ImGui::SameLine();
    if (ImGui::Button("不变")) do_refine("unchanged");
    ImGui::SameLine();
    if (ImGui::Button("增")) do_refine("increased");
    ImGui::SameLine();
    if (ImGui::Button("减")) do_refine("decreased");
    ImGui::SameLine();
    if (ImGui::Button("撤销")) do_undo();
    ImGui::SameLine();
    if (ImGui::Button("清空结果")) do_clear();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    const char* limits[] = {"200", "500", "1000", "5000", "20000"};
    const int limit_vals[] = {200, 500, 1000, 5000, 20000};
    int li = 2;
    for (int i = 0; i < 5; i++) if (limit_vals[i] == g_app.page_limit) li = i;
    if (ImGui::Combo("##lim", &li, limits, 5)) g_app.page_limit = limit_vals[li];
    ImGui::SameLine();
    ImGui::TextDisabled("显示上限");

    // AI 刚做了什么 / 结果集被外部改了 —— 不弹窗，只在搜索区下方留一句，
    // 人的视线就在附近，能看到但不打断操作。6 秒后自动消失。
    if (!g_app.ai_note.empty() && ImGui::GetTime() - g_app.ai_note_t < 6.0) {
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.32f, 1.0f), "AI: %s", g_app.ai_note.c_str());
    }
}

static void draw_results() {
    ImGui::Text("结果 %lld 条", (long long)g_app.hit_total);
    if (g_app.hit_total > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("| 第 %lld - %lld", (long long)g_app.page_offset + 1,
                            (long long)(g_app.page_offset + (int64_t)g_app.hits.size()));
        ImGui::SameLine();
        ImGui::BeginDisabled(g_app.page_offset <= 0);
        if (ImGui::Button("< 上页")) {
            g_app.page_offset -= g_app.page_limit;
            refresh_results(g_app.page_limit);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(g_app.page_offset + g_app.page_limit >= g_app.hit_total);
        if (ImGui::Button("下页 >")) {
            g_app.page_offset += g_app.page_limit;
            refresh_results(g_app.page_limit);
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("（单击选中，双击=按当前类型读值）");
    ImGui::SameLine(ImGui::GetWindowWidth() - 400);
    if (ImGui::Button("重新拉取")) refresh_results(g_app.page_limit);
    ImGui::SameLine();
    if (ImGui::Button("复制地址")) do_copy_addrs();
    ImGui::SameLine();
    if (ImGui::Button("导出 CSV")) do_export(true);
    ImGui::SameLine();
    if (ImGui::Button("导出地址")) do_export(false);

    if (ImGui::BeginTable("hits", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_NoHostExtendX)) {
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableSetupColumn("值（十进制）", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("hex", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)g_app.hits.size(); i++) {
            const HitRow& h = g_app.hits[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char abuf[32];
            snprintf(abuf, sizeof(abuf), "0x%llx", (unsigned long long)h.addr);
            ImGui::PushID(i);
            bool sel = (g_app.hit_sel == i);
            if (ImGui::Selectable(abuf, sel, ImGuiSelectableFlags_SpanAllColumns |
                                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                g_app.hit_sel = i;
                if (ImGui::IsMouseDoubleClicked(0)) do_read_one(h.addr);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(h.value.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(h.value_hex.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static void draw_actions() {
    const bool has_sel = (g_app.hit_sel >= 0 && g_app.hit_sel < (int)g_app.hits.size());
    const uint64_t sel_addr = has_sel ? g_app.hits[g_app.hit_sel].addr : 0;

    ImGui::TextDisabled("选中:");
    ImGui::SameLine();
    if (has_sel) ImGui::Text("0x%llx", (unsigned long long)sel_addr);
    else ImGui::TextDisabled("（无）");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##wv", "写入/冻结的值", g_app.write_input, sizeof(g_app.write_input));

    ImGui::SameLine();
    ImGui::BeginDisabled(!has_sel);
    if (ImGui::Button("写入")) do_write(sel_addr);
    ImGui::SameLine();
    if (ImGui::Button("冻结")) do_freeze(sel_addr, g_app.write_input);
    ImGui::SameLine();
    if (ImGui::Button("解冻")) do_unfreeze(sel_addr);
    ImGui::SameLine();
    if (ImGui::Button("读值")) do_read_one(sel_addr);
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(g_app.hits.empty());
    if (ImGui::Button("批量冻结前 N 条")) do_freeze_all();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::DragInt("##fl", &g_app.freeze_limit, 10.0f, 1, 5000, "N=%d");
}

static void draw_log(float height) {
    ImGui::BeginChild("log", ImVec2(0, height), ImGuiChildFlags_Borders);
    for (const auto& l : g_app.log) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

static void draw_ui() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##root", nullptr, flags);

    draw_top_bar();
    ImGui::Separator();

    const float log_h = 116.0f;
    const float content_h = ImGui::GetContentRegionAvail().y - log_h - 8.0f;

    ImGui::BeginChild("left", ImVec2(300, content_h), ImGuiChildFlags_None);
    draw_proc_panel();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("right", ImVec2(0, content_h), ImGuiChildFlags_None);
    draw_search_panel();
    ImGui::Separator();
    draw_actions();
    ImGui::Separator();
    draw_results();
    ImGui::EndChild();

    ImGui::Separator();
    draw_log(log_h);

    ImGui::End();
}

// ===========================================================================
// main
// ===========================================================================
static const char* g_font_loaded = "(none)";

static void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    // 中文必须显式加载字体：imgui 内置的 ProggyClean 只有 ASCII，中文全是方块。
    // 字体文件优先用 build/ 下的副本（本机直接读 C:\Windows\Fonts 的 ttc 会失败）。
    const char* candidates[] = {
        "build\\msyh.ttc",
        "build\\Deng.ttf",
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\Deng.ttf",
        "C:\\Windows\\Fonts\\simfang.ttf",
    };

    // 字形范围用 Builder 手动构建：Latin 基础集 + 界面里出现的所有中文。
    // 不直接用 GetGlyphRangesChineseSimplifiedCommon()：imgui 1.93 下实测
    // 传这个指针 glyph 没被烘进图集，界面仍旧全是方块；Builder 这条路不依赖
    // 那个 API 的内部行为。ranges 必须 static —— AddFont 之后到 Build 之间
    // 才读它，局部变量会悬垂。
    static const char* kUiZh =
        "连接重连未目标选择刷新进程列表过滤名值支持前缀区间用精确搜索模糊首次扫描"
        "然后改数再点变不增减撤销清空结果显示上限地址冻结解冻写入读批量前条选中"
        "无日志帮助中文字体位加载失败已断开始正在结果本页总数命中截断错误非法需"
        "要先格式例如当前可用驱动状态中请稍候成功恢复剩余元素个数字节内存地址"
        "进程名可读写匿名区域全部值是设置操作取消确认";
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesDefault());
        // 再把"常用 2500 字"整段加进来：手写 kUiZh 必然漏字（第一版就漏了
        // "模糊/首次/进程"里的字，界面上是方块）。2500 字 @17px 的图集约
        // 2048x2048，D3D11 完全吃得下。
        b.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        b.AddText(kUiZh);
        b.BuildRanges(&ranges);
    }

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    for (const char* path : candidates) {
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, 17.0f, &cfg, ranges.Data);
        if (f) {
            g_font_loaded = path;
            printf("[字体] %s (glyph ranges=%d)\n", path, (int)ranges.Size);
            return;
        }
    }
    io.Fonts->AddFontDefault();
    printf("[字体] 未找到中文字体，界面中文会显示为方块\n");
}

int main(int argc, char** argv) {
    SetProcessDPIAware();

    // 命令行：--pid <n>  启动即 attach（省得每次在列表里找）；
    //         带上 --search 时再自动按当前值/类型搜一次，方便脚本化验证。
    int  auto_pid = 0;
    int  auto_page = 0;
    bool auto_search = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pid") && i + 1 < argc) auto_pid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) auto_page = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--search")) auto_search = true;
        else if (!strcmp(argv[i], "--value") && i + 1 < argc)
            snprintf(g_app.value_input, sizeof(g_app.value_input), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--type") && i + 1 < argc) {
            for (int t = 0; t < (int)(sizeof(kTypeNames) / sizeof(kTypeNames[0])); t++)
                if (!strcmp(argv[i + 1], kTypeNames[t])) { g_app.type_idx = t; break; }
            i++;
        }
    }

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                      GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
                      L"GG-UI", nullptr};
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"GG-UI — 内存搜索",
                                WS_OVERLAPPEDWINDOW, 80, 60, 1280, 820,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { printf("CreateWindow failed\n"); return 1; }
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        printf("D3D11 init failed\n");
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "gg-ui.ini";     // 窗口布局落盘，下次启动保持
    ImGui::StyleColorsDark();
    load_fonts();
    {
        char title[512];
        const int n = (int)io.Fonts->Fonts.Size;
        snprintf(title, sizeof(title), "GG-UI - font=%s nfonts=%d",
                 g_font_loaded, n);
        SetWindowTextA(hwnd, title);
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 启动就尝试一次连接：adb forward + wire 握手约 1~2s，失败也只是显示"未连接"，
    // 用户可以再点"连接 / 重连"。省掉每次开程序都要点一下。
    do_connect();
    if (auto_pid > 0) {
        attach_pid(auto_pid);
        if (auto_search) do_exact_search();
    }
    // --page N：搜索后直接跳到第 N 页（0 = 第一页）。给脚本和截图验证用，
    // 手点翻页和这条路走的是同一个 refresh_results。
    if (auto_page > 0 && g_app.hit_total > 0) {
        g_app.page_offset = (int64_t)auto_page * g_app.page_limit;
        refresh_results(g_app.page_limit);
    }

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_resize) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
            g_resize = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ui_tick();      /* 与 AI 的联动：心跳 / 取指令 / 跟随外部结果集变化 */
        draw_ui();
        ImGui::Render();

        const ImVec4 clear(0.08f, 0.08f, 0.09f, 1.0f);
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, (float*)&clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // 标记为已关闭而不是删文件：AI 那边 gg_context 仍能看到"人最后在搜什么"，
    // 只是 running=false（shared_ctx::ui_alive 会先看 closed）。
    if (g_app.ai_context) {
        json c;
        if (gg::ctx::load(&c)) {
            c["closed"] = true;
            c["ts"] = gg::ctx::now_secs();
            gg::ctx::save(c);
        }
        gg::ctx::remove_file(gg::ctx::cmd_path());
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
