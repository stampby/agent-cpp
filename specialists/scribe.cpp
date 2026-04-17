// scribe — session persistence specialist.
//
// One job: every interesting message that flows on the bus gets
// appended to a JSONL log file. Makes conversations survive restarts,
// gives you a trail for debugging, lets cartograph ingest history
// later if needed.
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

        nlohmann::json entry = {
            {"ts",      std::chrono::duration_cast<std::chrono::milliseconds>(
                            msg.ts.time_since_epoch()).count()},
            {"id",      msg.id},
            {"from",    msg.from},
            {"to",      msg.to},
            {"kind",    msg.kind},
            {"payload", msg.payload},
        };

        std::lock_guard<std::mutex> lk(mu_);
        if (file_.is_open()) {
            file_ << entry.dump() << "\n";
            file_.flush();  // crash-safe by default
        }
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
        std::fprintf(stderr, "[scribe] session log: %s\n", path_.c_str());
    }
};

std::unique_ptr<Agent> make_scribe() { return std::make_unique<Scribe>(); }

}  // namespace rocm_cpp::agents::specialists
