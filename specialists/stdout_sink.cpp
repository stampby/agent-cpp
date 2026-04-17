// stdout_sink — terminal echo specialist (core-ish).
//
// One job: print every message it receives to stdout, prefixed with
// source + kind. Useful as a debug sink, as the default route for
// Muse's replies during scaffold tests, and as a smoke-test target.
//
// Contract:
//   listens for : anything
//   emits       : nothing

#include "agents/agent.h"
#include "agents/runtime.h"
#include <cstdio>
#include <memory>

namespace rocm_cpp::agents::specialists {

class StdoutSink : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& msg, Runtime& /*rt*/) override {
        std::printf("[%s -> %s : %s] %s\n",
                    msg.from.c_str(), msg.to.c_str(),
                    msg.kind.c_str(), msg.payload.c_str());
        std::fflush(stdout);
    }
private:
    std::string name_ = "stdout";
};

std::unique_ptr<Agent> make_stdout_sink() { return std::make_unique<StdoutSink>(); }

}  // namespace
