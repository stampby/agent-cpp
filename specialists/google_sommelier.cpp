// google_sommelier — Google Workspace API bridge (read-focused).
//
// Calls Drive/Gmail/Calendar REST APIs. Bearer token sourced in this order:
//   1. GOOGLE_ACCESS_TOKEN env var (set by a refresh helper or manually)
//   2. `gcloud auth application-default print-access-token` (one-time setup:
//       gcloud auth application-default login)
//
// Deliberately doesn't implement the full OAuth2 dance — gcloud owns credential
// storage + refresh, we just consume tokens. Users who don't want gcloud can
// export GOOGLE_ACCESS_TOKEN themselves from any refresh mechanism they like.
//
// Contract:
//   listens for :
//     "gdrive_search"       {query, limit?, mime_type?}
//     "gdrive_read"         {file_id}
//     "gmail_search"        {query, limit?}
//     "gcal_upcoming"       {calendar_id?, hours_ahead?}
//   emits :
//     "<kind>_result"       {ok, data/error}  → to msg.from
//     "<kind>_error"        {error}           → to msg.from
//
// Env:
//   GOOGLE_ACCESS_TOKEN   (preferred — just paste/refresh a bearer token)
//   (fallback: runs `gcloud auth application-default print-access-token`)

#include "agents/agent.h"
#include "agents/runtime.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include <httplib.h>

namespace rocm_cpp::agents::specialists {

namespace {

std::string run_capture(const char* cmd) {
    std::array<char, 4096> buf{};
    std::string out;
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) return {};
    while (fgets(buf.data(), buf.size(), pipe.get())) out.append(buf.data());
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

// URL-encode a query string value. Keeps this file dependency-free beyond httplib.
std::string urlenc(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

}  // namespace

class GoogleSommelier : public Agent {
public:
    GoogleSommelier() = default;

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind == "gdrive_search")   return on_drive_search_(msg, rt);
        if (msg.kind == "gdrive_read")     return on_drive_read_(msg, rt);
        if (msg.kind == "gmail_search")    return on_gmail_search_(msg, rt);
        if (msg.kind == "gcal_upcoming")   return on_calendar_upcoming_(msg, rt);
    }

private:
    std::string name_ = "google_sommelier";

    std::string token_() {
        if (const char* t = std::getenv("GOOGLE_ACCESS_TOKEN"); t && *t) return t;
        return run_capture("gcloud auth application-default print-access-token 2>/dev/null");
    }

    void reply_(Runtime& rt, const Message& src, const std::string& kind,
                const nlohmann::json& body) {
        rt.send({.from=name_, .to=src.from, .kind=kind, .payload=body.dump()});
    }

    void err_(Runtime& rt, const Message& src, const std::string& kind,
              const std::string& why) {
        reply_(rt, src, kind + "_error", nlohmann::json{{"error", why}});
    }

    // One-shot HTTPS GET with bearer auth. Returns parsed JSON or {error}.
    nlohmann::json https_get_(const std::string& host, const std::string& path,
                               const std::string& bearer) {
        httplib::Client cli(host.c_str());
        cli.set_connection_timeout(10);
        cli.set_read_timeout(30);
        httplib::Headers h{
            {"Authorization", "Bearer " + bearer},
            {"Accept", "application/json"},
        };
        auto res = cli.Get(path, h);
        if (!res) return nlohmann::json{{"error", "unreachable: " + host}};
        if (res->status < 200 || res->status >= 300) {
            return nlohmann::json{{"error", "HTTP " + std::to_string(res->status)},
                                   {"body", res->body.substr(0, 300)}};
        }
        try { return nlohmann::json::parse(res->body); }
        catch (const std::exception& e) {
            return nlohmann::json{{"error", std::string("parse: ") + e.what()}};
        }
    }

