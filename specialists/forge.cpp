// forge — tool dispatch.
//
// One job: translate "tool_call" messages into executed tool output,
// via warden gate. Ships with three small in-process tools so the
// framework is demonstrable without external deps:
//
//   echo   args{text} -> returns text
//   clock  args{}     -> returns ISO-8601 UTC timestamp
//   which  args{name} -> returns first PATH hit, or empty
//
// More tools (shell with warden policy, HTTP GET, file read) land
// behind warden's allow-list. The C API surface here stays tiny;
// tools register themselves by name.
//
// Contract:
//   listens for : "tool_call"    (payload = JSON {id, tool, args})
//                 "exec_allow"   (from warden, after forge asked)
//                 "exec_deny"    (from warden)
//   emits       : "exec_request" (to warden, before running anything)
//                 "tool_result"  (payload = JSON {id, ok, data, error})

#include "agents/agent.h"
#include "agents/runtime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace rocm_cpp::agents::specialists {

using ToolFn = std::function<nlohmann::json(const nlohmann::json&)>;

static nlohmann::json tool_echo(const nlohmann::json& args) {
    return {{"text", args.value("text", std::string(""))}};
}

static nlohmann::json tool_clock(const nlohmann::json&) {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return {{"iso_utc", buf}};
}

static nlohmann::json tool_which(const nlohmann::json& args) {
    const std::string name = args.value("name", std::string(""));
    const char* pathenv = std::getenv("PATH");
    if (name.empty() || !pathenv) return {{"path", ""}};
    std::string p = pathenv;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        std::string dir = p.substr(start, end - start);
        if (!dir.empty()) {
            std::filesystem::path cand = std::filesystem::path(dir) / name;
            std::error_code ec;
            if (std::filesystem::exists(cand, ec))
                return {{"path", cand.string()}};
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {{"path", ""}};
}

class Forge : public Agent {
public:
    Forge() {
        tools_["echo"]  = tool_echo;
        tools_["clock"] = tool_clock;
        tools_["which"] = tool_which;
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind == "tool_call") {
            // Pass through to warden for the allow check; keep caller
            // context so the "exec_allow" reply carries back here.
            Message q;
            q.from = name_;
            q.to   = "warden";
            q.kind = "exec_request";
            q.payload = msg.payload;
            pending_caller_[msg.id] = msg.from;
            pending_body_[msg.id]   = msg.payload;
            rt.send(std::move(q));
            return;
        }
        if (msg.kind == "exec_allow") {
            dispatch_(msg, rt, true);
            return;
        }
        if (msg.kind == "exec_deny") {
            dispatch_(msg, rt, false);
            return;
        }
    }

private:
    std::string name_ = "forge";
    std::unordered_map<std::string, ToolFn>    tools_;
    std::unordered_map<uint64_t, std::string>   pending_caller_;
    std::unordered_map<uint64_t, std::string>   pending_body_;

    // Given a warden reply, find the original caller + tool call body
    // by correlating the incoming message's payload.
    void dispatch_(const Message& msg, Runtime& rt, bool allowed) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (...) { return; }

        std::string tool_name = j.value("tool", std::string(""));
        std::string caller;  // find a pending caller with matching tool
        uint64_t    kid = 0;
        nlohmann::json args = j.value("args", nlohmann::json::object());
        for (auto& [id, body] : pending_body_) {
            try {
                auto jb = nlohmann::json::parse(body);
                if (jb.value("tool", std::string("")) == tool_name) {
                    caller = pending_caller_[id];
                    kid = id;
                    break;
                }
            } catch (...) {}
        }
        if (kid) { pending_caller_.erase(kid); pending_body_.erase(kid); }
        if (caller.empty()) caller = "stdout";

        nlohmann::json out;
        out["tool"] = tool_name;
        if (!allowed) {
            out["ok"]    = false;
            out["error"] = j.value("reason", std::string("denied"));
        } else {
            auto it = tools_.find(tool_name);
            if (it == tools_.end()) {
                out["ok"]    = false;
                out["error"] = "unknown tool: " + tool_name;
            } else {
                try {
                    auto data = it->second(args);
                    out["ok"]   = true;
                    out["data"] = data;
                } catch (const std::exception& e) {
                    out["ok"]    = false;
                    out["error"] = std::string("tool exception: ") + e.what();
                }
            }
        }

        Message r;
        r.from = name_;
        r.to   = caller;
        r.kind = "tool_result";
        r.payload = out.dump();
        rt.send(std::move(r));
    }
};

std::unique_ptr<Agent> make_forge() { return std::make_unique<Forge>(); }

}  // namespace rocm_cpp::agents::specialists
