#include "agents/runtime.h"

#include <cstdio>
#include <utility>

namespace rocm_cpp::agents {

Runtime::Runtime() = default;

Runtime::~Runtime() {
    shutdown();
}

void Runtime::register_agent(std::unique_ptr<Agent> a) {
    if (running_.load()) {
        std::fprintf(stderr, "[runtime] refusing to register '%s' after run()\n",
                     a->name().c_str());
        return;
    }
    auto mb = std::make_unique<Mailbox>();
    std::string n = a->name();
    mb->agent = std::move(a);
    agents_.emplace(std::move(n), std::move(mb));
}

bool Runtime::send(Message msg) {
    if (stopping_.load()) return false;
    auto it = agents_.find(msg.to);
    if (it == agents_.end()) {
        std::fprintf(stderr, "[runtime] dropped message to unknown '%s' (from '%s', kind '%s')\n",
                     msg.to.c_str(), msg.from.c_str(), msg.kind.c_str());
        return false;
    }
    msg.id = next_id_.fetch_add(1);
    Message audit_copy;
    bool do_audit = !audit_.empty() && audit_ != msg.to && agents_.count(audit_);
    if (do_audit) audit_copy = msg;   // pre-move copy
    {
        std::lock_guard<std::mutex> lk(it->second->m);
        it->second->q.push(std::move(msg));
    }
    it->second->cv.notify_one();
    if (do_audit) {
        auto ait = agents_.find(audit_);
        if (ait != agents_.end()) {
            {
                std::lock_guard<std::mutex> lk(ait->second->m);
                ait->second->q.push(std::move(audit_copy));
            }
            ait->second->cv.notify_one();
        }
    }
    return true;
}

void Runtime::worker_loop(Mailbox& mb) {
    mb.agent->start(*this);
    while (true) {
        Message msg;
        {
            std::unique_lock<std::mutex> lk(mb.m);
            mb.cv.wait(lk, [&] { return !mb.q.empty() || stopping_.load(); });
            if (mb.q.empty()) break;  // shutdown + drained
            msg = std::move(mb.q.front());
            mb.q.pop();
        }
        try {
            mb.agent->handle(msg, *this);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[runtime] '%s' threw on '%s': %s\n",
                         mb.agent->name().c_str(), msg.kind.c_str(), e.what());
        }
    }
    mb.agent->stop();
}

void Runtime::run() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    for (auto& [name, mb] : agents_) {
        mb->thr = std::thread([this, &mb = *mb] { worker_loop(mb); });
    }

    // Block until shutdown. The grace-period notify is fired from
    // shutdown() itself — don't double-notify here, it would race the
    // grace window and drop last-mile messages.
    while (!stopping_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (auto& [name, mb] : agents_) if (mb->thr.joinable()) mb->thr.join();
}

void Runtime::shutdown() {
    if (stopping_.exchange(true)) return;
    // 100 ms grace — lets in-flight handle() calls emit their follow-up
    // messages into peer inboxes before workers notice stopping_ and
    // exit. Avoids losing the last reply on a pipe-driven demo.
    std::thread([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (auto& [name, mb] : agents_) mb->cv.notify_all();
    }).detach();
}

}  // namespace rocm_cpp::agents
