// echo_mouth — text-to-speech bridge (halo-kokoro backend).
//
// One job: take model output text and speak it via the local
// halo-kokoro server (Bun shim over kokoro_tts CLI). v1 writes the WAV
// to a temp path and emits "tts_done" with that path plus a base64 copy
// for bus consumers (visualizer, Discord upload). Streaming is v2 —
// the current Kokoro server returns a complete WAV.
//
// halo-kokoro contract (server.ts on :8083):
//   POST /tts            {"text": "...", "voice": "af_sky", "speed": 1.0}
//                        -> 200 audio/wav (raw body)
//   GET  /healthz        -> "ok"
//   GET  /voices         -> {"voices": [...], "default": "af_sky"}
//
// Contract:
//   listens for :
//     "tts_say"    {text, voice?, speed?}   (explicit — most common)
//     "muse_reply" <text>                    (implicit — if TTS_AUTO_MUSE=1)
//     "tts_stop"                             (reserved; no streaming yet)
//   emits :
//     "tts_done"   {path, audio_b64, bytes, voice} -> to msg.from
//     "tts_error"  {error}                         -> to msg.from
//
// Env:
//   KOKORO_URL        override (default http://127.0.0.1:8083)
//   KOKORO_VOICE      default voice (default "af_sky")
//   TTS_AUTO_MUSE     if "1", speak every muse_reply automatically.

#include "agents/agent.h"
#include "agents/runtime.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include <httplib.h>

namespace rocm_cpp::agents::specialists {

namespace {

std::string b64_encode(const std::string& in) {
    static const char A[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c; valb += 8;
        while (valb >= 0) { out += A[(val >> valb) & 0x3F]; valb -= 6; }
    }
    if (valb > -6) out += A[((val << 8) >> (valb + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

void split_url(const std::string& url, std::string& host, int& port, bool& https) {
    std::string u = url; https = false;
    if (u.rfind("http://",  0) == 0) { u.erase(0, 7); https = false; }
    if (u.rfind("https://", 0) == 0) { u.erase(0, 8); https = true;  }
    auto slash = u.find('/'); if (slash != std::string::npos) u = u.substr(0, slash);
    auto colon = u.find(':');
    if (colon == std::string::npos) { host = u; port = https ? 443 : 80; }
    else { host = u.substr(0, colon); port = std::atoi(u.c_str() + colon + 1); }
}

std::filesystem::path cache_dir() {
    const char* x = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path base;
    if (x && *x) base = x;
    else {
        const char* h = std::getenv("HOME");
        base = h ? std::filesystem::path(h) / ".cache" : std::filesystem::path("/tmp");
    }
    return base / "agent-cpp" / "tts";
}

}  // namespace

class EchoMouth : public Agent {
public:
    EchoMouth() {
        const char* u = std::getenv("KOKORO_URL");
        url_   = (u && *u) ? u : "http://127.0.0.1:8083";
        const char* v = std::getenv("KOKORO_VOICE");
        voice_ = (v && *v) ? v : "af_sky";
        const char* a = std::getenv("TTS_AUTO_MUSE");
        auto_muse_ = (a && *a && std::string(a) != "0");
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        std::string text, voice = voice_;
        double speed = 1.0;
        bool has_speed = false;

        if (msg.kind == "tts_say") {
            nlohmann::json j;
            try { j = nlohmann::json::parse(msg.payload); }
            catch (const std::exception& e) {
                err_(rt, msg, std::string("bad JSON: ") + e.what()); return;
            }
            text  = j.value("text",  std::string(""));
            voice = j.value("voice", voice_);
            if (j.contains("speed") && j["speed"].is_number()) {
                speed = j["speed"].get<double>();
                has_speed = true;
            }
        } else if (msg.kind == "muse_reply") {
            if (!auto_muse_) return;    // opt-in to prevent noisy dev
            text = msg.payload;
        } else {
            return;
        }
        if (text.empty()) { err_(rt, msg, "empty text"); return; }

        std::string host; int port; bool https;
        split_url(url_, host, port, https);
        httplib::Client cli(host, port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(120);
        httplib::Headers headers{{"Content-Type", "application/json"}};
        nlohmann::json body = {{"text", text}, {"voice", voice}};
        if (has_speed) body["speed"] = speed;

        auto res = cli.Post("/tts", headers, body.dump(), "application/json");
        if (!res) { err_(rt, msg, "kokoro unreachable at " + url_); return; }
        if (res->status < 200 || res->status >= 300) {
            err_(rt, msg, "kokoro HTTP " + std::to_string(res->status)
                       + ": " + res->body.substr(0, 200));
            return;
        }
        const std::string& wav = res->body;

        // Write to cache for downstream players / Discord uploads.
        std::error_code ec;
        std::filesystem::create_directories(cache_dir(), ec);
        std::string stamp; stamp.resize(32);
        std::time_t t = std::time(nullptr);
        stamp.resize(std::strftime(stamp.data(), stamp.size(),
                                   "%Y%m%d-%H%M%S", std::localtime(&t)));
        auto path = cache_dir() / (stamp + "-" + voice + ".wav");
        { std::ofstream f(path, std::ios::binary); f.write(wav.data(), wav.size()); }

        nlohmann::json out = {
            {"path",       path.string()},
            {"bytes",      wav.size()},
            {"voice",      voice},
            {"audio_b64",  b64_encode(wav)},
        };
        rt.send({.from=name_, .to=msg.from,
                 .kind="tts_done", .payload=out.dump()});
    }

private:
    std::string name_ = "echo_mouth";
    std::string url_;
    std::string voice_;
    bool        auto_muse_ = false;

    static void err_(Runtime& rt, const Message& src, const std::string& why) {
        nlohmann::json jj = {{"error", why}};
        rt.send({.from="echo_mouth", .to=src.from,
                 .kind="tts_error", .payload=jj.dump()});
    }
};

std::unique_ptr<Agent> make_echo_mouth() { return std::make_unique<EchoMouth>(); }

}  // namespace rocm_cpp::agents::specialists
