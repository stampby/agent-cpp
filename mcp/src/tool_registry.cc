#include "halo_mcp/tool_registry.hpp"

namespace halo_mcp {

void ToolRegistry::register_tool(Tool t) {
    tools_[t.name] = std::move(t);
}

const Tool* ToolRegistry::find(const std::string& name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second;
}

std::vector<Tool> ToolRegistry::all() const {
    std::vector<Tool> out;
    out.reserve(tools_.size());
    for (const auto& [_, t] : tools_) out.push_back(t);
    return out;
}

// Phase 1.5 registry — only tools that map to a real specialist handler.
//
// Read-only surface across scribe + cartograph. Librarian and sentinel are
// event-driven (react to webhooks/Discord events) not query-driven, so
// they don't get MCP tools here; they'll expose tools in Phase 2 once we
// either add query-style handlers or wrap them with a reflective adapter.
// Write specialists (herald/quartermaster/magistrate/anvil) arrive in
// Phase 2 behind the CVG gate.
ToolRegistry make_default_registry() {
    using nlohmann::json;
    ToolRegistry r;

    // ── cartograph (semantic memory) ─────────────────────────────────
    r.register_tool(Tool{
        .name = "cartograph_remember",
        .description = "Store a piece of text in cartograph's semantic memory with optional tags.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"text", {{"type", "string"}}},
                {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"required", json::array({"text"})},
        },
        .target_agent = "cartograph",
        .message_kind = "remember",
        .is_write = true,
    });

    r.register_tool(Tool{
        .name = "cartograph_recall",
        .description = "Retrieve k memories from cartograph ranked by similarity to the query.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}}},
                {"k", {{"type", "integer"}, {"default", 3}, {"minimum", 1}, {"maximum", 32}}},
            }},
            {"required", json::array({"query"})},
        },
        .target_agent = "cartograph",
        .message_kind = "recall",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "cartograph_forget_all",
        .description = "WIPE all cartograph memory. Destructive. CVG-gated in Phase 2; currently unrestricted.",
        .input_schema = json{
            {"type", "object"},
            {"properties", json::object()},
            {"required", json::array()},
        },
        .target_agent = "cartograph",
        .message_kind = "forget_all",
        .is_write = true,
    });

    // ── forge + warden (CVG-gated tool dispatch) ─────────────────────
    // Single entry point that covers every tool forge knows about.
    // Currently: echo, clock, which. Each call passes through warden's
    // exec_request / exec_allow policy before forge dispatches.
    r.register_tool(Tool{
        .name = "forge_exec",
        .description = "Invoke a forge-registered tool through the warden CVG gate. "
                       "Current tools: echo, clock, which. Use args to pass parameters. "
                       "Set reason to a short human-readable string — warden logs and gates on it.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"tool", {{"type", "string"}, {"description", "one of: echo, clock, which"}}},
                {"args", {{"type", "object"}, {"description", "per-tool arguments (echo: {text}, clock: {}, which: {bin})"}}},
                {"reason", {{"type", "string"}, {"description", "why this call is being made — logged by warden"}}},
            }},
            {"required", json::array({"tool", "reason"})},
        },
        .target_agent = "forge",
        .message_kind = "tool_call",
        .is_write = true,   // forge can run side-effectful tools; warden gates
    });

    // ── scribe (audit journal) ───────────────────────────────────────
    r.register_tool(Tool{
        .name = "scribe_new_session",
        .description = "Rotate the audit log — close the current session, open a new one.",
        .input_schema = json{
            {"type", "object"},
            {"properties", json::object()},
            {"required", json::array()},
        },
        .target_agent = "scribe",
        .message_kind = "session_new",
        .is_write = true,
    });

    r.register_tool(Tool{
        .name = "scribe_where",
        .description = "Report the filesystem path of the current session's audit log.",
        .input_schema = json{
            {"type", "object"},
            {"properties", json::object()},
            {"required", json::array()},
        },
        .target_agent = "scribe",
        .message_kind = "session_where",
        .is_write = false,
    });

    return r;
}

}
