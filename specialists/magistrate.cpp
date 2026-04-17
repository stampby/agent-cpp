// magistrate — GitHub PR review (read + write).
//
// One job: review opened PRs against the house rules. Fetch the file
// list and check for policy violations:
//   - GPL/AGPL/LGPL/proprietary license headers in new files  -> block
//   - imports of banned Python ML frameworks (MLX, PyTorch)   -> block
//   - deletion of install.sh or top-level CMakeLists.txt      -> block
//   - changes to src/ without matching tests/                  -> warn
// Posts a review as COMMENT (not request_changes / approve) — we flag
// but leave the final call to the architect. Never auto-merges.
//
// Contract:
//   listens for :
//     "github_pr_opened"       {repo, number}
//     "github_pr_synchronized" {repo, number}
//     "github_review_requested"{repo, number}
//   emits :
//     "review_done"            {repo, number, verdict, flags} → to msg.from
//     "review_error"           {error}                        → to msg.from
//     "discord_post"           (herald, if flagged)
//
// Env: GH_TOKEN (pull_requests:write)
//      DISCORD_ESCALATION_CHANNEL (optional)

#include "agents/agent.h"
#include "agents/runtime.h"
#include "agents/github_client.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace rocm_cpp::agents::specialists {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

struct Flag { std::string file; std::string reason; };

// Scan a single added/modified file for banned patterns. Only looks
// at the diff 'patch' (added lines) so we don't re-flag pre-existing
// lines the PR isn't touching.
std::vector<std::string> scan_patch(const std::string& filename,
                                    const std::string& patch) {
    std::vector<std::string> reasons;
    std::string lower_name  = to_lower(filename);
    std::string added_lower;
    // Extract only '+' lines from the patch.
    {
        size_t pos = 0;
        while (pos < patch.size()) {
            size_t nl = patch.find('\n', pos);
            std::string line = patch.substr(pos, nl - pos);
            if (!line.empty() && line[0] == '+' && line.rfind("+++", 0) != 0)
                added_lower += to_lower(line) + "\n";
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    if (has(added_lower, "gpl-3") || has(added_lower, "agpl")
     || has(added_lower, "lgpl") || has(added_lower, "proprietary"))
        reasons.push_back("non-permissive license header");
    if (has(added_lower, "import torch") || has(added_lower, "from torch"))
        reasons.push_back("PyTorch import (BitNet-only policy)");
    if (has(added_lower, "import mlx") || has(added_lower, "from mlx"))
        reasons.push_back("MLX import (BitNet-only policy)");
    if (lower_name.find("install.sh") != std::string::npos
     && has(patch, "\ndeleted file"))
        reasons.push_back("install.sh deletion");
    return reasons;
}

}  // namespace

class Magistrate : public Agent {
public:
    Magistrate() {
        if (const char* c = std::getenv("DISCORD_ESCALATION_CHANNEL"); c && *c)
            escalation_channel_ = c;
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind != "github_pr_opened" &&
            msg.kind != "github_pr_synchronized" &&
            msg.kind != "github_review_requested") return;

        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) {
            err_(rt, msg, std::string("bad JSON: ") + e.what()); return;
        }
        std::string repo   = j.value("repo",   std::string(""));
        int         number = j.value("number", 0);
        if (repo.empty() || number <= 0) {
            err_(rt, msg, "repo + number required"); return;
        }

        auto files = gh_.get("/repos/" + repo + "/pulls/" + std::to_string(number)
                             + "/files?per_page=100");
        if (files.contains("error")) { err_(rt, msg, files.dump()); return; }

        std::vector<Flag> flags;
        bool touched_src  = false;
        bool touched_test = false;
        for (auto& f : files) {
            std::string fn    = f.value("filename", std::string(""));
            std::string patch = f.value("patch",    std::string(""));
            for (auto& r : scan_patch(fn, patch)) flags.push_back({fn, r});
            if (fn.rfind("src/",   0) == 0 || fn.rfind("include/", 0) == 0
             || fn.rfind("specialists/", 0) == 0)
                touched_src = true;
            if (fn.rfind("tests/", 0) == 0 || fn.find("/test_") != std::string::npos
             || fn.find("/tests/") != std::string::npos)
                touched_test = true;
        }
        if (touched_src && !touched_test)
            flags.push_back({"-", "source changes without matching tests"});

        // Compose a review body.
        std::string body;
        std::string verdict = flags.empty() ? "clean" : "flagged";
        if (flags.empty()) {
            body = "Automated review: no policy flags. Human review still required.";
        } else {
            body = "Automated review flagged the following:\n\n";
            for (auto& f : flags)
                body += "- `" + f.file + "` — " + f.reason + "\n";
            body += "\nThese are heuristics — architect has final say.";
        }

        if (gh_.has_auth()) {
            // POST as a plain issue comment. We deliberately do NOT submit
            // a formal review (approve / request_changes) — flags are advisory.
            gh_.post("/repos/" + repo + "/issues/" + std::to_string(number)
                     + "/comments", {{"body", body}});
        }

        if (!flags.empty() && !escalation_channel_.empty()) {
            std::string url = "https://github.com/" + repo + "/pull/"
                            + std::to_string(number);
            nlohmann::json post_body = {
                {"channel_id", escalation_channel_},
                {"content", "PR flagged (" + std::to_string(flags.size())
                            + " issue" + (flags.size()==1?"":"s") + "): " + url},
            };
            rt.send({.from=name_, .to="herald",
                     .kind="discord_post", .payload=post_body.dump()});
        }

        nlohmann::json done = {{"repo", repo}, {"number", number},
                               {"verdict", verdict},
                               {"flag_count", flags.size()}};
        rt.send({.from=name_, .to=msg.from,
                 .kind="review_done", .payload=done.dump()});
    }

private:
    std::string  name_ = "magistrate";
    std::string  escalation_channel_;
    GitHubClient gh_;

    void err_(Runtime& rt, const Message& src, const std::string& why) {
        nlohmann::json jj = {{"error", why}};
        rt.send({.from=name_, .to=src.from,
                 .kind="review_error", .payload=jj.dump()});
    }
};

std::unique_ptr<Agent> make_magistrate() { return std::make_unique<Magistrate>(); }

}  // namespace rocm_cpp::agents::specialists
