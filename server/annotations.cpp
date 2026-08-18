// annotations.cpp — 标注库实现（JSON 文件持久化）
#include "annotations.h"

#include <chrono>
#include <cstdio>
#include <fstream>

namespace gg {

void AnnotationStore::load() {
    std::ifstream f(path_);
    if (!f.is_open()) {
        data_ = json::object();
        return;
    }
    try {
        f >> data_;
    } catch (...) {
        data_ = json::object();
    }
    if (!data_.is_object() || !data_.contains("annotations") ||
        !data_["annotations"].is_object()) {
        data_ = json::object();
        data_["annotations"] = json::object();
    }
}

void AnnotationStore::save() {
    std::ofstream f(path_, std::ios::trunc);
    if (!f.is_open()) return;
    f << data_.dump(2);
    f.close();
}

std::string AnnotationStore::now_iso() {
    auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
    return buf;
}

json AnnotationStore::add(const json& a) {
    std::lock_guard<std::mutex> lk(mtx_);
    json& ann = data_["annotations"];
    std::string tag = a.value("tag", "");
    if (tag.empty()) return json();
    const std::string now = now_iso();

    if (ann.contains(tag)) {
        json& e = ann[tag];
        for (auto it = a.begin(); it != a.end(); ++it) {
            if (it.key() == "tag" || it.key() == "created") continue;
            e[it.key()] = it.value();
        }
        e["updated"] = now;
    } else {
        json e = a;
        e["created"] = now;
        e["updated"] = now;
        ann[tag] = e;
    }
    save();
    return ann[tag];
}

json AnnotationStore::list(const std::string& tag) {
    std::lock_guard<std::mutex> lk(mtx_);
    json out = json::object();
    out["count"] = 0;
    out["annotations"] = json::array();
    json& ann = data_["annotations"];
    if (!tag.empty()) {
        if (ann.contains(tag)) {
            out["annotations"].push_back(ann[tag]);
            out["count"] = 1;
        }
        return out;
    }
    for (auto it = ann.begin(); it != ann.end(); ++it) {
        out["annotations"].push_back(it.value());
    }
    out["count"] = (int)ann.size();
    return out;
}

json AnnotationStore::get(const std::string& tag) {
    std::lock_guard<std::mutex> lk(mtx_);
    json& ann = data_["annotations"];
    if (ann.contains(tag)) return ann[tag];
    return json();
}

bool AnnotationStore::remove(const std::string& tag) {
    std::lock_guard<std::mutex> lk(mtx_);
    json& ann = data_["annotations"];
    if (!ann.contains(tag)) return false;
    ann.erase(tag);
    save();
    return true;
}

std::string AnnotationStore::context_text(const std::string& tag) {
    std::lock_guard<std::mutex> lk(mtx_);
    json& ann = data_["annotations"];
    std::string out = "## 已知字段标注（跨会话记忆）\n";
    if (ann.empty()) {
        out += "（暂无标注）\n";
        return out;
    }
    int shown = 0;
    for (auto it = ann.begin(); it != ann.end(); ++it) {
        if (!tag.empty() && it.key() != tag) continue;
        const json& e = it.value();
        out += "- [" + it.key() + "] ";
        if (e.contains("description") && e["description"].is_string())
            out += e["description"].get<std::string>() + " ";
        if (e.contains("type") && e["type"].is_string())
            out += "(" + e["type"].get<std::string>() + ") ";
        if (e.contains("module") && e["module"].is_string() &&
            e.contains("module_offset") && e["module_offset"].is_string())
            out += "@ " + e["module"].get<std::string>() + "+" +
                   e["module_offset"].get<std::string>() + " ";
        else if (e.contains("address") && e["address"].is_string())
            out += "@ " + e["address"].get<std::string>() + " ";
        if (e.contains("confidence") && e["confidence"].is_number())
            out += "置信度 " + std::to_string(e["confidence"].get<double>()) + " ";
        if (e.contains("notes") && e["notes"].is_string())
            out += "— " + e["notes"].get<std::string>();
        out += "\n";
        shown++;
    }
    if (shown == 0) out += "（无匹配标注）\n";
    return out;
}

size_t AnnotationStore::count() {
    std::lock_guard<std::mutex> lk(mtx_);
    return data_["annotations"].size();
}

}  // namespace gg
