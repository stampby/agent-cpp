// llm_client — thin wrapper around rocm-cpp's OpenAI-compat HTTP endpoint.
//
// Specialists use this to "think" — send a prompt through librocm_cpp
// and get a string back. The LLM itself runs in a separate process
// (bitnet_decode --server), keeping agent-cpp free of HIP/ROCm deps.
//
// This is the only place in the core that knows about HTTP. Specialists
// just call llm_client::chat(...) and get text.

#ifndef ROCM_CPP_AGENTS_LLM_CLIENT_H
#define ROCM_CPP_AGENTS_LLM_CLIENT_H

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rocm_cpp::agents {

struct ChatMessage {
    std::string role;     // "system" / "user" / "assistant"
    std::string content;
};

struct ChatOptions {
    int    max_tokens  = 256;
    double temperature = 0.7;
    double top_p       = 0.9;
    double freq_penalty = 0.15;
    std::vector<std::string> stop;
};

struct ChatResult {
    bool        ok = false;
    std::string content;
    std::string error;
    double      latency_ms = 0.0;
    int         prompt_tokens = 0;
    int         completion_tokens = 0;
};

class LLMClient {
public:
    // base_url like "http://127.0.0.1:8080". If api_key is non-empty,
    // a Bearer authorization header is added to every request — covers
    // OpenAI, Groq, DeepSeek, OpenRouter, and any other OpenAI-compat
    // endpoint that uses Bearer auth.
    explicit LLMClient(std::string base_url = "http://127.0.0.1:8080",
                       std::string api_key = "",
                       std::string model_id = "bitnet-b1.58-2b-4t")
        : base_url_(std::move(base_url)), api_key_(std::move(api_key)),
          model_id_(std::move(model_id)) {
        parse_(base_url_, host_, port_);
    }

    const std::string& base_url() const { return base_url_; }
    const std::string& model_id() const { return model_id_; }

    ChatResult chat(const std::vector<ChatMessage>& msgs,
                    const ChatOptions& opts = {}) const
    {
        nlohmann::json body;
        body["model"]       = model_id_;
        body["max_tokens"]  = opts.max_tokens;
        body["temperature"] = opts.temperature;
        body["top_p"]       = opts.top_p;
        body["frequency_penalty"] = opts.freq_penalty;
        body["messages"]    = nlohmann::json::array();
        for (const auto& m : msgs)
            body["messages"].push_back({{"role", m.role}, {"content", m.content}});
        if (!opts.stop.empty()) body["stop"] = opts.stop;

        httplib::Client cli(host_, port_);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(300);

        httplib::Headers headers{{"Content-Type", "application/json"}};
        if (!api_key_.empty())
            headers.emplace("Authorization", "Bearer " + api_key_);

        ChatResult r;
        auto res = cli.Post("/v1/chat/completions", headers, body.dump(), "application/json");
        if (!res) { r.error = "cannot reach " + base_url_; return r; }
        if (res->status != 200) {
            r.error = "HTTP " + std::to_string(res->status) + ": " + res->body;
            return r;
        }
        try {
            auto j = nlohmann::json::parse(res->body);
            r.content = j["choices"][0]["message"]["content"].get<std::string>();
            auto u = j.value("usage", nlohmann::json::object());
            r.latency_ms         = u.value("latency_ms", 0.0);
            r.prompt_tokens      = u.value("prompt_tokens", 0);
            r.completion_tokens  = u.value("completion_tokens", 0);
            r.ok = true;
        } catch (const std::exception& e) {
            r.error = std::string("parse: ") + e.what();
        }
        return r;
    }

private:
    std::string base_url_;
    std::string api_key_;
    std::string model_id_;
    std::string host_;
    int         port_ = 8080;

    static void parse_(const std::string& url, std::string& host, int& port) {
        std::string u = url;
        if (u.rfind("http://", 0) == 0) u.erase(0, 7);
        if (u.rfind("https://", 0) == 0) u.erase(0, 8);
        auto slash = u.find('/'); if (slash != std::string::npos) u = u.substr(0, slash);
        auto colon = u.find(':');
        if (colon == std::string::npos) { host = u; port = 80; }
        else { host = u.substr(0, colon); port = std::atoi(u.c_str() + colon + 1); }
    }
};

}  // namespace rocm_cpp::agents
#endif
