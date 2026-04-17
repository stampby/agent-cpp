// Runtime — owns agents, routes messages, drives shutdown.
//
// Each registered Agent gets its own dedicated thread and an inbox
// (std::queue guarded by a mutex/condvar). send() appends to the
// target's inbox; the target's thread drains and dispatches.
//
// There is intentionally NO direct Agent-to-Agent pointer. If a
// specialist wants to talk to another, it calls Runtime::send().
// That keeps the one-job contract tight.

#ifndef ROCM_CPP_AGENTS_RUNTIME_H
#define ROCM_CPP_AGENTS_RUNTIME_H

#include "agents/agent.h"
#include "agents/message.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rocm_cpp::agents {

class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Register an agent. Must be called before run(). Agent is started
    // on its own thread when run() is invoked.
    void register_agent(std::unique_ptr<Agent> a);

    // Enqueue a message for the named target. Returns false if target
    // doesn't exist or the runtime is shutting down.
    bool send(Message msg);

    // Start all agents, block until shutdown() is called.
    void run();

    // Signal shutdown. Drains inboxes, calls stop() on each agent,
    // joins threads. Idempotent.
    void shutdown();

    // Optional "tap" — if set, every successfully-routed message is
    // also delivered as a copy to the named agent (typically "scribe"
    // for session logging). Set BEFORE run(). Empty string disables.
    void set_audit(std::string agent_name) { audit_ = std::move(agent_name); }

private:
    struct Mailbox {
        std::mutex m;
        std::condition_variable cv;
        std::queue<Message> q;
        std::unique_ptr<Agent> agent;
        std::thread thr;
    };

    void worker_loop(Mailbox& mb);

    std::unordered_map<std::string, std::unique_ptr<Mailbox>> agents_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> next_id_{1};
    std::string audit_;
};

}  // namespace rocm_cpp::agents
#endif
