// echo_ear — Whisper.cpp speech-to-text bridge.
//
// One job: listen on a unix socket for raw PCM chunks from the mic
// capture process, run whisper.cpp, emit transcribed utterances.
// Push-to-talk semantics in v1 (Man Cave page), continuous behind flag.
//
// Contract:
//   listens for : "mic_pcm" (payload = binary chunk, typically from OS audio)
//                 "mic_eou" (end of utterance — trigger flush)
//   emits       : "user_said" (payload = transcribed text)
// Dep          : whisper.cpp server running locally (systemd user service)

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class EchoEar : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: POST to whisper-server /inference, relay transcript as user_said.
    }
private:
    std::string name_ = "echo_ear";
};
std::unique_ptr<Agent> make_echo_ear() { return std::make_unique<EchoEar>(); }
}  // namespace
