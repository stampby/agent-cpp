#include "halo_mcp/stdio_server.hpp"

#include <iostream>
#include <string>

namespace halo_mcp {

using nlohmann::json;

static constexpr const char* kProtocolVersion = "2024-11-05";
static constexpr int kErrInvalidRequest = -32600;
static constexpr int kErrMethodNotFound = -32601;
static constexpr int kErrInternal = -32603;
static constexpr int kErrServer = -32000;

StdioServer::StdioServer(const ToolRegistry& registry, CallHandler handler)
    : registry_(registry), handler_(std::move(handler)) {}

static json ok(const json& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

static json err(const json& id, int code, std::string_view msg) {
    return json{{"jsonrpc", "2.0"}, {"id", id},
                {"error", json{{"code", code}, {"message", std::string(msg)}}}};
}

json StdioServer::handle_request(const json& req) {
    const json id = req.value("id", json());
    const std::string method = req.value("method", "");

    if (method == "initialize") {
        return ok(id, json{
            {"protocolVersion", kProtocolVersion},
            {"capabilities", json{{"tools", json::object()}}},
            {"serverInfo", json{{"name", "halo-mcp"}, {"version", "0.1.0"}}},
        });
    }

    if (method == "tools/list") {
        json tools = json::array();
        for (const auto& t : registry_.all()) {
            tools.push_back(json{
                {"name", t.name},
                {"description", t.description},
                {"inputSchema", t.input_schema},
            });
        }
        return ok(id, json{{"tools", std::move(tools)}});
    }

    if (method == "tools/call") {
        const auto& params = req.value("params", json::object());
        const std::string name = params.value("name", "");
        const json args = params.value("arguments", json::object());
        const Tool* tool = registry_.find(name);
        if (!tool) return err(id, kErrServer, "unknown tool: " + name);
        try {
            json result = handler_(*tool, args);
            return ok(id, json{{"content", json::array({
                json{{"type", "text"}, {"text", result.dump()}},
            })}});
        } catch (const std::exception& e) {
            return err(id, kErrInternal, e.what());
        }
    }

    return err(id, kErrMethodNotFound, "method not found: " + method);
}

void StdioServer::run() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json req;
        try {
            req = json::parse(line);
        } catch (const std::exception& e) {
            std::cout << err(json(), kErrInvalidRequest, e.what()).dump() << '\n' << std::flush;
            continue;
        }
        const json resp = handle_request(req);
        std::cout << resp.dump() << '\n' << std::flush;
    }
}

}
