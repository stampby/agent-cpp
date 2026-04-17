// echo_ear — speech-to-text bridge (faster-whisper backend).
//
// One job: turn audio bytes into text. Caller hands us a WAV payload
// (base64 on the bus) or a path to a WAV on disk plus an optional
// language hint; we POST multipart to the local whisper-server.service
// and emit user_said with the transcript.
//
// Contract:
//   listens for :
//     "mic_wav_b64"   {audio_b64, language?}       (in-memory path)
//     "mic_wav_path"  {path, language?}            (file path on disk)
//     "mic_eou"                                     (reserved for streaming v2)
//   emits :
//     "user_said"     <transcript>                 → to msg.from
//     "stt_error"     {error}                      → to msg.from
//
// Env:
//   WHISPER_URL   override for whisper-server (default http://127.0.0.1:8082)

#include "agents/agent.h"
#include "agents/runtime.h"

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
    if (u.rfind("http://",  0) == 0) { u.erase(0, 7); https = false; }
    if (u.rfind("https://", 0) == 0) { u.erase(0, 8); https = true;  }
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

        std::string wav;
        if (msg.kind == "mic_wav_b64") {
            std::string enc = j.value("audio_b64", std::string(""));
            if (enc.empty()) { err_(rt, msg, "audio_b64 required"); return; }
            wav = b64_decode(enc);
        } else {
            std::string path = j.value("path", std::string(""));
            if (path.empty()) { err_(rt, msg, "path required"); return; }
            std::ifstream f(path, std::ios::binary);
            if (!f) { err_(rt, msg, "cannot read " + path); return; }
            std::ostringstream ss; ss << f.rdbuf(); wav = ss.str();
        }
        if (wav.empty()) { err_(rt, msg, "empty audio"); return; }

        std::string language = j.value("language", std::string("en"));

        std::string host; int port; bool https;
        split_url(url_, host, port, https);
        httplib::Client cli(host, port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(60);

        httplib::MultipartFormDataItems items = {
            {"audio",    wav,      "audio.wav", "audio/wav"},
            {"language", language, "",          ""},
        };
        auto res = cli.Post("/transcribe", items);
        if (!res) { err_(rt, msg, "whisper-server unreachable at " + url_); return; }
        if (res->status < 200 || res->status >= 300) {
            err_(rt, msg, "whisper HTTP " + std::to_string(res->status)
                       + ": " + res->body.substr(0, 200));
            return;
        }
        std::string text;
        try { text = nlohmann::json::parse(res->body).value("text", std::string("")); }
        catch (const std::exception& e) {
            err_(rt, msg, std::string("parse: ") + e.what()); return;
        }
        rt.send({.from=name_, .to=msg.from,
                 .kind="user_said", .payload=text});
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
