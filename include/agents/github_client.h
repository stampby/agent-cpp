// github_client — thin httplib wrapper for the GitHub REST API.
//
// The three GH specialists (quartermaster, magistrate, librarian) all
// do the same dance: read GH_TOKEN, POST/GET against api.github.com with
// Accept: application/vnd.github+json and Bearer auth. Factor it once.
//
// Returns plain nlohmann::json. Errors are reported as a json object
// with "error" and optional "status". Keep it dependency-free beyond
// what the runtime already pulls in.
//
// Deliberately not a full GH SDK — we only need the ~5 endpoints the
// specialists actually touch (issues, comments, reviews, contents, PRs).

#ifndef ROCM_CPP_AGENTS_GITHUB_CLIENT_H
#define ROCM_CPP_AGENTS_GITHUB_CLIENT_H

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <string>

namespace rocm_cpp::agents {

class GitHubClient {
public:
    explicit GitHubClient(std::string token = "")
        : token_(std::move(token)) {
        if (token_.empty())
            if (const char* t = std::getenv("GH_TOKEN"); t && *t) token_ = t;
    }

    bool has_auth() const { return !token_.empty(); }

    // GET /repos/:repo/... -> parsed JSON or {error}
    nlohmann::json get(const std::string& path) const { return req_("GET", path, {}); }
    nlohmann::json post(const std::string& path, const nlohmann::json& body) const {
        return req_("POST", path, body);
    }
    nlohmann::json patch(const std::string& path, const nlohmann::json& body) const {
        return req_("PATCH", path, body);
    }
    nlohmann::json put(const std::string& path, const nlohmann::json& body) const {
        return req_("PUT", path, body);
    }

private:
    std::string token_;

    nlohmann::json req_(const char* method, const std::string& path,
                        const nlohmann::json& body) const
    {
        httplib::Client cli("https://api.github.com");
        cli.set_connection_timeout(10);
        cli.set_read_timeout(30);
        httplib::Headers h{
            {"Accept",     "application/vnd.github+json"},
            {"User-Agent", "agent-cpp/1"},
            {"X-GitHub-Api-Version", "2022-11-28"},
        };
        if (!token_.empty()) h.emplace("Authorization", "Bearer " + token_);

        httplib::Result res;
        std::string payload = body.is_null() ? std::string{} : body.dump();
        if      (!std::strcmp(method, "GET"))   res = cli.Get(path, h);
        else if (!std::strcmp(method, "POST"))  res = cli.Post(path, h, payload, "application/json");
        else if (!std::strcmp(method, "PATCH")) res = cli.Patch(path, h, payload, "application/json");
        else if (!std::strcmp(method, "PUT"))   res = cli.Put(path, h, payload, "application/json");
        else return {{"error", "unsupported method"}};

        if (!res) return {{"error", "GitHub unreachable"}};
        if (res->status < 200 || res->status >= 300) {
            return {{"error", "HTTP " + std::to_string(res->status)},
                    {"status", res->status},
                    {"body", res->body.substr(0, 300)}};
        }
        try { return nlohmann::json::parse(res->body); }
        catch (const std::exception& e) {
            return {{"error", std::string("parse: ") + e.what()}};
        }
    }
};

}  // namespace rocm_cpp::agents
#endif
