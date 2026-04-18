// sentinel — Discord watcher (read).
//
// One job: watch one or more Discord channels and relay new messages
// onto the bus as "discord_message" events. Does NOT reply — herald
// owns the write side, keeping outbound auditable in one place.
//
// v1 uses REST polling (GET /channels/:id/messages?after=:last_id every
// few seconds). Not gateway-realtime, but dependency-free (reuses the
// httplib+openssl stack we already have) and plenty for the use cases
// we actually care about: PR webhooks via a channel, install-help
// messages for carpenter, and occasional @mentions for herald to
// respond to.
//
// Upgrade path: swap the poll loop for a libwebsockets gateway client
// when we need <1s latency. Contract stays the same.
//
// Contract:
//   listens for : "sentinel_watch"   {channel_id}  (add a new watch)
//                 "sentinel_unwatch" {channel_id}
//                 "reconnect"                       (restart poll thread)
//   emits       : "discord_message"  {channel, author, content, id}
//
// Env:
//   DISCORD_TOKEN              bot token
//   DISCORD_WATCH_CHANNELS     comma-separated channel_id list (optional;
//                              otherwise watch set is built via sentinel_watch)
//   SENTINEL_POLL_MS           poll interval (default 3000)

#include "agents/agent.h"
#include "agents/runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <httplib.h>

namespace rocm_cpp::agents::specialists {

class Sentinel : public Agent {
public:
    Sentinel() {
        if (const char* t = std::getenv("DISCORD_TOKEN"); t && *t) token_ = t;
        if (const char* p = std::getenv("SENTINEL_POLL_MS"); p && *p)
            poll_ms_ = std::max(500, std::atoi(p));
        if (const char* c = std::getenv("DISCORD_WATCH_CHANNELS"); c && *c) {
            std::string s = c, tok;
            for (char ch : s) {
                if (ch == ',') { if (!tok.empty()) watched_.insert(tok); tok.clear(); }
                else if (ch != ' ') tok += ch;
            }
            if (!tok.empty()) watched_.insert(tok);
        }
    }

    ~Sentinel() override { stop(); }

    const std::string& name() const override { return name_; }

    void start(Runtime& rt) override {
        running_ = true;
        poll_thr_ = std::thread([this, &rt]{ this->poll_loop_(rt); });
    }

    void stop() override {
        running_ = false;
        cv_.notify_all();
        if (poll_thr_.joinable()) poll_thr_.join();
    }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind == "sentinel_watch" || msg.kind == "sentinel_unwatch") {
            nlohmann::json j;
            try { j = nlohmann::json::parse(msg.payload); } catch (...) { return; }
            std::string cid = j.value("channel_id", std::string(""));
            if (cid.empty()) return;
            std::lock_guard<std::mutex> lk(mu_);
            if (msg.kind == "sentinel_watch") watched_.insert(cid);
            else                              watched_.erase(cid);
            cv_.notify_all();
            return;
        }
        if (msg.kind == "reconnect") {
            std::lock_guard<std::mutex> lk(mu_);
            last_id_.clear();      // re-sync against "now"
            cv_.notify_all();
            return;
        }
        if (msg.kind == "fetch_recent") return fetch_recent_(msg, rt);
    }

