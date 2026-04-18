#pragma once

#include "halo_mcp/tool_registry.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace halo_mcp {

using CallHandler = std::function<nlohmann::json(const Tool&, const nlohmann::json& args)>;

class StdioServer {
public:
    StdioServer(const ToolRegistry& registry, CallHandler handler);

    void run();

private:
    nlohmann::json handle_request(const nlohmann::json& req);

    const ToolRegistry& registry_;
    CallHandler handler_;
};

}
