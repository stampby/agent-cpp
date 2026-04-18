#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace halo_mcp {

struct Tool {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    std::string target_agent;
    std::string message_kind;
    bool is_write;
};

class ToolRegistry {
public:
    void register_tool(Tool t);
    [[nodiscard]] const Tool* find(const std::string& name) const;
    [[nodiscard]] std::vector<Tool> all() const;

private:
    std::unordered_map<std::string, Tool> tools_;
};

ToolRegistry make_default_registry();

}
