// warden — permission-gated execution policy check.
//
// One job: decide whether a tool invocation is allowed. Forge asks,
// warden answers. Default-deny for anything not explicitly allowed.
//
// Gate shape (4 checks, structural — not advisory):
//   1. policy     — tool must be on the allow-list
//   2. intent     — caller must state a reason (counterfactual-coherence:
//                   articulation forces the asker to model the request)
//   3. consent    — runtime kill-switch file must not exist
//                   ($XDG_CONFIG_HOME/agent-cpp/locked)
//   4. bounds     — per-tool arg sanity (host/path/size limits)
// First failing check emits exec_deny with numeric code + reason.
//
// Contract:
//   listens for : "exec_request"  (payload = {tool, args, reason?})
//   emits       : "exec_allow"    same payload → to msg.from
//                 "exec_deny"     {tool, code, reason} → to msg.from
//
// Policy today is hard-coded (below). A real deployment will load this
// from $XDG_CONFIG_HOME/agent-cpp/policy.toml.

#include "agents/agent.h"
#include "agents/runtime.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>

namespace rocm_cpp::agents::specialists {

namespace {

std::filesystem::path revoke_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base;
    if (xdg && *xdg) base = xdg;
    else {
        const char* home = std::getenv("HOME");
        base = (home ? std::filesystem::path(home) / ".config"
                     : std::filesystem::path("/tmp"));
    }
    return base / "agent-cpp" / "locked";
}

}  // namespace

class Warden : public Agent {
public:
    Warden() {
        // Allow-list of tools by name. Tools themselves still vet their
        // args — warden only gates which tool is reachable.
        allowed_ = {"echo", "clock", "which"};
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind != "exec_request") return;

        nlohmann::json j;
        std::string tool_name;
        try {
            j = nlohmann::json::parse(msg.payload);
            tool_name = j.value("tool", std::string(""));
        } catch (const std::exception& e) {
            deny_(rt, msg, tool_name, 1, std::string("bad JSON: ") + e.what());
            return;
        }

        // 1. policy
        if (!allowed_.count(tool_name)) {
            deny_(rt, msg, tool_name, 1, "tool not on allow-list: " + tool_name);
            return;
        }
        // 2. intent — caller must articulate *why*
        std::string reason = j.value("reason", std::string(""));
        if (reason.empty()) {
            deny_(rt, msg, tool_name, 2,
                  "missing 'reason' field — state intent before invoking tools");
            return;
        }
        // 3. consent — runtime kill-switch
        std::error_code ec;
        if (std::filesystem::exists(revoke_path(), ec)) {
            deny_(rt, msg, tool_name, 3,
                  "consent revoked (lockfile present at " + revoke_path().string() + ")");
            return;
        }
        // 4. bounds — per-tool arg sanity (stub for v1; extend per tool)
        if (tool_name == "echo") {
            auto args = j.value("args", nlohmann::json::object());
            std::string text = args.value("text", std::string(""));
            if (text.size() > 4096) {
                deny_(rt, msg, tool_name, 4, "echo text exceeds 4096 bytes");
                return;
            }
        }

        // All four checks passed.
        rt.send({.from=name_, .to=msg.from,
                 .kind="exec_allow", .payload=msg.payload});
    }

private:
    std::string                     name_ = "warden";
    std::unordered_set<std::string> allowed_;

    void deny_(Runtime& rt, const Message& src, const std::string& tool,
               int code, const std::string& why) {
        nlohmann::json resp = {{"tool", tool}, {"code", code}, {"reason", why}};
        rt.send({.from=name_, .to=src.from,
                 .kind="exec_deny", .payload=resp.dump()});
    }
};

std::unique_ptr<Agent> make_warden() { return std::make_unique<Warden>(); }

}  // namespace rocm_cpp::agents::specialists
