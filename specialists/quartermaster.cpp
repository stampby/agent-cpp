// quartermaster — GitHub issue triage (read + write).
//
// One job: when a new issue lands on a repo we own, classify it by
// keyword, apply labels, leave a short acknowledgement, and escalate
// to Discord #chat if it looks critical (security / data-loss). No
// LLM in v1 — deterministic keyword rules. Upgrade path: route to
// planner for borderline cases once planner handles tool_calls.
//
// Contract:
//   listens for :
//     "github_issue_opened"   {repo, number, title, body, author}
//     "github_issue_triage"   {repo, number}   (self-fetches the rest)
//   emits :
//     "triage_done"           {repo, number, labels} → to msg.from
//     "triage_error"          {error}               → to msg.from
//     "discord_post"          (to herald, critical issues only)
//
// Env: GH_TOKEN (issues:write + content:read scopes)
//      DISCORD_ESCALATION_CHANNEL (optional channel_id for herald escalation)

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

// Rule-based classifier. Multiple labels can match. Empty = no opinion.
std::vector<std::string> classify(const std::string& title, const std::string& body) {
    std::string t = to_lower(title + " " + body);
    auto has = [&](const char* kw){ return t.find(kw) != std::string::npos; };
    std::vector<std::string> out;
    if (has("install") || has("build") || has("cmake") || has("compile"))
        out.push_back("installation");
    if (has("crash") || has("segfault") || has("abort") || has("error")
     || has("broken") || has("bug"))
        out.push_back("bug");
    if (has("feature") || has("would be nice") || has("request") || has("proposal"))
        out.push_back("enhancement");
    if (has("document") || has("readme") || has("typo") || has("docs"))
        out.push_back("documentation");
    if (has("rocm") || has("hip") || has("gfx1151") || has("strix"))
        out.push_back("rocm");
    if (has("bitnet") || has("1-bit") || has(".h1b") || has("ternary"))
        out.push_back("bitnet");
    return out;
}

bool is_critical(const std::string& title, const std::string& body) {
    std::string t = to_lower(title + " " + body);
    return t.find("security")   != std::string::npos
        || t.find("data loss")  != std::string::npos
        || t.find("corruption") != std::string::npos
        || t.find("rce")        != std::string::npos
        || t.find("cve")        != std::string::npos;
}

}  // namespace

class Quartermaster : public Agent {
public:
    Quartermaster() {
        if (const char* c = std::getenv("DISCORD_ESCALATION_CHANNEL"); c && *c)
            escalation_channel_ = c;
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind != "github_issue_opened" &&
            msg.kind != "github_issue_triage") return;

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

        std::string title = j.value("title", std::string(""));
        std::string body  = j.value("body",  std::string(""));

        // Self-fetch path if caller only supplied {repo, number}.
        if (msg.kind == "github_issue_triage" || (title.empty() && body.empty())) {
            auto issue = gh_.get("/repos/" + repo + "/issues/" + std::to_string(number));
            if (issue.contains("error")) { err_(rt, msg, issue.dump()); return; }
            title = issue.value("title", std::string(""));
            body  = issue.value("body",  std::string(""));
        }

        auto labels = classify(title, body);

        if (!labels.empty() && gh_.has_auth()) {
            auto r = gh_.post("/repos/" + repo + "/issues/" + std::to_string(number)
                              + "/labels", {{"labels", labels}});
            if (r.contains("error"))
                std::fprintf(stderr, "[quartermaster] label failed: %s\n",
                             r.dump().c_str());

            std::string lbl_list;
            for (auto& l : labels) { if (!lbl_list.empty()) lbl_list += ", "; lbl_list += l; }
            std::string ack = "Thanks for the report — auto-triaged as: " + lbl_list
                            + ". A human will follow up.";
            gh_.post("/repos/" + repo + "/issues/" + std::to_string(number)
                     + "/comments", {{"body", ack}});
        }

        if (is_critical(title, body) && !escalation_channel_.empty()) {
            std::string url = "https://github.com/" + repo + "/issues/"
                            + std::to_string(number);
            nlohmann::json post_body = {
                {"channel_id", escalation_channel_},
                {"content",    "CRITICAL issue opened: " + title + "\n" + url},
            };
            rt.send({.from=name_, .to="herald",
                     .kind="discord_post", .payload=post_body.dump()});
        }

        nlohmann::json done = {{"repo", repo}, {"number", number},
                               {"labels", labels}};
        rt.send({.from=name_, .to=msg.from,
                 .kind="triage_done", .payload=done.dump()});
    }

private:
    std::string    name_ = "quartermaster";
    std::string    escalation_channel_;
    GitHubClient   gh_;

    void err_(Runtime& rt, const Message& src, const std::string& why) {
        nlohmann::json jj = {{"error", why}};
        rt.send({.from=name_, .to=src.from,
                 .kind="triage_error", .payload=jj.dump()});
    }
};

std::unique_ptr<Agent> make_quartermaster() {
    return std::make_unique<Quartermaster>();
}

}  // namespace rocm_cpp::agents::specialists
