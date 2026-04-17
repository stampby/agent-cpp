// scribe — context compaction + session persistence.
//
// One job: keep the conversation honest. Append every inbound user/muse
// exchange to a JSONL session log; on demand, compact old turns into a
// summary so the KV window stays usable.
//
// Contract:
//   listens for : "user_said", "muse_reply", "request_compact"
//   emits       : "session_compacted", "session_loaded"
// Storage      : $XDG_STATE_HOME/agent-cpp/sessions/<id>.jsonl

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Scribe : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: JSONL append, compaction via librocm_cpp summarizer.
    }
private:
    std::string name_ = "scribe";
};
std::unique_ptr<Agent> make_scribe() { return std::make_unique<Scribe>(); }
}  // namespace