    void on_drive_search_(const Message& msg, Runtime& rt) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) { err_(rt, msg, "gdrive_search", std::string("bad JSON: ") + e.what()); return; }
        std::string query = j.value("query", std::string(""));
        if (query.empty()) { err_(rt, msg, "gdrive_search", "query required"); return; }
        int limit = std::max(1, std::min(j.value("limit", 20), 100));
        std::string mime = j.value("mime_type", std::string(""));

        std::string tok = token_();
        if (tok.empty()) { err_(rt, msg, "gdrive_search", "no access token (set GOOGLE_ACCESS_TOKEN or gcloud auth)"); return; }

        std::string q = "name contains '" + query + "' or fullText contains '" + query + "'";
        if (!mime.empty()) q += " and mimeType = '" + mime + "'";
        std::string path = "/drive/v3/files?pageSize=" + std::to_string(limit)
                         + "&q=" + urlenc(q)
                         + "&fields=files(id,name,mimeType,modifiedTime,owners,webViewLink)";
        auto resp = https_get_("https://www.googleapis.com", path, tok);
        reply_(rt, msg, "gdrive_search_result",
               {{"ok", !resp.contains("error")}, {"data", resp}});
    }

    void on_drive_read_(const Message& msg, Runtime& rt) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) { err_(rt, msg, "gdrive_read", std::string("bad JSON: ") + e.what()); return; }
        std::string fid = j.value("file_id", std::string(""));
        if (fid.empty()) { err_(rt, msg, "gdrive_read", "file_id required"); return; }
        std::string tok = token_();
        if (tok.empty()) { err_(rt, msg, "gdrive_read", "no access token"); return; }

        // First, fetch metadata to pick an export format for Google Docs/Sheets.
        auto meta = https_get_("https://www.googleapis.com",
                               "/drive/v3/files/" + fid + "?fields=mimeType,name", tok);
        if (meta.contains("error")) {
            reply_(rt, msg, "gdrive_read_result", {{"ok", false}, {"data", meta}});
            return;
        }
        std::string mime = meta.value("mimeType", std::string(""));
        std::string path;
        if (mime == "application/vnd.google-apps.document")
            path = "/drive/v3/files/" + fid + "/export?mimeType=text/markdown";
        else if (mime == "application/vnd.google-apps.spreadsheet")
            path = "/drive/v3/files/" + fid + "/export?mimeType=text/csv";
        else if (mime == "application/vnd.google-apps.presentation")
            path = "/drive/v3/files/" + fid + "/export?mimeType=text/plain";
        else
            path = "/drive/v3/files/" + fid + "?alt=media";

        httplib::Client cli("https://www.googleapis.com");
        cli.set_connection_timeout(10);
        cli.set_read_timeout(60);
        httplib::Headers h{{"Authorization", "Bearer " + tok}};
        auto res = cli.Get(path, h);
        if (!res || res->status < 200 || res->status >= 300) {
            reply_(rt, msg, "gdrive_read_result",
                   {{"ok", false}, {"error", res ? ("HTTP " + std::to_string(res->status)) : "unreachable"}});
            return;
        }
        reply_(rt, msg, "gdrive_read_result",
               {{"ok", true}, {"name", meta.value("name", "")}, {"mimeType", mime},
                {"content", res->body.substr(0, 500'000)}});  // cap at ~500 KB
    }

    void on_gmail_search_(const Message& msg, Runtime& rt) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) { err_(rt, msg, "gmail_search", std::string("bad JSON: ") + e.what()); return; }
        std::string query = j.value("query", std::string(""));
        if (query.empty()) { err_(rt, msg, "gmail_search", "query required"); return; }
        int limit = std::max(1, std::min(j.value("limit", 20), 100));
        std::string tok = token_();
        if (tok.empty()) { err_(rt, msg, "gmail_search", "no access token"); return; }

        std::string path = "/gmail/v1/users/me/messages?maxResults=" + std::to_string(limit)
                         + "&q=" + urlenc(query);
        auto resp = https_get_("https://gmail.googleapis.com", path, tok);
        reply_(rt, msg, "gmail_search_result",
               {{"ok", !resp.contains("error")}, {"data", resp}});
    }

    void on_calendar_upcoming_(const Message& msg, Runtime& rt) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) { err_(rt, msg, "gcal_upcoming", std::string("bad JSON: ") + e.what()); return; }
        std::string cal = j.value("calendar_id", std::string("primary"));
        int hours = std::max(1, std::min(j.value("hours_ahead", 24), 24 * 30));
        std::string tok = token_();
        if (tok.empty()) { err_(rt, msg, "gcal_upcoming", "no access token"); return; }

        // Google Calendar wants ISO-8601 timeMin/timeMax.
        std::time_t now = std::time(nullptr);
        std::time_t until = now + static_cast<std::time_t>(hours) * 3600;
        char t0[32]; std::strftime(t0, sizeof(t0), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
        char t1[32]; std::strftime(t1, sizeof(t1), "%Y-%m-%dT%H:%M:%SZ", gmtime(&until));
        std::string path = "/calendar/v3/calendars/" + urlenc(cal)
                         + "/events?singleEvents=true&orderBy=startTime"
                         + "&timeMin=" + t0 + "&timeMax=" + t1;
        auto resp = https_get_("https://www.googleapis.com", path, tok);
        reply_(rt, msg, "gcal_upcoming_result",
               {{"ok", !resp.contains("error")}, {"data", resp}});
    }
};

std::unique_ptr<Agent> make_google_sommelier() {
    return std::make_unique<GoogleSommelier>();
}

}  // namespace rocm_cpp::agents::specialists
