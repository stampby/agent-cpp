#include "halo_mcp/bus_bridge.hpp"

#include "agents/message.h"

#include <future>
#include <utility>

namespace halo_mcp {

using rocm_cpp::agents::Message;
using rocm_cpp::agents::Runtime;

BusBridge::BusBridge(std::chrono::milliseconds timeout) : timeout_(timeout) {}

void BusBridge::handle(const Message& msg, Runtime& /*rt*/) {
    std::unique_lock<std::mutex> lk(mu_);
    auto it = pending_.find(msg.from);
    if (it == pending_.end()) {
        // Unsolicited / broadcast / late reply — drop silently.
        return;
    }
    auto promise = std::move(it->second);
    pending_.erase(it);
    lk.unlock();

    BridgeReply r;
    r.ok = true;
    r.payload = msg.payload;
    promise.set_value(std::move(r));
}

BridgeReply BusBridge::send_request(Runtime& rt,
                                     const std::string& target,
                                     const std::string& kind,
                                     const std::string& payload) {
    std::future<BridgeReply> fut;
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (pending_.find(target) != pending_.end()) {
            return BridgeReply{
                .ok = false,
                .error = "another request to " + target + " is already in flight (single-flight limitation)",
            };
        }
        std::promise<BridgeReply> promise;
        fut = promise.get_future();
        pending_.emplace(target, std::move(promise));
    }

    Message m;
    m.from    = name_;
    m.to      = target;
    m.kind    = kind;
    m.payload = payload;

    if (!rt.send(std::move(m))) {
        std::unique_lock<std::mutex> lk(mu_);
        pending_.erase(target);
        return BridgeReply{.ok = false, .error = "bus send failed (target registered?)"};
    }

    if (fut.wait_for(timeout_) != std::future_status::ready) {
        std::unique_lock<std::mutex> lk(mu_);
        pending_.erase(target);
        return BridgeReply{
            .ok = false,
            .error = "timed out after " + std::to_string(timeout_.count()) + "ms waiting for " + target,
        };
    }
    return fut.get();
}

}
