// agent-cpp — CLI entry point for the specialist runtime.
//
// Minimal demo wiring: one source (stdin), one brain (muse), one sink
// (stdout). Type text + enter, muse receives "user_said", emits a
// reply, stdout prints it. Ctrl-D / Ctrl-C to quit.
//
// Real builds compose many more specialists; this main is intentionally
// tiny so contributors see the wiring in one file.

#include "agents/runtime.h"
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace rocm_cpp::agents::specialists {
std::unique_ptr<Agent> make_muse();
std::unique_ptr<Agent> make_stdout_sink();
}  // namespace

using namespace rocm_cpp::agents;

static Runtime* g_rt = nullptr;
static void on_sigint(int) { if (g_rt) g_rt->shutdown(); }

int main() {
    Runtime rt;
    g_rt = &rt;
    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);

    rt.register_agent(specialists::make_muse());
    rt.register_agent(specialists::make_stdout_sink());

    // Stdin reader thread: each line -> {kind: "user_said", to: "muse"}
    std::thread stdin_thr([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            rt.send({ .from = "stdin", .to = "muse",
                      .kind = "user_said", .payload = line });
        }
        rt.shutdown();
    });

    std::fprintf(stderr, "agent-cpp: type, press enter. Ctrl-D / Ctrl-C to quit.\n");
    rt.run();   // blocks until shutdown
    if (stdin_thr.joinable()) stdin_thr.detach();
    return 0;
}
