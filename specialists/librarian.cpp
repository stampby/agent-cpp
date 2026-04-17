// librarian — docs + changelog maintainer (read + write).
//
// One job: keep docs in sync with shipped code. Two scenarios:
//
//   1. "github_release" fires. Librarian appends a dated entry to
//      CHANGELOG.md on main and announces the release on Discord.
//
//   2. "github_push_main" fires. Librarian checks whether the push
//      touched source without touching docs/ — if so, opens an issue
//      reminding the author to document the change. Non-blocking.
//
// Contract:
//   listens for :
//     "github_release"     {repo, tag, name, body}
//     "github_push_main"   {repo, sha, files}
//     "docs_refresh_request"
//   emits :
//     "librarian_done"     {repo, action} → to msg.from
//     "librarian_error"    {error}        → to msg.from
//     "discord_post"       (announcements on release)
//
// Env: GH_TOKEN (contents:write for CHANGELOG append)
//      DISCORD_ANNOUNCEMENTS_CHANNEL (optional, for release announcements)

#include "agents/agent.h"
#include "agents/runtime.h"
#include "agents/github_client.h"

#include <cstdlib>
#include <ctime>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace rocm_cpp::agents::specialists {

namespace {

// RFC 4648 base64. Only needed for the GH contents endpoint which
// requires base64-encoded payloads on PUT.
std::string base64_encode(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c; valb += 8;
        while (valb >= 0) { out += tbl[(val >> valb) & 0x3F]; valb -= 6; }
    }
    if (valb > -6) out += tbl[((val << 8) >> (valb + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

std::string base64_decode(const std::string& in) {
    static int tbl[256]; static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) tbl[i] = -1;
        static const char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i)
            tbl[static_cast<unsigned char>(alphabet[i])] = i;
        init = true;
    }
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (tbl[c] < 0) continue;
        val = (val << 6) + tbl[c]; valb += 6;
        if (valb >= 0) { out += char((val >> valb) & 0xFF); valb -= 8; }
    }
    return out;
}

std::string today_ymd() {
    std::time_t t = std::time(nullptr);
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&t));
    return buf;
}

}  // namespace

class Librarian : public Agent {
public:
    Librarian() {
        if (const char* c = std::getenv("DISCORD_ANNOUNCEMENTS_CHANNEL"); c && *c)
            announcements_channel_ = c;
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind == "github_release") return on_release_(msg, rt);
        if (msg.kind == "github_push_main") return on_push_(msg, rt);
        // docs_refresh_request is reserved; no v1 behaviour yet.
    }

private:
    std::string  name_ = "librarian";
    std::string  announcements_channel_;
    GitHubClient gh_;

    void err_(Runtime& rt, const Message& src, const std::string& why) {
        nlohmann::json jj = {{"error", why}};
        rt.send({.from=name_, .to=src.from,
                 .kind="librarian_error", .payload=jj.dump()});
    }

    void on_release_(const Message& msg, Runtime& rt) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) {
            err_(rt, msg, std::string("bad JSON: ") + e.what()); return;
        }
        std::string repo = j.value("repo", std::string(""));
        std::string tag  = j.value("tag",  std::string(""));
        std::string rname= j.value("name", tag);
        std::string body = j.value("body", std::string(""));
        if (repo.empty() || tag.empty()) {
            err_(rt, msg, "repo + tag required"); return;
        }

        // Fetch existing CHANGELOG.md (may not exist).
        auto cur = gh_.get("/repos/" + repo + "/contents/CHANGELOG.md");
        std::string existing, sha;
        if (!cur.contains("error")) {
            std::string enc = cur.value("content", std::string(""));
            // GitHub returns base64 with embedded newlines — strip them first.
            std::string clean; for (char c : enc) if (c!='\n' && c!='\r') clean += c;
            existing = base64_decode(clean);
            sha      = cur.value("sha", std::string(""));
        }

        std::string entry = "## " + rname + " — " + today_ymd() + "\n\n"
                          + (body.empty() ? "Release " + tag + ".\n" : body + "\n") + "\n";
        std::string updated;
        if (existing.rfind("# Changelog", 0) == 0) {
            // Insert right after the header line.
            auto nl = existing.find('\n');
            updated = existing.substr(0, nl+1) + "\n" + entry + existing.substr(nl+1);
        } else {
            updated = "# Changelog\n\n" + entry + existing;
        }

        if (gh_.has_auth()) {
            nlohmann::json put_body = {
                {"message", "docs: append " + tag + " to CHANGELOG (librarian)"},
                {"content", base64_encode(updated)},
            };
            if (!sha.empty()) put_body["sha"] = sha;
            auto r = gh_.put("/repos/" + repo + "/contents/CHANGELOG.md", put_body);
            if (r.contains("error"))
                std::fprintf(stderr, "[librarian] CHANGELOG write failed: %s\n",
                             r.dump().c_str());
        }

        if (!announcements_channel_.empty()) {
            std::string url = "https://github.com/" + repo + "/releases/tag/" + tag;
            nlohmann::json pb = {
                {"channel_id", announcements_channel_},
                {"content",    "Released: " + rname + "\n" + url},
            };
            rt.send({.from=name_, .to="herald",
                     .kind="discord_post", .payload=pb.dump()});
        }

        nlohmann::json done = {{"repo", repo}, {"action", "changelog_appended"}};
        rt.send({.from=name_, .to=msg.from,
                 .kind="librarian_done", .payload=done.dump()});
    }

    void on_push_(const Message& msg, Runtime& rt) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) {
            err_(rt, msg, std::string("bad JSON: ") + e.what()); return;
        }
        std::string repo = j.value("repo", std::string(""));
        auto files = j.value("files", nlohmann::json::array());
        if (repo.empty() || files.empty()) {
            err_(rt, msg, "repo + files required"); return;
        }

        bool touched_src = false, touched_docs = false;
        for (auto& f : files) {
            std::string fn = f.get<std::string>();
            if (fn.rfind("src/", 0) == 0 || fn.rfind("include/", 0) == 0
             || fn.rfind("specialists/", 0) == 0)
                touched_src = true;
            if (fn.rfind("docs/", 0) == 0 || fn == "README.md")
                touched_docs = true;
        }

        std::string action = "no_action";
        if (touched_src && !touched_docs && gh_.has_auth()) {
            nlohmann::json issue = {
                {"title", "docs: src changed without doc update"},
                {"body",  "Librarian noticed source changes without any docs/"
                          " or README.md update on main. Please document the "
                          "behaviour change or close this as no-op."},
                {"labels", nlohmann::json::array({"documentation"})},
            };
            gh_.post("/repos/" + repo + "/issues", issue);
            action = "doc_reminder_opened";
        }

        nlohmann::json done = {{"repo", repo}, {"action", action}};
        rt.send({.from=name_, .to=msg.from,
                 .kind="librarian_done", .payload=done.dump()});
    }
};

std::unique_ptr<Agent> make_librarian() { return std::make_unique<Librarian>(); }

}  // namespace rocm_cpp::agents::specialists
