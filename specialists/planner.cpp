// planner — ReAct / tree-of-thought over librocm_cpp.
//
// One job: given a user request, produce a sequence of thoughts +
// tool-calls + reflections until the goal is met or budget exhausts.
// Uses the decode hot path in rocm-cpp with grammar-constrained JSON
// so every tool-call emit is parseable without regex rescue.
//
// Contract:
//   listens for : "user_goal"    (payload = text)
//                 "tool_result"  (payload = JSON)
//   emits       : "tool_call"    (to forge, grammar-checked)
//                 "final_answer" (to stdout / tui)

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Planner : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: wrap librocm_cpp decode with GBNF → JSON tool-call grammar.
    }
private:
    std::string name_ = "planner";
};
std::unique_ptr<Agent> make_planner() { return std::make_unique<Planner>(); }
}  // namespace
