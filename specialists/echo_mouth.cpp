// echo_mouth — Kokoro text-to-speech bridge.
//
// One job: take the model's text output and speak it via Kokoro TTS
// running as a local systemd service. Streams audio chunks back so
// the Man Cave visual can draw a waveform while Muse talks.
//
// Contract:
//   listens for : "muse_reply" (payload = text)
//                 "tts_stop"   (interrupt current utterance)
//   emits       : "tts_audio_chunk" (payload = binary PCM, for visualizer)
//                 "tts_done"        (utterance finished)
// Dep          : kokoro-tts.service HTTP API on localhost

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class EchoMouth : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: POST to kokoro /tts, stream audio to speaker + visualizer.
    }
private:
    std::string name_ = "echo_mouth";
};
std::unique_ptr<Agent> make_echo_mouth() { return std::make_unique<EchoMouth>(); }
}  // namespace
