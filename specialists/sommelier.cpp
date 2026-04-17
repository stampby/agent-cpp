// sommelier — model routing specialist.
//
// One job: pick which backend handles a given decode request. Local
// BitNet is the default; paid APIs (OpenAI, Anthropic, Groq, DeepSeek,
// xAI, OpenRouter) become available when their API keys are set in the
// environment. Caller can also request a specific backend by name via
// the "hint" field.
//
// Contract:
//   listens for : "decode_request"  {messages, max_tokens?, temperature?,
//                                    top_p?, hint?("local"/"openai"/...)}
//                 "list_backends"
//   emits       : "decode_result"   {content, model_used, latency_ms, ...}
//                 "backends_list"   {backends: [{name, url, model}]}
//                 "decode_error"    {error, ...}
//
// Environment (all optional):
//   AGENT_CPP_LLM_URL            local OpenAI-compat endpoint (default http://127.0.0.1:8080)
//   AGENT_CPP_OPENAI_API_KEY     → enables openai backend
//   AGENT_CPP_GROQ_API_KEY       → enables groq (OpenAI-compat) backend
//   AGENT_CPP_DEEPSEEK_API_KEY   → enables deepseek (OpenAI-compat) backend
//   AGENT_CPP_XAI_API_KEY        → enables xai (OpenAI-compat) backend
//   AGENT_CPP_OPENROUTER_API_KEY → enables openrouter aggregator backend

#include "agents/agent.h"
#include "agents/runtime.h"
#include "agents/llm_client.h"

#include <cstdlib>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace rocm_cpp::agents::specialists {

namespace {

struct Backend {
    std::string name;
    std::string url;
    std::string api_key;  // empty = no auth
    std::string model;    // default model for this backend
};

// Build the catalog from env vars at construction time. Only enabled
// backends (with a key set, or the default local) end up in the list.
std::vector<Backend> discover_backends() {
    std::vector<Backend> out;

    // Local is always present — the engine.
    const char* local_url = std::getenv("AGENT_CPP_LLM_URL");
    out.push_back({
        "local",
        local_url ? local_url : "http://127.0.0.1:8080",
        "",
        "bitnet-b1.58-2b-4t",
    });

    auto add_if_set = [&](const char* envk, const char* name,
                          const char* url, const char* dflt_model) {
        if (const char* k = std::getenv(envk); k && *k) {
            out.push_back({name, url, k, dflt_model});
        }
    };
    add_if_set("AGENT_CPP_OPENAI_API_KEY",      "openai",
               "https://api.openai.com",           "gpt-4o-mini");
    add_if_set("AGENT_CPP_GROQ_API_KEY",        "groq",
               "https://api.groq.com/openai",      "llama-3.3-70b-versatile");
    add_if_set("AGENT_CPP_DEEPSEEK_API_KEY",    "deepseek",
               "https://api.deepseek.com",         "deepseek-chat");
    add_if_set("AGENT_CPP_XAI_API_KEY",         "xai",
               "https://api.x.ai",                 "grok-2-latest");
    add_if_set("AGENT_CPP_OPENROUTER_API_KEY",  "openrouter",
               "https://openrouter.ai/api",        "openrouter/auto");
    return out;
}

}  // namespace

class Sommelier : public Agent {
public:
    Sommelier() {
        for (auto& b : discover_backends()) {
            clients_.emplace(b.name,
                std::make_unique<LLMClient>(b.url, b.api_key, b.model));
        }
    }

    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind == "list_backends") {
            nlohmann::json backends = nlohmann::json::array();
            for (auto& [n, c] : clients_)
                backends.push_back({{"name", n},
                                    {"url", c->base_url()},
                                    {"model", c->model_id()}});
            nlohmann::json r = {{"backends", backends}};
            rt.send({.from=name_, .to=msg.from,
                     .kind="backends_list", .payload=r.dump()});
            return;
        }
        if (msg.kind != "decode_request") return;

        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) {
            emit_err(rt, msg, std::string("bad JSON: ") + e.what());
            return;
        }

        // Pick a backend.
        std::string hint = j.value("hint", std::string("local"));
        auto it = clients_.find(hint);
        if (it == clients_.end()) {
            emit_err(rt, msg, "unknown backend '" + hint + "'");
            return;
        }
        LLMClient* llm = it->second.get();

        // Build messages from payload.
        std::vector<ChatMessage> chat;
        for (auto& m : j.value("messages", nlohmann::json::array())) {
            chat.push_back({m.value("role",    std::string("user")),
                            m.value("content", std::string(""))});
        }
        if (chat.empty()) { emit_err(rt, msg, "no messages"); return; }

        ChatOptions opts;
        opts.max_tokens   = j.value("max_tokens",   256);
        opts.temperature  = j.value("temperature",  0.7);
        opts.top_p        = j.value("top_p",        0.9);
        opts.freq_penalty = j.value("frequency_penalty", 0.0);

        auto r = llm->chat(chat, opts);

        nlohmann::json out;
        if (r.ok) {
            out = {
                {"content",    r.content},
                {"model_used", hint},
                {"latency_ms", r.latency_ms},
                {"prompt_tokens",     r.prompt_tokens},
                {"completion_tokens", r.completion_tokens},
            };
            rt.send({.from=name_, .to=msg.from,
                     .kind="decode_result", .payload=out.dump()});
        } else {
            emit_err(rt, msg, "[" + hint + "] " + r.error);
        }
    }

private:
    std::string                                          name_ = "sommelier";
    std::unordered_map<std::string, std::unique_ptr<LLMClient>> clients_;

    static void emit_err(Runtime& rt, const Message& src, const std::string& why) {
        nlohmann::json j = {{"error", why}};
        rt.send({.from="sommelier", .to=src.from,
                 .kind="decode_error", .payload=j.dump()});
    }
};

std::unique_ptr<Agent> make_sommelier() { return std::make_unique<Sommelier>(); }

}  // namespace rocm_cpp::agents::specialists
