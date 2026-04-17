// sommelier — model routing.
//
// One job: pick the right model for the request and route the decode
// call through it. Today there's one model (BitNet-2B-4T). Tomorrow:
// BitNet-8B for hard asks, BitNet-2B-fast for chat, domain-specific
// fine-tunes for code / voice. The planner doesn't care which — it
// says "decode this" and sommelier chooses.
//
// Contract:
//   listens for : "decode_request" (payload = JSON {prompt, max_new, hint})
//   emits       : "decode_result"  (payload = JSON {text, tok_s, model_used})

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Sommelier : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: model registry, route based on hint + load, call librocm_cpp.
    }
private:
    std::string name_ = "sommelier";
};
std::unique_ptr<Agent> make_sommelier() { return std::make_unique<Sommelier>(); }
}  // namespace
