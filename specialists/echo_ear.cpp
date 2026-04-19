// echo_ear — speech-to-text bridge (whisper.cpp whisper-server backend).
//
// One job: turn audio bytes into text. Caller hands us a WAV/OGG/FLAC
// payload (base64 on the bus) or a path on disk plus an optional
// language hint; we POST multipart to the local whisper-server.service
// and emit user_said with the transcript.
//
// whisper-server contract (verified live 2026-04-19 on :8082):
//   POST /inference   multipart/form-data
//     file              audio bytes (WAV/OGG/FLAC)
//     response_format   "json" | "text" | "verbose_json"
//     temperature       (optional)
//     language          (optional ISO-639-1, "auto" for detect)
//   returns 200 JSON: {"text": "..."} when response_format=json
//
// Contract:
//   listens for :
//     "mic_wav_b64"   {audio_b64, format?: "wav"|"ogg"|"flac", language?}
//     "mic_wav_path"  {path, format?, language?}
//     "mic_eou"                                     (reserved for streaming v2)
//   emits :
//     "user_said"     <transcript>                 -> to msg.from
//     "stt_error"     {error}                      -> to msg.from
//
// Env:
//   WHISPER_URL   override for whisper-server (default http://127.0.0.1:8082)

#include "agents/agent.h"
#include "agents/runtime.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include <httplib.h>

namespace rocm_cpp::agents::specialists {

namespace {

std::string b64_decode(const std::string& in) {
    static int tbl[256]; static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) tbl[i] = -1;
        static const char A[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) tbl[static_cast<unsigned char>(A[i])] = i;
        init = true;
    }
    std::string out; int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (tbl[c] < 0) continue;
        val = (val << 6) + tbl[c]; valb += 6;
        if (valb >= 0) { out += char((val >> valb) & 0xFF); valb -= 8; }
    }
    return out;
}

void split_url(const std::string& url, std::string& host, int& port, bool& https) {
    std::string u = url; https = false;
    if (u.rfind("https://", 0) == 0) { u.erase(0, 8); https = true;  }
    else if (u.rfind("http://",  0) == 0) { u.erase(0, 7); https = false; }
    auto slash = u.find('/'); if (slash != std::string::npos) u = u.substr(0, slash);
    auto colon = u.find(':');
    if (colon == std::string::npos) { host = u; port = https ? 443 : 80; }
    else { host = u.substr(0, colon); port = std::atoi(u.c_str() + colon + 1); }
}

}  // namespace

class EchoEar : public Agent {
public:
    EchoEar() {
        const char* u = std::getenv("WHISPER_URL");
        url_ = (u && *u) ? u : "http://127.0.0.1:8082";
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind != "mic_wav_b64" && msg.kind != "mic_wav_path") return;

        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) {
            err_(rt, msg, std::string("bad JSON: ") + e.what()); return;
        }

        std::string audio;
        std::string format = j.value("format", std::string("wav"));
        if (msg.kind == "mic_wav_b64") {
            std::string enc = j.value("audio_b64", std::string(""));
            if (enc.empty()) { err_(rt, msg, "audio_b64 required"); return; }
            audio = b64_decode(enc);
        } else {
            std::string path = j.value("path", std::string(""));
            if (path.empty()) { err_(rt, msg, "path required"); return; }
            std::ifstream f(path, std::ios::binary);
            if (!f) { err_(rt, msg, "cannot read " + path); return; }
            std::ostringstream ss; ss << f.rdbuf(); audio = ss.str();
            // Best-effort format sniff from extension if caller didn't set it.
            if (!j.contains("format")) {
                auto dot = path.rfind('.');
                if (dot != std::string::npos) {
                    std::string ext = path.substr(dot + 1);
                    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
                    if (ext == "wav" || ext == "ogg" || ext == "flac") format = ext;
                }
            }
        }
        if (audio.empty()) { err_(rt, msg, "empty audio"); return; }

        std::string language = j.value("language", std::string("en"));

        // Map format -> mime + filename. whisper-server sniffs by header,
        // but sending a correct Content-Type keeps logs clean.
        std::string mime = "audio/wav";
        std::string fname = "audio.wav";
        if      (format == "ogg")  { mime = "audio/ogg";  fname = "audio.ogg";  }
        else if (format == "flac") { mime = "audio/flac"; fname = "audio.flac"; }

        std::string host; int port; bool https;
        split_url(url_, host, port, https);
        httplib::Client cli(host, port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(60);

        // whisper-server wants the audio under field name "file" and expects
        // a "response_format" field to pick the wire format.
        httplib::MultipartFormDataItems items = {
            {"file",            audio,    fname, mime},
            {"response_format", "json",   "",    ""},
            {"language",        language, "",    ""},
            {"temperature",     "0.0",    "",    ""},
        };
        auto res = cli.Post("/inference", items);
        if (!res) { err_(rt, msg, "whisper-server unreachable at " + url_); return; }
        if (res->status < 200 || res->status >= 300) {
            err_(rt, msg, "whisper HTTP " + std::to_string(res->status)
                       + ": " + res->body.substr(0, 200));
            return;
        }

        // whisper-server returns {"text": "..."} in JSON mode. Some builds
        // also include segments[] with per-chunk timings and avg_logprob;
        // if present, surface the mean probability as a confidence proxy.
        std::string text;
        double confidence = -1.0;
        try {
            auto parsed = nlohmann::json::parse(res->body);
            text = parsed.value("text", std::string(""));
            if (parsed.contains("segments") && parsed["segments"].is_array()
                && !parsed["segments"].empty()) {
                double sum = 0.0; int n = 0;
                for (const auto& seg : parsed["segments"]) {
                    if (seg.contains("avg_logprob") && seg["avg_logprob"].is_number()) {
                        sum += std::exp(seg["avg_logprob"].get<double>());
                        ++n;
                    }
                }
                if (n > 0) confidence = sum / n;
            }
        } catch (const std::exception& e) {
            err_(rt, msg, std::string("parse: ") + e.what()); return;
        }
        // Trim a leading space that whisper likes to emit.
        while (!text.empty() && (text.front() == ' ' || text.front() == '\n'))
            text.erase(text.begin());

        // Keep the legacy "user_said" wire contract for existing listeners
        // (muse consumes raw transcript text), and also emit a structured
        // event so voice-UI specialists can react to confidence.
        rt.send({.from=name_, .to=msg.from,
                 .kind="user_said", .payload=text});
        nlohmann::json out = {{"text", text}};
        if (confidence >= 0.0) out["confidence"] = confidence;
        rt.send({.from=name_, .to=msg.from,
                 .kind="stt_result", .payload=out.dump()});
    }

private:
    std::string name_ = "echo_ear";
    std::string url_;

    static void err_(Runtime& rt, const Message& src, const std::string& why) {
        nlohmann::json jj = {{"error", why}};
        rt.send({.from="echo_ear", .to=src.from,
                 .kind="stt_error", .payload=jj.dump()});
    }
};

std::unique_ptr<Agent> make_echo_ear() { return std::make_unique<EchoEar>(); }

}  // namespace rocm_cpp::agents::specialists
