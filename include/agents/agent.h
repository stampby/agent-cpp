// Agent base interface — one specialist, one job.
//
// Contract:
//   * name()         — short unique identifier, used as message routing key
//   * handle(msg)    — process one message; called on this agent's thread
//   * start() / stop() — lifecycle hooks called by the Runtime
//
// An Agent must NOT block indefinitely inside handle(). If you need long
// work, spawn it from handle() and emit a follow-up message when done.
// If you need to call into librocm_cpp, do it here — that's why this
// layer exists.
//
// An Agent must NOT reach into another Agent. All cross-agent talk goes
// through the Runtime's message bus.

#ifndef ROCM_CPP_AGENTS_AGENT_H
#define ROCM_CPP_AGENTS_AGENT_H

#include "agents/message.h"
#include <string>

namespace rocm_cpp::agents {

class Runtime;  // fwd

class Agent {
public:
    virtual ~Agent() = default;

    virtual const std::string& name() const = 0;

    // Called once after registration, before any handle().
    virtual void start(Runtime& /*rt*/) {}

    // Called before shutdown. After this returns, no more handle() calls.
    virtual void stop() {}

    // Process one message. Guaranteed to be called on this agent's
    // dedicated thread — no re-entrancy. Keep it short.
    virtual void handle(const Message& msg, Runtime& rt) = 0;
};

}  // namespace rocm_cpp::agents
#endif
