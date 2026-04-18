// gateway — HTTP ingestion endpoint for the agent bus.
//
// One job: listen on :8081, accept POST /bus/:to with a JSON body,
// convert to a Message, and send() it onto the bus. Makes agent-cpp
// reachable from third-party apps, cron jobs, webhooks — anything
// that can curl.
//
// Bearer auth: if AGENT_CPP_GATEWAY_TOKEN is set, every request must
// carry Authorization: Bearer <token>. Otherwise open (dev mode).
//
// Rate limit: per-source-IP sliding-window counter, default 30 req/10s.
// Crude but sufficient; upgrade to a bucket when needed.
//
// Contract:
//   endpoint :
//     POST /bus/<to>
//       body : {"kind": "...", "payload": "..."}
//       resp : 202 Accepted  {"id": <msg_id>}
//              401 Unauthorized  (bad or missing bearer)
//              429 Too Many Requests  (rate limit)
//              400 Bad Request  (missing to / bad json)
//     GET  /healthz
//       resp : 200 {"ok": true, "specialists": <count>}
//
// Env :
//   AGENT_CPP_GATEWAY_PORT     default 8081
//   AGENT_CPP_GATEWAY_BIND     default 127.0.0.1 (loopback only)
//   AGENT_CPP_GATEWAY_TOKEN    optional bearer, strongly recommended
//   AGENT_CPP_GATEWAY_RATE     optional, default "30/10" (N req / N sec)

#include "agents/agent.h"
#include "agents/runtime.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>

#include <httplib.h>

namespace rocm_cpp::agents::specialists {

class Gateway : public Agent {
public:
    Gateway() {
        const char* p = std::getenv("AGENT_CPP_GATEWAY_PORT");
        port_ = (p && *p) ? std::atoi(p) : 8081;
        const char* b = std::getenv("AGENT_CPP_GATEWAY_BIND");
        bind_ = (b && *b) ? b : "127.0.0.1";
        if (const char* t = std::getenv("AGENT_CPP_GATEWAY_TOKEN"); t && *t)
            token_ = t;
        if (const char* r = std::getenv("AGENT_CPP_GATEWAY_RATE"); r && *r) {
            std::string s = r;
            auto slash = s.find('/');
            if (slash != std::string::npos) {
                rate_max_    = std::atoi(s.substr(0, slash).c_str());
                rate_window_ = std::atoi(s.c_str() + slash + 1);
            }
        }
    }

    ~Gateway() override { stop(); }

    const std::string& name() const override { return name_; }

    void start(Runtime& rt) override {
        rt_ = &rt;
        running_ = true;
        srv_thr_ = std::thread([this] { this->serve_(); });
    }

    void stop() override {
        running_ = false;
        if (srv_) srv_->stop();
        if (srv_thr_.joinable()) srv_thr_.join();
    }

    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // Gateway is push-only from the outside-in — it doesn't listen on
        // the bus itself. The HTTP thread drives everything.
    }

private:
    std::string        name_  = "gateway";
    std::string        bind_  = "127.0.0.1";
    int                port_  = 8081;
    std::string        token_;                 // empty = no auth
    int                rate_max_    = 30;      // req per window
    int                rate_window_ = 10;      // seconds

    Runtime*           rt_ = nullptr;
    std::thread        srv_thr_;
    std::unique_ptr<httplib::Server> srv_;
    std::atomic<bool>  running_{false};

    std::mutex         rate_mu_;
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> seen_;

    bool rate_limit_ok_(const std::string& ip) {
        auto now = std::chrono::steady_clock::now();
        auto cutoff = now - std::chrono::seconds(rate_window_);
        std::lock_guard<std::mutex> lk(rate_mu_);
        auto& q = seen_[ip];
        while (!q.empty() && q.front() < cutoff) q.pop_front();
        if ((int)q.size() >= rate_max_) return false;
        q.push_back(now);
        return true;
    }

    bool auth_ok_(const httplib::Request& req) {
        if (token_.empty()) return true;        // no token configured -> open
        auto a = req.get_header_value("Authorization");
        return a == "Bearer " + token_;
    }

    void serve_() {
        srv_ = std::make_unique<httplib::Server>();

        srv_->Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
            nlohmann::json j = {{"ok", true}};
            res.set_content(j.dump(), "application/json");
        });

        srv_->Post(R"(/bus/([A-Za-z_]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (!auth_ok_(req)) {
                res.status = 401;
                res.set_content("{\"error\":\"unauthorized\"}", "application/json");
                return;
            }
            std::string ip = req.remote_addr;
            if (!rate_limit_ok_(ip)) {
                res.status = 429;
                res.set_content("{\"error\":\"rate limit\"}", "application/json");
                return;
            }

            std::string to = req.matches[1];
            if (to.empty()) { res.status = 400;
                res.set_content("{\"error\":\"missing :to\"}", "application/json"); return; }

            nlohmann::json body;
            try { body = nlohmann::json::parse(req.body); }
            catch (const std::exception& e) {
                res.status = 400;
                nlohmann::json err = {{"error", std::string("bad json: ") + e.what()}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            std::string kind    = body.value("kind",    std::string(""));
            std::string payload = body.value("payload", std::string(""));
            if (kind.empty()) { res.status = 400;
                res.set_content("{\"error\":\"missing 'kind'\"}", "application/json"); return; }

            Message m;
            m.from    = name_;
            m.to      = to;
            m.kind    = kind;
            m.payload = payload;
            rt_->send(std::move(m));

            res.status = 202;
            nlohmann::json ok = {{"accepted", true}, {"to", to}, {"kind", kind}};
            res.set_content(ok.dump(), "application/json");
        });

        std::fprintf(stderr, "[gateway] listening on %s:%d (auth=%s)\n",
                     bind_.c_str(), port_, token_.empty() ? "open" : "bearer");
        srv_->listen(bind_, port_);
        std::fprintf(stderr, "[gateway] stopped\n");
    }
};

std::unique_ptr<Agent> make_gateway() { return std::make_unique<Gateway>(); }

}  // namespace rocm_cpp::agents::specialists
