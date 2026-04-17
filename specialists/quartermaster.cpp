// quartermaster — GitHub issue triage (read + write).
//
// One job: watch new issues across the repos we own (rocm-cpp,
// halo-1bit, agent-cpp), classify, label, answer FAQ-class questions,
// escalate the novel ones to the architect via herald -> #chat.
//
// Contract:
//   listens for : "github_issue_opened"  (from anvil's webhook receiver)
//                 "github_issue_comment"
//   emits       : "github_label"     (payload = JSON {repo, num, labels})
//                 "github_comment"   (payload = JSON {repo, num, body})
//                 "github_assign"
//                 "discord_post"     (escalation to #chat)
// Auth         : GH_TOKEN env, scope: repo (read/write on owned repos)

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Quartermaster : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: gh api calls, classification via planner (call into librocm_cpp).
    }
private:
    std::string name_ = "quartermaster";
};
std::unique_ptr<Agent> make_quartermaster() { return std::make_unique<Quartermaster>(); }
}  // namespace
