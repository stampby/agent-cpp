// agent-cpp — CLI entry point for the specialist runtime.
//
// Spins up a small working lineup:
//
//   stdin          -- user lines come in
//   muse           -- chats via the rocm-cpp HTTP server
//   planner        -- ReAct-style reasoner
//   forge + warden -- tool dispatch + permission check
//   stdout_sink    -- prints every message to the terminal
//
// Each specialist lives on its own thread, talks via the message bus,
// and has exactly one job. Running binary:
//
//   bitnet_decode model.h1b --server 8080   # terminal A
//   agent_cpp                               # terminal B
//   > hello                                 # routed to muse
//   > plan: <goal>                          # routed to planner
//   > tool: clock                           # fires a forge/warden round-trip
//
// Ctrl-D or "quit" to exit.

#include "agents/runtime.h"

#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace rocm_cpp::agents::specialists {
std::unique_ptr<Agent> make_muse();
std::unique_ptr<Agent> make_planner();
std::unique_ptr<Agent> make_forge();
std::unique_ptr<Agent> make_warden();
std::unique_ptr<Agent> make_cartograph();
std::unique_ptr<Agent> make_scribe();
std::unique_ptr<Agent> make_sommelier();
std::unique_ptr<Agent> make_stdout_sink();
}  // namespace

using namespace rocm_cpp::agents;

static Runtime* g_rt = nullptr;
static void on_sigint(int) { if (g_rt) g_rt->shutdown(); }

int main() {
    Runtime rt;
    g_rt = &rt;
    std::signal(SIGINT,  on_sigint);
    std::signal(SIGTERM, on_sigint);

    rt.register_agent(specialists::make_muse());
    rt.register_agent(specialists::make_planner());
    rt.register_agent(specialists::make_forge());
    rt.register_agent(specialists::make_warden());
    rt.register_agent(specialists::make_cartograph());
    rt.register_agent(specialists::make_scribe());
    rt.register_agent(specialists::make_sommelier());
    rt.register_agent(specialists::make_stdout_sink());
    rt.set_audit("scribe");  // journal every routed message

    std::thread stdin_thr([&] {
        std::fprintf(stderr,
            "agent-cpp: prefix a line with 'plan:', 'tool:', 'remember:', 'recall:', or nothing.\n"
            "  <text>            -> muse (chat)\n"
            "  plan: <goal>      -> planner\n"
            "  tool: <tool> [k=v ...]  -> forge (via warden)\n"
            "  remember: <text>  -> cartograph\n"
            "  recall: <query>   -> cartograph\n"
            "  quit / Ctrl-D to exit\n\n");

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "quit" || line == "exit") break;
            if (line.empty()) continue;

            if (line.rfind("plan:", 0) == 0) {
                std::string goal = line.substr(5);
                while (!goal.empty() && goal.front() == ' ') goal.erase(goal.begin());
                rt.send({.from="stdout", .to="planner",
                         .kind="user_goal", .payload=goal});
            }
            else if (line.rfind("tool:", 0) == 0) {
                std::string rest = line.substr(5);
                while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
                // Parse "<tool_name> k=v k=v"
                auto space = rest.find(' ');
                std::string tool = (space == std::string::npos) ? rest : rest.substr(0, space);
                nlohmann::json args = nlohmann::json::object();
                if (space != std::string::npos) {
                    std::string kv = rest.substr(space + 1);
                    size_t start = 0;
                    while (start < kv.size()) {
                        size_t sp = kv.find(' ', start);
                        std::string tok = kv.substr(start, sp - start);
                        auto eq = tok.find('=');
                        if (eq != std::string::npos)
                            args[tok.substr(0, eq)] = tok.substr(eq + 1);
                        if (sp == std::string::npos) break;
                        start = sp + 1;
                    }
                }
                nlohmann::json body = {{"tool", tool}, {"args", args}};
                rt.send({.from="stdout", .to="forge",
                         .kind="tool_call", .payload=body.dump()});
            }
            else if (line.rfind("route:", 0) == 0) {
                // route: <hint> <prompt>
                std::string rest = line.substr(6);
                while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
                auto sp = rest.find(' ');
                std::string hint = (sp == std::string::npos) ? rest : rest.substr(0, sp);
                std::string prompt = (sp == std::string::npos) ? "" : rest.substr(sp + 1);
                nlohmann::json body = {
                    {"hint",       hint},
                    {"max_tokens", 120},
                    {"messages", {{{"role","user"},{"content",prompt}}}},
                };
                rt.send({.from="stdout", .to="sommelier",
                         .kind="decode_request", .payload=body.dump()});
            }
            else if (line == "backends") {
                rt.send({.from="stdout", .to="sommelier",
                         .kind="list_backends", .payload="{}"});
            }
            else if (line.rfind("remember:", 0) == 0) {
                std::string text = line.substr(9);
                while (!text.empty() && text.front() == ' ') text.erase(text.begin());
                nlohmann::json body = {{"text", text}};
                rt.send({.from="stdout", .to="cartograph",
                         .kind="remember", .payload=body.dump()});
            }
            else if (line.rfind("recall:", 0) == 0) {
                std::string q = line.substr(7);
                while (!q.empty() && q.front() == ' ') q.erase(q.begin());
                nlohmann::json body = {{"query", q}, {"k", 3}};
                rt.send({.from="stdout", .to="cartograph",
                         .kind="recall", .payload=body.dump()});
            }
            else {
                rt.send({.from="stdout", .to="muse",
                         .kind="user_said", .payload=line});
            }
        }
        // Give in-flight specialist work (LLM HTTP calls can take 300-1000ms)
        // a chance to finish before shutdown. Ctrl-C short-circuits this.
        std::fprintf(stderr, "\n[agent-cpp] draining inboxes (10s)…\n");
        std::this_thread::sleep_for(std::chrono::seconds(10));
        rt.shutdown();
    });

    rt.run();
    if (stdin_thr.joinable()) stdin_thr.detach();
    return 0;
}
