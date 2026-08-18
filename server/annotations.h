// annotations.h — 轻量标注库（JSON 文件持久化）
//
// AI 逆向分析时每发现一个字段（金币/血量/坐标…）就保存一条标注，
// 后续会话/调用通过 gg_annotate_context 把标注作为上下文注入，
// 实现跨会话的"记忆"。
//
// 数据模型: {"annotations": {"<tag>": {...}}}
// 标注字段: tag/description/notes/address/module/module_offset/type/size/
//           frozen/confidence/last_value/pkg/pid/created/updated
#pragma once

#include <mutex>
#include <string>

#include "../third_party/json.hpp"

namespace gg {

using json = nlohmann::json;

class AnnotationStore {
public:
    explicit AnnotationStore(std::string path) : path_(std::move(path)) { load(); }

    /* 添加/更新标注（按 tag 去重: 已存在则合并字段并更新 updated）。
     * a 至少含 "tag"。返回保存后的标注。 */
    json add(const json& a);

    /* 列出标注（tag 为空=全部）。返回 {"annotations":[...]} */
    json list(const std::string& tag);

    /* 取单个标注；不存在返回 null。 */
    json get(const std::string& tag);

    /* 删除标注。返回是否删除。 */
    bool remove(const std::string& tag);

    /* 生成 AI 上下文文本（纯文本摘要, 供会话注入）。 */
    std::string context_text(const std::string& tag);

    /* 数量 */
    size_t count();

private:
    void load();
    void save();
    std::string now_iso();

    std::string path_;
    json data_ = json::object();
    std::mutex mtx_;
};

}  // namespace gg
