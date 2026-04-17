// warden — permission-gated execution policy check.
//
// One job: decide whether a tool invocation is allowed. Forge asks,
// warden answers. Default-deny for anything not explicitly allowed.
//
// Contract:
//   listens for : "exec_request"  (payload = JSON {tool, args, from})
//   emits       : "exec_allow"    (payload = same JSON) -> to msg.from
//                 "exec_deny"     (payload = JSON {tool, args, reason})
//
// Policy today is hard-coded (below). A real deployment will load this
// from $XDG_CONFIG_HOME/agent-cpp/policy.toml.

#include "agents/agent.h"
#include "agents/runtime.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>

namespace rocm_cpp::agents::specialists {

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

        std::string reason;
        bool ok = false;
        std::string tool_name;

        try {
            auto j = nlohmann::json::parse(msg.payload);
            tool_name = j.value("tool", std::string(""));
            if (allowed_.count(tool_name)) ok = true;
            else reason = "tool not on allow-list: " + tool_name;
        } catch (const std::exception& e) {
            reason = std::string("bad JSON: ") + e.what();
        }

        nlohmann::json resp;
        resp["tool"]   = tool_name;
        resp["reason"] = reason;
        Message out{
            .from    = name_,
            .to      = msg.from,  // back to the asker (normally "forge")
            .kind    = ok ? "exec_allow" : "exec_deny",
            .payload = ok ? msg.payload : resp.dump(),
        };
        rt.send(std::move(out));
    }

private:
    std::string                 name_ = "warden";
    std::unordered_set<std::string> allowed_;
};

std::unique_ptr<Agent> make_warden() { return std::make_unique<Warden>(); }

}  // namespace rocm_cpp::agents::specialists
