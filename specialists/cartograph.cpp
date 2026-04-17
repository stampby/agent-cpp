// cartograph — memory specialist (keyword recall in v1).
//
// One job: remember things and return them on demand. Accepts free-form
// text entries, stores them, returns the k most relevant on recall.
//
// v1 uses Jaccard token overlap for scoring — cheap, deterministic,
// exercises the whole message-bus wiring. usearch is already pulled in
// as a FetchContent dep (see CMakeLists) so a follow-up commit can swap
// the scorer to ANN over real embeddings once an embedding specialist
// lands without touching the public message contract.
//
// Contract (JSON payloads):
//   listens for:
//     "remember"        {"text": "...", "tags"?: [...] }
//     "recall"          {"query": "...", "k"?: int}
//     "forget_all"      {}
//   emits:
//     "remembered"      {"id": int}                    -> to msg.from
//     "recall_result"   {"hits": [{id, text, score}]}  -> to msg.from

#include "agents/agent.h"
#include "agents/runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace rocm_cpp::agents::specialists {

namespace {

// Token-overlap scorer used when no embedding is supplied.
// Simple whitespace tokenization + lowercase, set intersection / union.
std::unordered_set<std::string> tokenize(const std::string& s) {
    std::unordered_set<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isalnum((unsigned char)c)) {
            cur.push_back((char)std::tolower((unsigned char)c));
        } else if (!cur.empty()) {
            out.insert(cur); cur.clear();
        }
    }
    if (!cur.empty()) out.insert(cur);
    return out;
}

double jaccard(const std::unordered_set<std::string>& a,
               const std::unordered_set<std::string>& b) {
    if (a.empty() || b.empty()) return 0.0;
    size_t inter = 0;
    for (const auto& t : a) if (b.count(t)) ++inter;
    size_t uni = a.size() + b.size() - inter;
    return uni ? (double)inter / (double)uni : 0.0;
}

struct Entry {
    int64_t     id{};
    std::string text;
    std::vector<std::string> tags;
    std::unordered_set<std::string> tokens;  // cached for keyword recall
};

}  // namespace

class Cartograph : public Agent {
public:
    const std::string& name() const override { return name_; }

    void handle(const Message& msg, Runtime& rt) override {
        if (msg.kind == "forget_all") {
            std::lock_guard<std::mutex> lk(mu_);
            entries_.clear();
            return;
        }

        nlohmann::json j;
        try { j = nlohmann::json::parse(msg.payload); }
        catch (const std::exception& e) {
            emit_error(rt, msg, std::string("bad JSON: ") + e.what());
            return;
        }

        if (msg.kind == "remember") {
            std::lock_guard<std::mutex> lk(mu_);
            Entry e;
            e.id   = (int64_t)entries_.size();
            e.text = j.value("text", std::string(""));
            if (e.text.empty()) { emit_error(rt, msg, "remember: empty text"); return; }
            if (j.contains("tags")) e.tags = j["tags"].get<std::vector<std::string>>();
            e.tokens = tokenize(e.text);
            entries_.push_back(std::move(e));

            nlohmann::json r = {{"id", entries_.back().id}};
            rt.send({.from=name_, .to=msg.from, .kind="remembered", .payload=r.dump()});
            return;
        }

        if (msg.kind == "recall") {
            std::lock_guard<std::mutex> lk(mu_);
            int k = j.value("k", 3);
            k = std::max(1, std::min(k, 32));

            std::string q = j.value("query", std::string(""));
            if (q.empty()) { emit_error(rt, msg, "recall: empty query"); return; }

            auto qtok = tokenize(q);
            std::vector<std::pair<double, int64_t>> scored;
            scored.reserve(entries_.size());
            for (const auto& e : entries_)
                scored.emplace_back(jaccard(qtok, e.tokens), e.id);
            std::partial_sort(
                scored.begin(),
                scored.begin() + std::min<size_t>(k, scored.size()),
                scored.end(),
                [](auto& a, auto& b) { return a.first > b.first; });
            scored.resize(std::min<size_t>(k, scored.size()));

            nlohmann::json hits = nlohmann::json::array();
            for (auto& [score, id] : scored) {
                if (score <= 0.0) continue;
                if (id < 0 || id >= (int64_t)entries_.size()) continue;
                const auto& e = entries_[(size_t)id];
                hits.push_back({
                    {"id",    e.id},
                    {"text",  e.text},
                    {"score", score},
                });
            }
            nlohmann::json r = {{"hits", hits}};
            rt.send({.from=name_, .to=msg.from, .kind="recall_result", .payload=r.dump()});
            return;
        }
    }

private:
    std::string            name_ = "cartograph";
    std::mutex             mu_;
    std::vector<Entry>     entries_;

    static void emit_error(Runtime& rt, const Message& src, const std::string& why) {
        nlohmann::json j = {{"error", why}};
        rt.send({.from="cartograph", .to=src.from,
                 .kind="recall_error", .payload=j.dump()});
    }
};

std::unique_ptr<Agent> make_cartograph() { return std::make_unique<Cartograph>(); }

}  // namespace rocm_cpp::agents::specialists
