// halo-mcp — Phase 0 scaffold.
//
// Today: stdio JSON-RPC 2.0, tools/list from the registry, tools/call
// routed through a stub handler that returns a canned response.
//
// Next: wire the handler to rocm_cpp::agents::Runtime — send a Message
// to tool.target_agent with kind=tool.message_kind, await a reply via a
// request-correlation inbox. See docs/mcp-nexus-design.md for the full
// plan, including nexus enrollment in later phases.

#include "halo_mcp/stdio_server.hpp"
#include "halo_mcp/tool_registry.hpp"

#include <nlohmann/json.hpp>

#include <iostream>

using nlohmann::json;

int main() {
    halo_mcp::ToolRegistry registry = halo_mcp::make_default_registry();

    // TODO(phase-1): replace this stub with a real bus bridge.
    //
    // Phase 1 shape:
    //   Runtime rt;
    //   rt.register_agent(std::make_unique<scribe::Scribe>(...));
    //   ... (register the subset of specialists we expose as MCP tools)
    //   rt.set_audit("scribe");
    //   std::thread runtime_thread([&]{ rt.run(); });
    //
    //   handler = [&](const Tool& t, const json& args) {
    //       const auto request_id = next_id();
    //       setup_reply_inbox(request_id);
    //       rt.send({.from = "halo-mcp",
    //                .to   = t.target_agent,
    //                .kind = t.message_kind,
    //                .payload = args.dump(),
    //                ...});
    //       return await_reply(request_id);  // blocks w/ timeout
    //   };
    //
    // Phase 2 adds CVG by sending through warden first:
    //   rt.send({.to = "warden", .kind = "gate_request",
    //            .payload = encode(target=t.target_agent, args=args, tool=t.name)});
    //   auto decision = await_reply(request_id);
    //   if (!decision.allowed) return json{{"error", decision.reason}};
    //   ... then dispatch to t.target_agent as above.
    auto stub_handler = [](const halo_mcp::Tool& t, const json& args) -> json {
        return json{
            {"stub", true},
            {"tool", t.name},
            {"target_agent", t.target_agent},
            {"message_kind", t.message_kind},
            {"args", args},
            {"note", "phase-0 scaffold; bus bridge arrives in phase 1"},
        };
    };

    halo_mcp::StdioServer server(registry, stub_handler);
    server.run();
    return 0;
}
