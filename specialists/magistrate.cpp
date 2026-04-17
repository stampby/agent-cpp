// magistrate — GitHub PR review (read + write).
//
// One job: review opened PRs. Fetch diff, run checks per the repo's
// contribution rules (no MLX, BitNet-only, MIT-compatible licenses,
// install.sh still works), post inline comments, approve if clean,
// request changes if not. Never merges without human green light.
//
// Contract:
//   listens for : "github_pr_opened"
//                 "github_pr_synchronized"
//                 "github_review_requested"
//   emits       : "github_review_comment"  (inline)
//                 "github_review_submit"   (approve / request_changes / comment)
//                 "discord_post"           (ping architect on gray zones)
// Auth         : GH_TOKEN env, scope: repo (PR read + review write)

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Magistrate : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: gh pr diff → policy scan → planner verdict → post review.
    }
private:
    std::string name_ = "magistrate";
};
std::unique_ptr<Agent> make_magistrate() { return std::make_unique<Magistrate>(); }
}  // namespace
