// carpenter — install-support helper.
//
// One job: help beta testers get install.sh working. Reads pasted
// error output, diagnoses with a short regex table first (fast, no
// LLM for known failures), delegates novel cases to sommelier for a
// real-time reply. Posts the diagnosis back via herald.
//
// Contract:
//   listens for :
//     "install_log"     {log, channel_id, reply_to_id?} — direct paste
//     "discord_message" {channel, content, id} — Discord read-side
//                                                (filtered to help channel)
//     "decode_result"   — sommelier reply arriving after delegate
//   emits :
//     "discord_reply"   (via herald) with the diagnosis
//     "decode_request"  (to sommelier) for novel failures
//
// Env: AGENT_CPP_INSTALL_HELP_CHANNEL — Discord channel_id to watch.
//      Unset = only respond to direct install_log messages.

#include "agents/agent.h"
#include "agents/runtime.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rocm_cpp::agents::specialists {

namespace {

struct KnownFix {
    std::regex   pattern;
    std::string  short_name;
    std::string  fix;
};

// Add new entries here — order matters (first match wins).
const std::vector<KnownFix>& rules() {
    static const std::vector<KnownFix> r = {
        {std::regex("hipcc: command not found|cannot find -lhip"),
         "rocm-missing",
         "ROCm/HIP not found.\n"
         "  1. Install ROCm 7.x from AUR (`rocm-hip-sdk`) or TheRock.\n"
         "  2. `export PATH=/opt/rocm/bin:$PATH`\n"
         "  3. Confirm with `hipcc --version`."},
        {std::regex("clang(\\+\\+)?: (command not found|not found)"),
         "clang-missing",
         "clang not on PATH. Install `clang` (or `rocm-llvm`) and rerun."},
        {std::regex("git lfs|Git LFS|git-lfs"),
         "lfs-missing",
         "Git LFS not installed. `pacman -S git-lfs && git lfs install` "
         "then re-clone or `git lfs pull` in-tree."},
        {std::regex("gfx90[0-9]|gfx103[0-9]|wrong target|unsupported target"),
         "wrong-gfx",
         "Wrong GPU target. This stack is gfx1151 (Strix Halo) only.\n"
         "Set `CMAKE_HIP_ARCHITECTURES=gfx1151` or unset it and let us detect."},
        {std::regex("Permission denied|EACCES"),
         "permission",
         "Permission denied. Make sure you're running install.sh as your "
         "user (not root) and HOME is writable. Don't `sudo` the whole script."},
        {std::regex("disk full|No space left|ENOSPC"),
         "disk-full",
         "Disk full during build. Need ~20 GiB free for the full build "
         "(kernels + artifacts). `df -h .` to check, clear space, rerun."},
        {std::regex("CUDA|nvcc|nvidia"),
         "wrong-vendor",
         "You hit a CUDA reference — this stack is AMD/ROCm only. You may "
         "be in a toolbox or container with NVIDIA envs set. Start fresh."},
    };
    return r;
}

const KnownFix* match_known(const std::string& log) {
    for (const auto& r : rules())
        if (std::regex_search(log, r.pattern)) return &r;
    return nullptr;
}

}  // namespace

class Carpenter : public Agent {
public:
    Carpenter() {
        if (const char* c = std::getenv("AGENT_CPP_INSTALL_HELP_CHANNEL"); c && *c)
            help_channel_ = c;
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind == "install_log")      return on_log_(msg, rt);
        if (msg.kind == "discord_message")  return on_discord_(msg, rt);
        if (msg.kind == "decode_result")    return on_decode_(msg, rt);
    }

private:
    std::string name_ = "carpenter";
    std::string help_channel_;

    // Correlate outgoing LLM requests with where to post the reply.
    std::mutex pending_mu_;
    struct Pending { std::string channel_id; std::string reply_to_id; };
    std::unordered_map<uint64_t, Pending> pending_;

    void post_reply_(Runtime& rt, const std::string& channel,
                     const std::string& reply_to, const std::string& body) {
        if (channel.empty()) return;
        nlohmann::json pb = {{"channel_id", channel}, {"content", body}};
        std::string kind = "discord_post";
        if (!reply_to.empty()) {
            pb["reply_to_id"] = reply_to;
            kind = "discord_reply";
        }
        rt.send({.from=name_, .to="herald",
                 .kind=kind, .payload=pb.dump()});
    }

    // --- "install_log" entrypoint (direct paste, typically from a pipeline)
    void on_log_(const Message& msg, Runtime& rt) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (...) { return; }
        std::string log      = j.value("log",         std::string(""));
        std::string channel  = j.value("channel_id",  std::string(""));
        std::string reply_to = j.value("reply_to_id", std::string(""));
        diagnose_(rt, msg.id, log, channel, reply_to);
    }

    // --- "discord_message" entrypoint (sentinel relays gateway events)
    void on_discord_(const Message& msg, Runtime& rt) {
        if (help_channel_.empty()) return;   // no channel configured → ignore
        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (...) { return; }
        if (j.value("channel", std::string("")) != help_channel_) return;
        std::string content = j.value("content", std::string(""));
        std::string mid     = j.value("id",      std::string(""));
        if (content.empty()) return;
        diagnose_(rt, msg.id, content, help_channel_, mid);
    }

    // --- sommelier comes back with a generated diagnosis
    void on_decode_(const Message& msg, Runtime& rt) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (...) { return; }

        Pending p;
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            auto it = pending_.find(msg.id);
            if (it == pending_.end()) return;   // not ours
            p = it->second;
            pending_.erase(it);
        }
        std::string content = j.value("content", std::string("(no content)"));
        post_reply_(rt, p.channel_id, p.reply_to_id,
                    "I couldn't match this to a known failure, here's my best guess:\n\n"
                    + content);
    }

    // Shared diagnosis pipeline. Hit known rules first; delegate to LLM
    // if nothing matches.
    void diagnose_(Runtime& rt, uint64_t origin_id, const std::string& log,
                   const std::string& channel, const std::string& reply_to) {
        if (const auto* known = match_known(log)) {
            post_reply_(rt, channel, reply_to,
                        "Looks like: **" + known->short_name + "**\n\n" + known->fix);
            return;
        }
        // Unknown → delegate to sommelier for an LLM read.
        nlohmann::json req = {
            {"hint",       "local"},
            {"max_tokens", 220},
            {"temperature", 0.2},
            {"messages", nlohmann::json::array({
                {{"role","system"},
                 {"content","You are carpenter, an install-support assistant for "
                            "a HIP/ROCm build on gfx1151 (Strix Halo). Given an "
                            "error log, respond with 3-5 numbered steps, concrete, "
                            "no fluff."}},
                {{"role","user"},
                 {"content", log}},
            })},
        };
        // Stash context so we know where to reply when the result lands.
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            pending_[origin_id] = {channel, reply_to};
        }
        // We use origin_id as the request id — sommelier echoes msg.from
        // back, but the message id on decode_result is new. We key on the
        // carpenter-originated id by setting that as our tag — runtime doesn't
        // let us force an id directly, so we just accept single-flight v1.
        rt.send({.from=name_, .to="sommelier",
                 .kind="decode_request", .payload=req.dump()});
    }
};

std::unique_ptr<Agent> make_carpenter() { return std::make_unique<Carpenter>(); }

}  // namespace rocm_cpp::agents::specialists
