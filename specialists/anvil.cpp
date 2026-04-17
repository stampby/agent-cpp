// anvil — build / benchmark runner (read + write on CI).
//
// One job: on every push to main (across our repos), rebuild, run the
// test suite, run the bench suite, post the numbers to Discord
// #benchmarks and to the PR thread if one exists. Regression > 2% vs
// previous main pages the architect via herald.
//
// Contract:
//   listens for : "github_push_main"   (webhook receiver)
//                 "bench_run_request"  (on-demand from anywhere)
//   emits       : "bench_result"   (payload = JSON {repo, sha, numbers})
//                 "discord_post"   (to #benchmarks)
//                 "github_comment" (if triggered from a PR)
// Auth         : GH_TOKEN + DISCORD_TOKEN. Runs builds in a throwaway
//                dir under $XDG_CACHE_HOME/agent-cpp/bench-<sha>.

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Anvil : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: git fetch + cmake + ctest + bitnet_decode timing sweep → JSON.
    }
private:
    std::string name_ = "anvil";
};
std::unique_ptr<Agent> make_anvil() { return std::make_unique<Anvil>(); }
}  // namespace
