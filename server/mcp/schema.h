// schema.h — MCP 工具注册表
#pragma once

#include <string>
#include <vector>

#include "../../third_party/json.hpp"

namespace gg {

using json = nlohmann::json;

struct ToolDef {
    std::string name;
    std::string description;
    json input_schema;
};

// 返回全部工具定义（MCP tools/list）
const std::vector<ToolDef>& tool_defs();

}  // namespace gg
