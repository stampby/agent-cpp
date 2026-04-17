// warden — permission-gated execution.
//
// One job: decide whether forge is allowed to execute a given tool and
// with what scope. Reads a policy file at startup; every tool call is
// checked. Dangerous calls (rm -rf, outbound network to unknown hosts,
// writes outside the repo) are blocked or prompted.
//
// Contract:
//   listens for : "exec_request" (from forge, payload = JSON {tool, args})
//   emits       : "exec_allow"   (forge executes)
//                 "exec_deny"    (forge returns error to planner)
// Config       : $XDG_CONFIG_HOME/agent-cpp/policy.toml

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Warden : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: policy parse, pattern match, deny-by-default for unknowns.
    }
private:
    std::string name_ = "warden";
};
std::unique_ptr<Agent> make_warden() { return std::make_unique<Warden>(); }
}  // namespace
