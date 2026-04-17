// sentinel — Discord watcher (read).
//
// One job: hold the Discord gateway WebSocket, watch every channel in
// the configured guild, and relay events onto the message bus as
// typed inbound messages. Does NOT reply — that's herald's job.
// Keeps the read/write split clean so we can audit outbound traffic.
//
// Contract:
//   listens for : "reconnect" (operator asks for a fresh gateway session)
//   emits       : "discord_message" (payload = JSON {channel, author, content, id})
//                 "discord_reaction"
//                 "discord_member_join"
// Auth         : DISCORD_TOKEN env (bot token, scope: Gateway + message content)

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Sentinel : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: gateway WS client (libwebsockets or cpprestsdk), heartbeat,
        //       identify with MESSAGE_CONTENT intent, relay events.
    }
private:
    std::string name_ = "sentinel";
};
std::unique_ptr<Agent> make_sentinel() { return std::make_unique<Sentinel>(); }
}  // namespace
