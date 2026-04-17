// muse — voice persona specialist (stub).
//
// One job: carry the Man Cave persona. Takes user utterances in, emits
// Muse-flavored replies. Does NOT do STT, TTS, routing, or planning —
// those are separate specialists. Muse is just the voice.
//
// Real decode goes here in a later PR (calls into librocm_cpp). For the
// scaffold, we echo with a fixed line so the message bus can be verified
// end-to-end with zero deps.
//
// Contract:
//   listens for : kind == "user_said",  payload == text
//   emits       : kind == "muse_reply", payload == text

#include "agents/agent.h"
#include "agents/runtime.h"
#include <string>

namespace rocm_cpp::agents::specialists {

class Muse : public Agent {
public:
    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind != "user_said") return;   // not our job
        Message reply{
            .from    = name_,
            .to      = "stdout",  // demo wires the sink; real app routes via forge/tui
            .kind    = "muse_reply",
            .payload = "Greetings, architect. (muse stub — real decode lands in M2.)"
        };
        rt.send(std::move(reply));
    }

private:
    std::string name_ = "muse";
};

}  // namespace rocm_cpp::agents::specialists

// Factory — exposed so main.cpp / tests can construct without a header.
// Keeps the specialist's class itself hidden to enforce easy-in/easy-out.
#include <memory>
namespace rocm_cpp::agents::specialists {
std::unique_ptr<Agent> make_muse() {
    return std::make_unique<Muse>();
}
}  // namespace
