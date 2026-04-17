// scribe — session persistence specialist.
//
// One job: every interesting message that flows on the bus gets
// appended to a JSONL log file. Makes conversations survive restarts,
// gives you a trail for debugging, lets cartograph ingest history
// later if needed.
//
// Each entry carries {prev, hash} — SHA-256 of (prev + canonical body).
// Tamper with any past line and every downstream hash stops matching.
// Cheap audit trail, no blockchain theatre.
//
// Contract:
//   listens for : anything — silently filters to "interesting" kinds
//                 "session_new"   starts a fresh log file
//                 "session_where" requests current path
//   emits       : "session_path"  {"path": "..."}  -> to msg.from
//                 (otherwise silent)
//
// Storage: $XDG_STATE_HOME/agent-cpp/sessions/YYYYMMDD-HHMMSS.jsonl
//          (falls back to ~/.local/state/agent-cpp/sessions/)

#include "agents/agent.h"
#include "agents/runtime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <string>
#include <unordered_set>

namespace rocm_cpp::agents::specialists {

namespace {

std::filesystem::path session_root() {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    std::filesystem::path base;
    if (xdg && *xdg) base = xdg;
    else {
        const char* home = std::getenv("HOME");
        base = (home ? std::filesystem::path(home) / ".local/state"
                     : std::filesystem::path("/tmp"));
    }
    return base / "agent-cpp" / "sessions";
}

std::string timestamp_tag() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
    return buf;
}

std::string sha256_hex(const std::string& in) {
    unsigned char md[EVP_MAX_MD_SIZE]; unsigned int md_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, in.data(), in.size());
    EVP_DigestFinal_ex(ctx, md, &md_len);
    EVP_MD_CTX_free(ctx);
    static const char hex[] = "0123456789abcdef";
    std::string out; out.resize(md_len * 2);
    for (unsigned i = 0; i < md_len; ++i) {
        out[2*i]     = hex[md[i] >> 4];
        out[2*i + 1] = hex[md[i] & 0xf];
    }
    return out;
}

// Kinds we bother persisting. Debug chatter like "exec_allow" stays
// out so replaying the log is actually useful.
const std::unordered_set<std::string>& interesting_kinds() {
    static const std::unordered_set<std::string> s{
        "user_said", "muse_reply", "muse_error",
        "user_goal", "final_answer", "plan_error",
        "tool_call", "tool_result",
        "remembered", "recall", "recall_result",
    };
    return s;
}

}  // namespace

class Scribe : public Agent {
public:
    Scribe() { open_new_session_(); }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind == "session_new") {
            std::lock_guard<std::mutex> lk(mu_);
            open_new_session_();
            return;
        }
        if (msg.kind == "session_where") {
            nlohmann::json r = {{"path", path_.string()}};
            rt.send({.from=name_, .to=msg.from,
                     .kind="session_path", .payload=r.dump()});
            return;
        }
        if (!interesting_kinds().count(msg.kind)) return;

        nlohmann::json body = {
            {"ts",      std::chrono::duration_cast<std::chrono::milliseconds>(
                            msg.ts.time_since_epoch()).count()},
            {"id",      msg.id},
            {"from",    msg.from},
            {"to",      msg.to},
            {"kind",    msg.kind},
            {"payload", msg.payload},
        };

        std::lock_guard<std::mutex> lk(mu_);
        if (!file_.is_open()) return;
        std::string body_s = body.dump();
        std::string hash   = sha256_hex(prev_hash_ + body_s);
        nlohmann::json entry = {
            {"prev", prev_hash_}, {"hash", hash}, {"body", body},
        };
        file_ << entry.dump() << "\n";
        file_.flush();  // crash-safe by default
        prev_hash_ = hash;
    }

    void stop() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (file_.is_open()) file_.close();
    }

private:
    std::string                 name_ = "scribe";
    std::mutex                  mu_;
    std::filesystem::path       path_;
    std::ofstream               file_;
    std::string                 prev_hash_;  // running tamper-evident chain

    void open_new_session_() {
        auto dir = session_root();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::fprintf(stderr, "[scribe] cannot create %s: %s\n",
                         dir.c_str(), ec.message().c_str());
            return;
        }
        path_ = dir / (timestamp_tag() + ".jsonl");
        file_.open(path_, std::ios::out | std::ios::app);
        if (!file_.is_open()) {
            std::fprintf(stderr, "[scribe] cannot open %s\n", path_.c_str());
            return;
        }
        // Genesis line — seeds the hash chain with the session path + time.
        // Any later tampering with history breaks the chain from here on.
        std::string seed = path_.string() + ":" + timestamp_tag();
        prev_hash_ = sha256_hex("genesis:" + seed);
        nlohmann::json genesis = {
            {"prev", ""}, {"hash", prev_hash_},
            {"body", {{"kind", "session_start"}, {"seed", seed}}},
        };
        file_ << genesis.dump() << "\n"; file_.flush();
        std::fprintf(stderr, "[scribe] session log: %s\n", path_.c_str());
    }
};

std::unique_ptr<Agent> make_scribe() { return std::make_unique<Scribe>(); }

}  // namespace rocm_cpp::agents::specialists
