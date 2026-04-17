// anvil — build / benchmark runner (local CI).
//
// One job: given a repo + sha, shell out to clone → configure → build
// → bench, capture the numbers, post the JSON result back on the bus
// (and to Discord #benchmarks when configured).
//
// Deliberately naive: uses std::system with a throwaway work dir.
// Keeps the whole operation synchronous inside handle(); Runtime spins
// a dedicated thread per agent so a long build blocks only the anvil
// mailbox, not the rest of the bus. If a build takes longer than
// ANVIL_TIMEOUT_SEC (default 1800s), we record the timeout and move on.
//
// Contract:
//   listens for :
//     "bench_run_request" {repo, clone_url, ref?, bench_cmd?}
//                             repo       = "owner/name" (for labeling)
//                             clone_url  = git URL to clone
//                             ref        = branch/tag/sha (default "main")
//                             bench_cmd  = shell override (default bench.sh)
//   emits :
//     "bench_result"  {repo, ref, sha, seconds, output, exit_code} → to msg.from
//     "bench_error"   {error}                                       → to msg.from
//     "discord_post"  (herald, if DISCORD_BENCH_CHANNEL is set)
//
// Env:
//   ANVIL_TIMEOUT_SEC          build+bench timeout, default 1800
//   ANVIL_WORK_DIR             override work root ($XDG_CACHE_HOME/agent-cpp/anvil)
//   DISCORD_BENCH_CHANNEL      optional channel_id for herald announcements

#include "agents/agent.h"
#include "agents/runtime.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/wait.h>

namespace rocm_cpp::agents::specialists {

namespace {

std::filesystem::path work_root() {
    const char* o = std::getenv("ANVIL_WORK_DIR");
    if (o && *o) return o;
    const char* x = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path base;
    if (x && *x) base = x;
    else {
        const char* h = std::getenv("HOME");
        base = h ? std::filesystem::path(h) / ".cache" : std::filesystem::path("/tmp");
    }
    return base / "agent-cpp" / "anvil";
}

std::string shell_safe(const std::string& in) {
    // Minimal quoting for values we feed into a shell command line. We
    // only accept strings the architect trusts already, but belt + braces.
    std::string out = "'";
    for (char c : in) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
}

// Run a command, stream both stdout and stderr into a captured string.
// Returns {exit_code, combined_output}.
struct RunResult { int exit = -1; std::string out; };
RunResult run_capture(const std::string& cmd, int timeout_sec) {
    RunResult r;
    std::string wrapped = "timeout " + std::to_string(timeout_sec)
                        + " bash -c " + shell_safe(cmd) + " 2>&1";
    FILE* p = popen(wrapped.c_str(), "r");
    if (!p) { r.exit = -2; r.out = "popen failed"; return r; }
    std::array<char, 4096> buf;
    while (std::fgets(buf.data(), buf.size(), p)) r.out += buf.data();
    int rc = pclose(p);
    r.exit = WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
    return r;
}

}  // namespace

class Anvil : public Agent {
public:
    Anvil() {
        if (const char* t = std::getenv("ANVIL_TIMEOUT_SEC"); t && *t)
            timeout_sec_ = std::max(60, std::atoi(t));
        if (const char* c = std::getenv("DISCORD_BENCH_CHANNEL"); c && *c)
            bench_channel_ = c;
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind != "bench_run_request") return;

        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) {
            err_(rt, msg, std::string("bad JSON: ") + e.what()); return;
        }
        std::string repo      = j.value("repo",      std::string(""));
        std::string clone_url = j.value("clone_url", std::string(""));
        std::string ref       = j.value("ref",       std::string("main"));
        std::string bench_cmd = j.value("bench_cmd", std::string(""));
        if (repo.empty() || clone_url.empty()) {
            err_(rt, msg, "repo + clone_url required"); return;
        }

        std::error_code ec;
        std::filesystem::create_directories(work_root(), ec);
        if (ec) { err_(rt, msg, "work dir: " + ec.message()); return; }

        std::time_t t = std::time(nullptr);
        char stamp[32]; std::strftime(stamp, sizeof(stamp),
                                      "%Y%m%d-%H%M%S", std::localtime(&t));
        auto work = work_root() / (std::string(stamp) + "-"
                                  + (repo.find('/')==std::string::npos
                                     ? repo : repo.substr(repo.find('/')+1)));

        auto t0 = std::chrono::steady_clock::now();

        // Clone + checkout + build + bench, fail-fast via &&.
        std::string cmd =
            "git clone --depth 100 " + shell_safe(clone_url) + " "
              + shell_safe(work.string()) + " && "
            "cd "        + shell_safe(work.string()) + " && "
            "git checkout " + shell_safe(ref) + " && ";
        if (bench_cmd.empty()) {
            // Default: CMake build + run ./bench.sh if present, else ctest.
            cmd +=
                "cmake -B build -DCMAKE_BUILD_TYPE=Release && "
                "cmake --build build -j$(nproc) && "
                "( [ -x ./bench.sh ] && ./bench.sh || "
                "  ( [ -f build/CTestTestfile.cmake ] && ctest --test-dir build --output-on-failure ) )";
        } else {
            cmd += bench_cmd;
        }

        auto r = run_capture(cmd, timeout_sec_);

        auto t1 = std::chrono::steady_clock::now();
        double seconds = std::chrono::duration<double>(t1 - t0).count();

        // Best-effort sha extraction from the clone (ref may have been a branch).
        std::string sha;
        {
            auto s = run_capture("cd " + shell_safe(work.string())
                               + " && git rev-parse HEAD", 10);
            if (s.exit == 0) {
                sha = s.out;
                if (!sha.empty() && sha.back() == '\n') sha.pop_back();
            }
        }

        nlohmann::json result = {
            {"repo",      repo},
            {"ref",       ref},
            {"sha",       sha},
            {"seconds",   seconds},
            {"exit_code", r.exit},
            {"output",    r.out.size() > 4000
                          ? r.out.substr(0, 2000) + "\n...\n"
                            + r.out.substr(r.out.size() - 2000)
                          : r.out},
            {"work_dir",  work.string()},
        };

        rt.send({.from=name_, .to=msg.from,
                 .kind="bench_result", .payload=result.dump()});

        if (!bench_channel_.empty()) {
            std::string headline = "[" + repo + "@" + ref + "] "
                                 + (r.exit == 0 ? "bench OK" : "bench FAIL")
                                 + " in " + std::to_string((int)seconds) + "s";
            nlohmann::json post_body = {
                {"channel_id", bench_channel_},
                {"content",    headline},
            };
            rt.send({.from=name_, .to="herald",
                     .kind="discord_post", .payload=post_body.dump()});
        }
    }

private:
    std::string name_ = "anvil";
    int         timeout_sec_ = 1800;
    std::string bench_channel_;

    void err_(Runtime& rt, const Message& src, const std::string& why) {
        nlohmann::json jj = {{"error", why}};
        rt.send({.from=name_, .to=src.from,
                 .kind="bench_error", .payload=jj.dump()});
    }
};

std::unique_ptr<Agent> make_anvil() { return std::make_unique<Anvil>(); }

}  // namespace rocm_cpp::agents::specialists
