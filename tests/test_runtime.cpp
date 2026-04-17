// Minimal runtime smoke test — confirm message bus routes end-to-end.

#include "agents/agent.h"
#include "agents/runtime.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <thread>

using namespace rocm_cpp::agents;

namespace {
class Echo : public Agent {
public:
    std::atomic<int> received{0};
    const std::string& name() const override { return name_; }
    void handle(const Message& msg, Runtime&) override {
        received.fetch_add(1);
        last = msg.payload;
    }
    std::string last;
private:
    std::string name_ = "echo";
};
}  // namespace

int main() {
    Runtime rt;
    auto a = std::make_unique<Echo>();
    Echo* raw = a.get();
    rt.register_agent(std::move(a));

    std::thread driver([&] {
        for (int i = 0; i < 5; ++i) {
            rt.send({ .from = "test", .to = "echo",
                      .kind = "ping", .payload = "hi" });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rt.shutdown();
    });
    rt.run();
    driver.join();

    assert(raw->received.load() == 5);
    assert(raw->last == "hi");
    std::puts("test_runtime: OK");
    return 0;
}
