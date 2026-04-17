// carpenter — install-support helper.
//
// One job: help beta testers get install.sh working on their box. Reads
// the user's error output (shared as a paste or attached log), diagnoses
// common failures (missing ROCm, wrong clang path, LFS not installed,
// wrong gfx arch), walks them through the fix over Discord.
//
// Contract:
//   listens for : "discord_message" (filtered to install-help channel)
//                 "install_log"     (payload = raw install.sh output)
//   emits       : "discord_reply"   (step-by-step fix)
//                 "github_issue_comment"  (if they filed one)
// Auth         : DISCORD_TOKEN + GH_TOKEN.

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Carpenter : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: regex-match known failures, delegate novel cases to planner.
    }
private:
    std::string name_ = "carpenter";
};
std::unique_ptr<Agent> make_carpenter() { return std::make_unique<Carpenter>(); }
}  // namespace
