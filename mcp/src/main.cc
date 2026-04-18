// halo-mcp — Phase 1. Real bus bridge.
//
// Lifecycle:
//   1. Build a Runtime with a BusBridge registered as "halo-mcp-bridge".
//      Phase 1 doesn't register the specialists themselves — that happens
//      when halo-mcp is linked into the same process as agent_cpp, OR we
//      add an IPC hop to a running halo-agent.service (next iteration).
//   2. Start the Runtime on a background thread.
//   3. Run the stdio JSON-RPC 2.0 loop on the main thread. tools/call
//      flows through bridge.send_request(target, kind, args_json).
//   4. On EOF or signal: runtime.shutdown(), join, exit.

#include "halo_mcp/bus_bridge.hpp"
#include "halo_mcp/stdio_server.hpp"
#include "halo_mcp/tool_registry.hpp"

#include "agents/runtime.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <thread>

using nlohmann::json;
using rocm_cpp::agents::Runtime;

namespace {

std::chrono::milliseconds load_timeout() {
    if (const char* v = std::getenv("HALO_MCP_TIMEOUT_MS")) {
        long ms = std::atol(v);
        if (ms > 0) return std::chrono::milliseconds(ms);
    }
    return std::chrono::milliseconds(30'000);
}

Runtime* g_runtime = nullptr;
void on_signal(int) { if (g_runtime) g_runtime->shutdown(); }

}  // namespace

int main() {
    halo_mcp::ToolRegistry registry = halo_mcp::make_default_registry();

    auto bridge_owned = std::make_unique<halo_mcp::BusBridge>(load_timeout());
    halo_mcp::BusBridge* bridge = bridge_owned.get();

    Runtime runtime;
    runtime.register_agent(std::move(bridge_owned));
    runtime.set_audit("scribe");   // no-op if scribe isn't registered in this process

    g_runtime = &runtime;
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT,  on_signal);

    std::thread runtime_thread([&]{ runtime.run(); });

    auto bus_handler = [&](const halo_mcp::Tool& t, const json& args) -> json {
        const std::string payload = args.dump();
        auto reply = bridge->send_request(runtime, t.target_agent, t.message_kind, payload);
        if (!reply.ok) {
            return json{{"error", reply.error},
                        {"target_agent", t.target_agent},
                        {"tool", t.name}};
        }
        // Try parsing the reply as JSON; if it isn't, return as a string.
        try {
            return json::parse(reply.payload);
        } catch (const json::parse_error&) {
            return json{{"text", reply.payload}};
        }
    };

    halo_mcp::StdioServer server(registry, bus_handler);
    server.run();

    runtime.shutdown();
    if (runtime_thread.joinable()) runtime_thread.join();
    return 0;
}
