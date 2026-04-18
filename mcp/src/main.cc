// halo-mcp — Phase 1.5. Bus bridge + read-only specialists.
//
// Lifecycle:
//   1. Build a Runtime with:
//        - BusBridge registered as "halo-mcp-bridge"
//        - scribe, librarian, cartograph, sentinel (read-only subset)
//      Write specialists (quartermaster, magistrate, herald, anvil) are
//      NOT registered here — they'll land in Phase 2 behind CVG gating.
//   2. Start the Runtime on a background thread.
//   3. Run the stdio JSON-RPC 2.0 loop on the main thread. tools/call
//      flows through bridge.send_request(target, kind, args_json).
//   4. On EOF or signal: runtime.shutdown(), join, exit.
//
// Disable specific specialists via HALO_MCP_DISABLE=comma,separated,names.

#include "halo_mcp/bus_bridge.hpp"
#include "halo_mcp/stdio_server.hpp"
#include "halo_mcp/tool_registry.hpp"

#include "agents/agent.h"
#include "agents/runtime.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

using nlohmann::json;
using rocm_cpp::agents::Agent;
using rocm_cpp::agents::Runtime;

// Forward-declare the specialist factories we want. Each lives in a .cpp
// under agent-cpp/specialists/ and is linked via agent_cpp_specialists.
namespace rocm_cpp::agents::specialists {
std::unique_ptr<Agent> make_scribe();
std::unique_ptr<Agent> make_librarian();
std::unique_ptr<Agent> make_cartograph();
std::unique_ptr<Agent> make_sentinel();
std::unique_ptr<Agent> make_forge();
std::unique_ptr<Agent> make_warden();
std::unique_ptr<Agent> make_muse();
std::unique_ptr<Agent> make_sommelier();
std::unique_ptr<Agent> make_echo_mouth();
std::unique_ptr<Agent> make_quartermaster();
}  // namespace rocm_cpp::agents::specialists

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

    // Read-only specialist subset — safe to host inside halo-mcp directly.
    // Write-side specialists (quartermaster, magistrate, herald, anvil) are
    // deliberately NOT registered here; they arrive in Phase 2 behind CVG.
    std::unordered_set<std::string> disabled;
    if (const char* v = std::getenv("HALO_MCP_DISABLE")) {
        std::string s = v;
        size_t start = 0;
        while (start < s.size()) {
            size_t end = s.find(',', start);
            std::string tok = s.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
            if (!tok.empty()) disabled.insert(tok);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    auto maybe_register = [&](const char* name, std::unique_ptr<Agent> a) {
        if (disabled.count(name)) {
            std::fprintf(stderr, "[halo-mcp] specialist %s disabled via HALO_MCP_DISABLE\n", name);
            return;
        }
        runtime.register_agent(std::move(a));
        std::fprintf(stderr, "[halo-mcp] registered %s\n", name);
    };
    maybe_register("scribe",      rocm_cpp::agents::specialists::make_scribe());
    maybe_register("librarian",   rocm_cpp::agents::specialists::make_librarian());
    maybe_register("cartograph",  rocm_cpp::agents::specialists::make_cartograph());
    maybe_register("sentinel",    rocm_cpp::agents::specialists::make_sentinel());
    maybe_register("warden",        rocm_cpp::agents::specialists::make_warden());
    maybe_register("forge",         rocm_cpp::agents::specialists::make_forge());
    maybe_register("muse",          rocm_cpp::agents::specialists::make_muse());
    maybe_register("sommelier",     rocm_cpp::agents::specialists::make_sommelier());
    maybe_register("echo_mouth",    rocm_cpp::agents::specialists::make_echo_mouth());
    maybe_register("quartermaster", rocm_cpp::agents::specialists::make_quartermaster());

    runtime.set_audit("scribe");   // journal every routed message through scribe

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
