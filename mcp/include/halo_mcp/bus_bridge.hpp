#pragma once

// BusBridge — a specialist-shaped Agent that lives in halo-mcp.
//
// Phase 1 design: one in-flight request per target specialist. That's
// enough for Claude Code calling tools one at a time, and avoids needing
// specialists to echo a correlation_id in their replies. A Phase 1.5
// upgrade could use a pool of bridge agents (halo-mcp-bridge-1..N) to
// allow concurrent requests to the same specialist.
//
// Contract:
//   send_request(target, kind, payload) — called from the MCP stdio thread.
//     Registers a pending promise keyed by `target`, emits a Message to that
//     target, blocks on the future with a configurable timeout.
//   handle(msg)                          — called from the Runtime thread.
//     Looks up pending_[msg.from]; fulfills the promise with msg.payload.
//     Messages without a pending entry are ignored (broadcast / unsolicited).
//
// If a second request is issued while the first to the same target is
// in-flight, send_request() returns an error immediately (the MCP client
// gets -32001). Callers can retry.

#include "agents/agent.h"
#include "agents/runtime.h"

#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>

namespace halo_mcp {

struct BridgeReply {
    bool ok = false;
    std::string payload;   // on success: specialist reply payload (usually JSON)
    std::string error;     // on error: human-readable reason
};

class BusBridge : public rocm_cpp::agents::Agent {
public:
    explicit BusBridge(std::chrono::milliseconds timeout);
    const std::string& name() const override { return name_; }
    void handle(const rocm_cpp::agents::Message& msg,
                rocm_cpp::agents::Runtime& rt) override;

    // Called from the MCP stdio thread. Blocks up to the configured timeout.
    BridgeReply send_request(rocm_cpp::agents::Runtime& rt,
                             const std::string& target,
                             const std::string& kind,
                             const std::string& payload);

private:
    std::string name_{"halo-mcp-bridge"};
    std::chrono::milliseconds timeout_;
    std::mutex mu_;
    std::unordered_map<std::string, std::promise<BridgeReply>> pending_;
};

}
