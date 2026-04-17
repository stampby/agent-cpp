// Typed message envelope for inter-agent communication.
//
// Messages are the ONLY way specialists talk to each other. No shared
// state, no direct method calls, no "grab the other agent's pointer."
// If you find yourself wanting to bypass this, you're violating the
// one-agent-one-job rule.

#ifndef ROCM_CPP_AGENTS_MESSAGE_H
#define ROCM_CPP_AGENTS_MESSAGE_H

#include <chrono>
#include <cstdint>
#include <string>

namespace rocm_cpp::agents {

struct Message {
    std::string from;           // sender agent name (e.g., "muse")
    std::string to;             // target agent name, or "*" for broadcast
    std::string kind;           // message type (e.g., "user_said", "tool_result")
    std::string payload;        // JSON or raw text — specialist decides
    uint64_t    id = 0;         // monotonic, set by Runtime on enqueue
    std::chrono::steady_clock::time_point ts{std::chrono::steady_clock::now()};
};

}  // namespace rocm_cpp::agents
#endif
