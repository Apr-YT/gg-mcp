// annotations.cpp — 标注库实现（JSON 文件持久化）
#include "annotations.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>

namespace gg {

void AnnotationStore::load() {
    std::ifstream f(path_);
    if (!f.is_open()) {
        data_ = json::object();
        return;
    }
    bool bad = false;
    try {
        f >> data_;
    } catch (...) {
        bad = true;
    }
    if (!bad && (!data_.is_object() || !data_.contains("annotations") ||
                 !data_["annotations"].is_object())) {
        bad = true;
    }
    if (bad) {
        /* 文件坏了：先备份再清空。
         * 这是跨会话记忆，直接丢等于让用户积攒的标注归零 —— 而更难察觉的是
         * 下一次 save() 会用空库覆盖损坏文件，原内容再也回不来。 */
        f.close();
        const std::string bak = path_ + ".corrupt";
        std::remove(bak.c_str());          // Windows 的 rename 不覆盖已存在文件
        if (std::rename(path_.c_str(), bak.c_str()) == 0) {
            std::cerr << "[annotations] " << path_ << " 已损坏，原文件备份为 "
                      << bak << "，本次以空库启动\n";
        } else {
            std::cerr << "[annotations] " << path_ << " 已损坏且备份失败，"
                      << "继续写入会覆盖原内容\n";
        }
        data_ = json::object();
    }
    if (!data_.contains("annotations")) data_["annotations"] = json::object();
}

void AnnotationStore::save() {
    /* 原子写：先写 .tmp 再 rename。
     * 直接 trunc 原文件的话，写一半崩溃/断电就留下截断的 JSON，
     * 下次 load 解析失败 → 清空 → 又覆盖一遍，数据全丢。 */
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open()) {
            std::cerr << "[annotations] 无法写入 " << tmp
                      << "（磁盘满或权限不足），本次标注未保存\n";
            return;
        }
        f << data_.dump(2);
        f.flush();
        if (!f.good()) {
            std::cerr << "[annotations] 写入 " << tmp << " 失败，本次标注未保存\n";
            return;
        }
    }
#ifdef _WIN32
    std::remove(path_.c_str());            // Windows 的 rename 不覆盖已存在文件
#endif
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        std::cerr << "[annotations] " << tmp << " -> " << path_
                  << " 重命名失败，本次标注未保存\n";
        std::remove(tmp.c_str());
    }
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
