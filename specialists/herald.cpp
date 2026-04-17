// herald — Discord poster (write).
//
// One job: send messages to Discord. Announcements, bot replies,
// reactions, edits. Uses code-block format by default (per project
// convention — no link previews, no embeds, no user avatar leakage).
//
// Contract:
//   listens for : "discord_post"       (payload = JSON {channel_id, content})
//                 "discord_reply"      (payload = JSON {channel_id, reply_to, content})
//                 "discord_reaction"   (payload = JSON {message_id, emoji})
//   emits       : "discord_post_ok"    (payload = JSON {id})
//                 "discord_post_fail"  (payload = JSON {error})
// Auth         : DISCORD_TOKEN env. Writes rate-limited to stay under Discord's buckets.

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Herald : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: libcurl POST to Discord REST API, rate-limit bucket awareness.
    }
private:
    std::string name_ = "herald";
};
std::unique_ptr<Agent> make_herald() { return std::make_unique<Herald>(); }
}  // namespace