private:
    std::string       name_  = "sentinel";
    std::string       token_;
    int               poll_ms_ = 3000;
    std::thread       poll_thr_;
    std::atomic<bool> running_{false};
    std::mutex        mu_;
    std::condition_variable cv_;
    std::unordered_set<std::string>               watched_;
    std::unordered_map<std::string, std::string>  last_id_;  // channel → last seen msg id

    void poll_loop_(Runtime& rt) {
        if (token_.empty()) {
            std::fprintf(stderr, "[sentinel] DISCORD_TOKEN not set — polling disabled\n");
            return;
        }
        while (running_) {
            std::unordered_set<std::string> snapshot;
            {
                std::lock_guard<std::mutex> lk(mu_);
                snapshot = watched_;
            }
            for (const auto& channel : snapshot) {
                if (!running_) break;
                poll_channel_(rt, channel);
            }
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(poll_ms_),
                         [&]{ return !running_; });
        }
    }

    // Synchronous "give me the last N messages" for MCP tool callers.
    // Separate from the poll_loop_ — does one REST call, returns the
    // raw Discord messages array. No bus event dedup.
    void fetch_recent_(const Message& msg, Runtime& rt) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) {
            nlohmann::json er = {{"error", std::string("bad JSON: ") + e.what()}};
            rt.send({.from=name_, .to=msg.from, .kind="fetch_recent_error", .payload=er.dump()});
            return;
        }
        std::string channel = j.value("channel_id", std::string(""));
        int limit = std::max(1, std::min(j.value("limit", 25), 100));
        if (channel.empty()) {
            nlohmann::json er = {{"error", "channel_id required"}};
            rt.send({.from=name_, .to=msg.from, .kind="fetch_recent_error", .payload=er.dump()});
            return;
        }
        if (token_.empty()) {
            nlohmann::json er = {{"error", "DISCORD_TOKEN not set"}};
            rt.send({.from=name_, .to=msg.from, .kind="fetch_recent_error", .payload=er.dump()});
            return;
        }

        httplib::Client cli("https://discord.com");
        cli.set_connection_timeout(5);
        cli.set_read_timeout(15);
        httplib::Headers headers{
            {"Authorization", "Bot " + token_},
            {"User-Agent",    "agent-cpp/sentinel"},
        };
        std::string path = "/api/v10/channels/" + channel + "/messages?limit=" + std::to_string(limit);
        auto res = cli.Get(path, headers);
        if (!res) {
            nlohmann::json er = {{"error", "Discord unreachable"}};
            rt.send({.from=name_, .to=msg.from, .kind="fetch_recent_error", .payload=er.dump()});
            return;
        }
        if (res->status < 200 || res->status >= 300) {
            nlohmann::json er = {{"error", "HTTP " + std::to_string(res->status)},
                                 {"body", res->body.substr(0, 300)}};
            rt.send({.from=name_, .to=msg.from, .kind="fetch_recent_error", .payload=er.dump()});
            return;
        }
        nlohmann::json data;
        try { data = nlohmann::json::parse(res->body); }
        catch (const std::exception& e) {
            nlohmann::json er = {{"error", std::string("parse: ") + e.what()}};
            rt.send({.from=name_, .to=msg.from, .kind="fetch_recent_error", .payload=er.dump()});
            return;
        }
        nlohmann::json out = {{"channel_id", channel}, {"messages", data}};
        rt.send({.from=name_, .to=msg.from, .kind="fetch_recent_result", .payload=out.dump()});
    }

    void poll_channel_(Runtime& rt, const std::string& channel) {
        httplib::Client cli("https://discord.com");
        cli.set_connection_timeout(5);
        cli.set_read_timeout(15);
        httplib::Headers headers{
            {"Authorization", "Bot " + token_},
            {"User-Agent",    "agent-cpp/sentinel"},
        };
        std::string path = "/api/v10/channels/" + channel + "/messages?limit=25";
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = last_id_.find(channel);
            if (it != last_id_.end() && !it->second.empty())
                path += "&after=" + it->second;
        }
        auto res = cli.Get(path, headers);
        if (!res) return;
        if (res->status == 429) {
            // Respect retry-after (seconds fractional).
            double retry = 1.0;
            try { retry = nlohmann::json::parse(res->body).value(
                "retry_after", 1.0); } catch (...) {}
            std::this_thread::sleep_for(
                std::chrono::milliseconds((int)(retry * 1000)));
            return;
        }
        if (res->status < 200 || res->status >= 300) {
            std::fprintf(stderr, "[sentinel] channel %s: HTTP %d\n",
                         channel.c_str(), res->status);
            return;
        }

        nlohmann::json arr;
        try { arr = nlohmann::json::parse(res->body); }
        catch (...) { return; }
        if (!arr.is_array() || arr.empty()) return;

        // Discord returns newest-first. Iterate reversed so bus order
        // matches send order. Track the highest id for next poll.
        std::string max_id;
        for (auto it = arr.rbegin(); it != arr.rend(); ++it) {
            const auto& m = *it;
            std::string id      = m.value("id", std::string(""));
            std::string content = m.value("content", std::string(""));
            std::string author  = m.contains("author")
                ? m["author"].value("username", std::string("")) : std::string();
            std::string author_id = m.contains("author")
                ? m["author"].value("id",       std::string("")) : std::string();
            nlohmann::json out = {
                {"channel",   channel},
                {"id",        id},
                {"author",    author},
                {"author_id", author_id},
                {"content",   content},
            };
            rt.send({.from=name_, .to="",  // broadcast — interested agents filter
                     .kind="discord_message", .payload=out.dump()});
            if (id > max_id) max_id = id;   // Discord ids are snowflakes; > works
        }
        if (!max_id.empty()) {
            std::lock_guard<std::mutex> lk(mu_);
            last_id_[channel] = max_id;
        }
    }
};

std::unique_ptr<Agent> make_sentinel() { return std::make_unique<Sentinel>(); }

}  // namespace rocm_cpp::agents::specialists
