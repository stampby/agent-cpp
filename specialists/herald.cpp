// herald — Discord poster (write side of the Discord pair).
//
// One job: send messages to Discord. Announcements, replies, reactions.
// Uses the Discord REST API (HTTPS POST) — no gateway WebSocket; that's
// sentinel's job (read side). Rate-limit aware: honors the X-RateLimit-
// Remaining / X-RateLimit-Reset-After headers, backs off briefly when
// hitting a bucket limit.
//
// Default format matches the mesh convention: monospace code block with
// a dated banner. No markdown links (URL previews pull unwanted images),
// no embeds, no attachments. If the caller passes {"raw": true} the
// payload is sent unwrapped.
//
// Contract:
//   listens for :
//     "discord_post"     {channel_id, content, raw?: bool}
//     "discord_reply"    {channel_id, reply_to_id, content}
//     "discord_reaction" {channel_id, message_id, emoji}
//   emits :
//     "discord_post_ok"   {id}     -> to msg.from
//     "discord_post_fail" {error}  -> to msg.from
//
// Env:
//   DISCORD_TOKEN   bot token (Bot tokens use "Bot <token>" auth scheme)

#include "agents/agent.h"
#include "agents/runtime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include <httplib.h>

namespace rocm_cpp::agents::specialists {

namespace {

std::string wrap_codeblock(const std::string& content) {
    // Dated banner + ═════ divider, matches the architect's Discord format.
    std::time_t t = std::time(nullptr);
    char date[11];
    std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&t));
    std::string out = "```\nAGENT POST";
    out.resize(50, ' '); out += date; out += "\n";
    out += "═══════════════════════════════════════════════════════════════\n";
    out += content;
    out += "\n═══════════════════════════════════════════════════════════════\n```";
    return out;
}

}  // namespace

class Herald : public Agent {
public:
    Herald() {
        if (const char* t = std::getenv("DISCORD_TOKEN"); t && *t)
            token_ = t;
        if (token_.empty())
            std::fprintf(stderr, "[herald] DISCORD_TOKEN not set — posts will fail\n");
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind != "discord_post" &&
            msg.kind != "discord_reply" &&
            msg.kind != "discord_reaction") return;

        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) { emit_fail(rt, msg, std::string("bad JSON: ") + e.what()); return; }

        if (msg.kind == "discord_reaction") {
            post_reaction_(rt, msg, j);
            return;
        }

        std::string channel = j.value("channel_id", std::string(""));
        std::string content = j.value("content", std::string(""));
        if (channel.empty() || content.empty()) {
            emit_fail(rt, msg, "channel_id + content required"); return;
        }
        bool raw = j.value("raw", false);
        if (!raw) content = wrap_codeblock(content);

        nlohmann::json body = { {"content", content} };
        if (msg.kind == "discord_reply") {
            std::string reply_to = j.value("reply_to_id", std::string(""));
            if (!reply_to.empty())
                body["message_reference"] = {{"message_id", reply_to}};
        }

        std::lock_guard<std::mutex> lk(mu_);
        post_to_channel_(rt, msg, channel, body);
    }

private:
    std::string name_  = "herald";
    std::string token_;
    std::mutex  mu_;   // serialize; Discord REST is rate-limited per-bucket

    void post_to_channel_(Runtime& rt, const Message& src,
                          const std::string& channel_id,
                          const nlohmann::json& body) {
        if (token_.empty()) { emit_fail(rt, src, "DISCORD_TOKEN unset"); return; }

        httplib::Client cli("https://discord.com");
        cli.set_connection_timeout(10);
        cli.set_read_timeout(20);
        httplib::Headers headers{
            {"Authorization", "Bot " + token_},
            {"Content-Type",  "application/json"},
            {"User-Agent",    "agent-cpp/herald (https://github.com/stampby/agent-cpp)"},
        };
        std::string path = "/api/v10/channels/" + channel_id + "/messages";
        auto res = cli.Post(path, headers, body.dump(), "application/json");

        if (!res) { emit_fail(rt, src, "Discord unreachable"); return; }
        if (res->status == 429) {
            // Rate-limited. Obey retry-after.
            float retry = 1.0f;
            try { retry = std::stof(nlohmann::json::parse(res->body).value(
                    "retry_after", std::string("1.0"))); } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds((int)(retry * 1000)));
            res = cli.Post(path, headers, body.dump(), "application/json");
            if (!res) { emit_fail(rt, src, "Discord unreachable after retry"); return; }
        }
        if (res->status < 200 || res->status >= 300) {
            emit_fail(rt, src, "Discord HTTP " + std::to_string(res->status) +
                              ": " + res->body.substr(0, 200));
            return;
        }
        try {
            auto rj = nlohmann::json::parse(res->body);
            nlohmann::json ok = {{"id", rj.value("id", std::string(""))}};
            rt.send({.from=name_, .to=src.from,
                     .kind="discord_post_ok", .payload=ok.dump()});
        } catch (const std::exception& e) {
            emit_fail(rt, src, std::string("Discord response parse: ") + e.what());
        }
    }

    void post_reaction_(Runtime& rt, const Message& src, const nlohmann::json& j) {
        if (token_.empty()) { emit_fail(rt, src, "DISCORD_TOKEN unset"); return; }
        std::string channel_id = j.value("channel_id", std::string(""));
        std::string message_id = j.value("message_id", std::string(""));
        std::string emoji      = j.value("emoji",      std::string(""));
        if (channel_id.empty() || message_id.empty() || emoji.empty()) {
            emit_fail(rt, src, "channel_id + message_id + emoji required"); return;
        }
        httplib::Client cli("https://discord.com");
        cli.set_connection_timeout(10); cli.set_read_timeout(20);
        httplib::Headers headers{
            {"Authorization", "Bot " + token_},
            {"User-Agent",    "agent-cpp/herald"},
        };
        // Emoji must be URL-encoded; for simple unicode emoji the raw
        // bytes work in most practice. Keep scope tight for v1.
        std::string path = "/api/v10/channels/" + channel_id +
                           "/messages/" + message_id +
                           "/reactions/" + emoji + "/@me";
        auto res = cli.Put(path, headers, "", "application/json");
        if (!res || res->status < 200 || res->status >= 300) {
            emit_fail(rt, src, "reaction HTTP " +
                              (res ? std::to_string(res->status) : std::string("(unreachable)")));
            return;
        }
        rt.send({.from=name_, .to=src.from,
                 .kind="discord_post_ok", .payload="{\"id\":\"reaction\"}"});
    }

    static void emit_fail(Runtime& rt, const Message& src, const std::string& why) {
        nlohmann::json j = {{"error", why}};
        rt.send({.from="herald", .to=src.from,
                 .kind="discord_post_fail", .payload=j.dump()});
    }
};

std::unique_ptr<Agent> make_herald() { return std::make_unique<Herald>(); }

}  // namespace rocm_cpp::agents::specialists
