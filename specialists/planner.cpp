// planner — ReAct-style thinker over librocm_cpp.
//
// One job: take a user goal, produce a reasoned final answer. In v1
// there are no tool calls wired — planner emits a single-shot chain-
// of-thought pass. When forge is fleshed out the loop here upgrades
// to "thought → tool_call → tool_result → thought → final_answer".
//
// Contract:
//   listens for : "user_goal"     (payload = text)
//                 "tool_result"   (payload = JSON {id, ok, data})
//   emits       : "final_answer"  (payload = text)  -> to msg.from
//                 "tool_call"     (payload = JSON) -> to forge  (when wired)
//   env         : AGENT_CPP_LLM_URL (default http://127.0.0.1:8080)

#include "agents/agent.h"
#include "agents/runtime.h"
#include "agents/llm_client.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace rocm_cpp::agents::specialists {

class Planner : public Agent {
public:
    Planner() {
        const char* url = std::getenv("AGENT_CPP_LLM_URL");
        llm_ = std::make_unique<LLMClient>(url ? url : "http://127.0.0.1:8080");
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind != "user_goal") return;

        std::vector<ChatMessage> prompt = {
            {"system",
             "You are a careful reasoner. For each user goal, think step "
             "by step and then give a concise final answer. Keep your "
             "answer under 100 words unless the question demands detail."},
            {"user", msg.payload},
        };
        ChatOptions opts;
        opts.max_tokens  = 300;
        opts.temperature = 0.5;
        opts.top_p       = 0.9;
        opts.freq_penalty = 0.1;

        auto r = llm_->chat(prompt, opts);
        Message out{
            .from    = name_,
            .to      = msg.from.empty() ? "stdout" : msg.from,
            .kind    = r.ok ? "final_answer" : "plan_error",
            .payload = r.ok ? r.content : ("[planner] " + r.error),
        };
        rt.send(std::move(out));
    }

private:
    std::string                name_ = "planner";
    std::unique_ptr<LLMClient> llm_;
};

std::unique_ptr<Agent> make_planner() { return std::make_unique<Planner>(); }

}  // namespace rocm_cpp::agents::specialists
