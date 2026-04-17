// muse — voice persona specialist (real implementation).
//
// One job: be the voice of the Man Cave. Holds a conversation history,
// runs replies through librocm_cpp via the shared LLMClient. No voice
// I/O in this specialist — that's echo_ear (STT) and echo_mouth (TTS).
// Muse is just the brain behind the voice.
//
// Contract:
//   listens for : "user_said"       (payload = text)
//                 "reset"           (clears history)
//   emits       : "muse_reply"      (payload = text)  -> to msg.from
//   env         : AGENT_CPP_LLM_URL (default http://127.0.0.1:8080)

#include "agents/agent.h"
#include "agents/runtime.h"
#include "agents/llm_client.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rocm_cpp::agents::specialists {

class Muse : public Agent {
public:
    Muse() {
        const char* url = std::getenv("AGENT_CPP_LLM_URL");
        llm_ = std::make_unique<LLMClient>(url ? url : "http://127.0.0.1:8080");
        history_.push_back({"system",
            "You are Muse — a thoughtful, terse voice running on the Man Cave "
            "at the edge of an AMD Strix Halo box. You speak plainly, in short "
            "sentences. No markdown. No lists unless asked. You have the "
            "architect's back."});
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind == "reset") {
            std::lock_guard<std::mutex> lk(mu_);
            if (history_.size() > 1) history_.resize(1);  // keep system prompt
            return;
        }
        if (msg.kind != "user_said") return;

        ChatResult r;
        {
            std::lock_guard<std::mutex> lk(mu_);
            history_.push_back({"user", msg.payload});
            ChatOptions opts;
            opts.max_tokens  = 180;
            opts.temperature = 0.7;
            opts.top_p       = 0.9;
            opts.freq_penalty = 0.15;
            r = llm_->chat(history_, opts);
            if (r.ok) history_.push_back({"assistant", r.content});
            else      history_.pop_back();  // drop the user turn if we failed
        }

        Message reply{
            .from    = name_,
            .to      = msg.from.empty() ? "stdout" : msg.from,
            .kind    = r.ok ? "muse_reply" : "muse_error",
            .payload = r.ok ? r.content : ("[muse] " + r.error),
        };
        rt.send(std::move(reply));
    }

private:
    std::string              name_ = "muse";
    std::unique_ptr<LLMClient> llm_;
    std::mutex               mu_;
    std::vector<ChatMessage> history_;
};

std::unique_ptr<Agent> make_muse() { return std::make_unique<Muse>(); }

}  // namespace rocm_cpp::agents::specialists
