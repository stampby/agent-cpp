// librarian — docs + changelog maintainer (read + write).
//
// One job: keep docs honest. When a PR lands that changes public API,
// update the README/docs table; when a release tag lands, append to
// CHANGELOG.md with the architect's commit-message style (why, not what).
//
// Contract:
//   listens for : "github_push_main"
//                 "github_release"
//                 "docs_refresh_request"
//   emits       : "github_pr_open"   (docs-only PR with the diff)
//                 "discord_post"     (notice to #announcements for releases)
// Auth         : GH_TOKEN env.

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Librarian : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: diff public API, patch docs tables, open PR via gh api.
    }
private:
    std::string name_ = "librarian";
};
std::unique_ptr<Agent> make_librarian() { return std::make_unique<Librarian>(); }
}  // namespace
