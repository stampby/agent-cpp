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

    // ── sommelier (external-API backend selector) ───────────────────
    r.register_tool(Tool{
        .name = "sommelier_list_backends",
        .description = "List the external LLM / API backends sommelier knows about and their status.",
        .input_schema = json{
            {"type", "object"},
            {"properties", json::object()},
            {"required", json::array()},
        },
        .target_agent = "sommelier",
        .message_kind = "list_backends",
        .is_write = false,
    });

    // ── muse (chat specialist) ──────────────────────────────────────
    r.register_tool(Tool{
        .name = "muse_reset",
        .description = "Clear muse's running conversation state and start a fresh chat thread.",
        .input_schema = json{
            {"type", "object"},
            {"properties", json::object()},
            {"required", json::array()},
        },
        .target_agent = "muse",
        .message_kind = "reset",
        .is_write = true,
    });

    // ── sentinel (Discord watcher) ──────────────────────────────────
    r.register_tool(Tool{
        .name = "sentinel_watch_channel",
        .description = "Add a Discord channel to sentinel's active watch list. Requires DISCORD_TOKEN.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {{"channel_id", {{"type", "string"}}}}},
            {"required", json::array({"channel_id"})},
        },
        .target_agent = "sentinel",
        .message_kind = "sentinel_watch",
        .is_write = true,
    });

    r.register_tool(Tool{
        .name = "sentinel_unwatch_channel",
        .description = "Remove a Discord channel from sentinel's active watch list.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {{"channel_id", {{"type", "string"}}}}},
            {"required", json::array({"channel_id"})},
        },
        .target_agent = "sentinel",
        .message_kind = "sentinel_unwatch",
        .is_write = true,
    });

    r.register_tool(Tool{
        .name = "sentinel_reconnect",
        .description = "Force sentinel's Discord WebSocket to reconnect. Useful after credential rotation.",
        .input_schema = json{
            {"type", "object"},
            {"properties", json::object()},
            {"required", json::array()},
        },
        .target_agent = "sentinel",
        .message_kind = "reconnect",
        .is_write = true,
    });

    // ── echo_mouth (TTS output) ─────────────────────────────────────
    r.register_tool(Tool{
        .name = "echo_mouth_say",
        .description = "Speak text via the TTS backend (kokoro by default). Requires the TTS service to be running.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"text", {{"type", "string"}}},
                {"voice", {{"type", "string"}, {"description", "optional voice name"}}},
            }},
            {"required", json::array({"text"})},
        },
        .target_agent = "echo_mouth",
        .message_kind = "tts_say",
        .is_write = true,
    });

    // ── quartermaster (GitHub triage) ───────────────────────────────
    r.register_tool(Tool{
        .name = "quartermaster_triage_issue",
        .description = "Run LLM-backed triage on a GitHub issue — labels, severity, owner suggestion.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"repo", {{"type", "string"}, {"description", "owner/repo"}}},
                {"number", {{"type", "integer"}}},
            }},
            {"required", json::array({"repo", "number"})},
        },
        .target_agent = "quartermaster",
        .message_kind = "github_issue_triage",
        .is_write = false,
    });

    // ── quartermaster GitHub CRUD ───────────────────────────────────
    r.register_tool(Tool{
        .name = "github_create_issue",
        .description = "Create a new issue on a repo. Write — requires GH_TOKEN. CVG-gated in Phase 2.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"repo",   {{"type", "string"}, {"description", "owner/repo"}}},
                {"title",  {{"type", "string"}}},
                {"body",   {{"type", "string"}}},
                {"labels", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"required", json::array({"repo", "title"})},
        },
        .target_agent = "quartermaster",
        .message_kind = "github_create_issue",
        .is_write = true,
    });

    r.register_tool(Tool{
        .name = "github_comment",
        .description = "Post a comment on an issue or PR. Write — requires GH_TOKEN.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"repo",   {{"type", "string"}}},
                {"number", {{"type", "integer"}}},
                {"body",   {{"type", "string"}}},
            }},
            {"required", json::array({"repo", "number", "body"})},
        },
        .target_agent = "quartermaster",
        .message_kind = "github_comment",
        .is_write = true,
    });

    r.register_tool(Tool{
        .name = "github_review_pr",
        .description = "Fetch PR metadata, diff file list, and comments in one call. Read-only.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"repo",      {{"type", "string"}}},
                {"pr_number", {{"type", "integer"}}},
            }},
            {"required", json::array({"repo", "pr_number"})},
        },
        .target_agent = "quartermaster",
        .message_kind = "github_review_pr",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "github_list_prs",
        .description = "List pull requests for a repo.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"repo",  {{"type", "string"}}},
                {"state", {{"type", "string"}, {"enum", {"open", "closed", "all"}}, {"default", "open"}}},
            }},
            {"required", json::array({"repo"})},
        },
        .target_agent = "quartermaster",
        .message_kind = "github_list_prs",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "librarian_search_docs",
        .description = "Search a local docs directory for a query. Walks *.md/*.mdx/*.txt. DOCS_ROOT env var or 'root' arg overrides the default.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}}},
                {"limit", {{"type", "integer"}, {"default", 10}, {"maximum", 100}}},
                {"root",  {{"type", "string"}, {"description", "override DOCS_ROOT"}}},
            }},
            {"required", json::array({"query"})},
        },
        .target_agent = "librarian",
        .message_kind = "search_docs",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "sentinel_fetch_recent",
        .description = "Fetch the last N messages from a Discord channel via REST. Requires DISCORD_TOKEN.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"channel_id", {{"type", "string"}}},
                {"limit", {{"type", "integer"}, {"default", 25}, {"maximum", 100}}},
            }},
            {"required", json::array({"channel_id"})},
        },
        .target_agent = "sentinel",
        .message_kind = "fetch_recent",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "github_search_repo",
        .description = "Search within a repository. kind ∈ {code, issues, commits}.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"repo",  {{"type", "string"}}},
                {"query", {{"type", "string"}}},
                {"kind",  {{"type", "string"}, {"enum", {"code", "issues", "commits"}}, {"default", "code"}}},
            }},
            {"required", json::array({"repo", "query"})},
        },
        .target_agent = "quartermaster",
        .message_kind = "github_search_repo",
        .is_write = false,
    });

    // ── google_sommelier (Drive / Gmail / Calendar read-focused) ────
    // Bearer token: GOOGLE_ACCESS_TOKEN env var, or falls back to shelling
    // out to `gcloud auth application-default print-access-token`. User
    // runs `gcloud auth application-default login` once to set up.
    r.register_tool(Tool{
        .name = "google_drive_search",
        .description = "Search Google Drive by name + full text. Optional mime_type filter (e.g. application/pdf).",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"query",     {{"type", "string"}}},
                {"limit",     {{"type", "integer"}, {"default", 20}, {"maximum", 100}}},
                {"mime_type", {{"type", "string"}}},
            }},
            {"required", json::array({"query"})},
        },
        .target_agent = "google_sommelier",
        .message_kind = "gdrive_search",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "google_drive_read",
        .description = "Read the contents of a Drive file. Google Docs export to Markdown, Sheets to CSV, Slides to text; others raw. Cap 500 KB.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {{"file_id", {{"type", "string"}}}}},
            {"required", json::array({"file_id"})},
        },
        .target_agent = "google_sommelier",
        .message_kind = "gdrive_read",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "google_gmail_search",
        .description = "Search Gmail with standard Gmail query syntax (from:, subject:, has:attachment, label:, after:, etc.).",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}}},
                {"limit", {{"type", "integer"}, {"default", 20}, {"maximum", 100}}},
            }},
            {"required", json::array({"query"})},
        },
        .target_agent = "google_sommelier",
        .message_kind = "gmail_search",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "google_calendar_upcoming",
        .description = "List upcoming Google Calendar events from now to now+hours_ahead (default 24). calendar_id defaults to 'primary'.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"calendar_id", {{"type", "string"}, {"default", "primary"}}},
                {"hours_ahead", {{"type", "integer"}, {"default", 24}, {"maximum", 720}}},
            }},
            {"required", json::array()},
        },
        .target_agent = "google_sommelier",
        .message_kind = "gcal_upcoming",
        .is_write = false,
    });

    return r;
}

}
