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

ToolRegistry make_default_registry() {
    using nlohmann::json;
    ToolRegistry r;

    r.register_tool(Tool{
        .name = "scribe_recall",
        .description = "Retrieve the last N messages from the session audit log.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {{"n", {{"type", "integer"}, {"default", 20}}}}},
            {"required", json::array()},
        },
        .target_agent = "scribe",
        .message_kind = "recall",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "librarian_lookup",
        .description = "Look up a document or passage from the local knowledge base.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}}},
                {"top_k", {{"type", "integer"}, {"default", 5}}},
            }},
            {"required", json::array({"query"})},
        },
        .target_agent = "librarian",
        .message_kind = "lookup",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "cartograph_repo_map",
        .description = "Produce a structural map of the current repository.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}, {"default", "."}}}}},
            {"required", json::array()},
        },
        .target_agent = "cartograph",
        .message_kind = "map",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "sentinel_discord_read",
        .description = "Read recent messages from a Discord channel.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"channel_id", {{"type", "string"}}},
                {"limit", {{"type", "integer"}, {"default", 50}}},
            }},
            {"required", json::array({"channel_id"})},
        },
        .target_agent = "sentinel",
        .message_kind = "read_discord",
        .is_write = false,
    });

    // ── GitHub (quartermaster / magistrate / librarian) ────────────────
    // Routed through the existing GitHub specialists in agent-cpp. Write
    // tools (create_issue, comment) gated by CVG on remote calls.

    r.register_tool(Tool{
        .name = "github_search_repo",
        .description = "Search across a repository: code, issues, PRs, or commits.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"repo", {{"type", "string"}, {"description", "owner/repo"}}},
                {"query", {{"type", "string"}}},
                {"kind", {{"type", "string"}, {"enum", {"code", "issues", "prs", "commits"}}, {"default", "code"}}},
                {"limit", {{"type", "integer"}, {"default", 20}}},
            }},
            {"required", json::array({"repo", "query"})},
        },
        .target_agent = "librarian",
        .message_kind = "github_search",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "github_list_prs",
        .description = "List pull requests for a repo, with state and review status.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"repo", {{"type", "string"}}},
                {"state", {{"type", "string"}, {"enum", {"open", "closed", "all"}}, {"default", "open"}}},
            }},
            {"required", json::array({"repo"})},
        },
        .target_agent = "librarian",
        .message_kind = "github_list_prs",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "github_review_pr",
        .description = "Review a pull request — read diff, comments, CI status. Read-only.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"repo", {{"type", "string"}}},
                {"pr_number", {{"type", "integer"}}},
            }},
            {"required", json::array({"repo", "pr_number"})},
        },
        .target_agent = "magistrate",
        .message_kind = "review_pr",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "github_create_issue",
        .description = "Create a new issue on a repo. Write — CVG-gated; deny-by-default for remote callers.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"repo", {{"type", "string"}}},
                {"title", {{"type", "string"}}},
                {"body", {{"type", "string"}}},
                {"labels", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"required", json::array({"repo", "title"})},
        },
        .target_agent = "quartermaster",
        .message_kind = "create_issue",
        .is_write = true,
    });

    r.register_tool(Tool{
        .name = "github_comment",
        .description = "Post a comment on an issue or PR. Write — CVG-gated.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"repo", {{"type", "string"}}},
                {"number", {{"type", "integer"}}},
                {"body", {{"type", "string"}}},
            }},
            {"required", json::array({"repo", "number", "body"})},
        },
        .target_agent = "quartermaster",
        .message_kind = "comment",
        .is_write = true,
    });

    // ── Google (sommelier → external APIs) ────────────────────────────
    // Sommelier is the existing specialist that handles external paid APIs.
    // We route Google Drive / Gmail / Calendar through it so credentials
    // stay in one place and OAuth lifecycle is centralized.

    r.register_tool(Tool{
        .name = "google_drive_search",
        .description = "Search files in Google Drive by name, content, or metadata.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}}},
                {"limit", {{"type", "integer"}, {"default", 20}}},
                {"mime_type", {{"type", "string"}, {"description", "optional filter (e.g. application/pdf)"}}},
            }},
            {"required", json::array({"query"})},
        },
        .target_agent = "sommelier",
        .message_kind = "gdrive_search",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "google_drive_read",
        .description = "Read the text contents of a Google Drive file (Docs, Sheets, text, PDF).",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"file_id", {{"type", "string"}}},
            }},
            {"required", json::array({"file_id"})},
        },
        .target_agent = "sommelier",
        .message_kind = "gdrive_read",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "google_gmail_search",
        .description = "Search Gmail messages with Gmail query syntax (from:, subject:, has:attachment, etc.).",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}}},
                {"limit", {{"type", "integer"}, {"default", 20}}},
            }},
            {"required", json::array({"query"})},
        },
        .target_agent = "sommelier",
        .message_kind = "gmail_search",
        .is_write = false,
    });

    r.register_tool(Tool{
        .name = "google_calendar_upcoming",
        .description = "List upcoming events from Google Calendar within a time window.",
        .input_schema = json{
            {"type", "object"},
            {"properties", {
                {"calendar_id", {{"type", "string"}, {"default", "primary"}}},
                {"hours_ahead", {{"type", "integer"}, {"default", 24}}},
            }},
            {"required", json::array()},
        },
        .target_agent = "sommelier",
        .message_kind = "gcal_upcoming",
        .is_write = false,
    });

    return r;
}

}
