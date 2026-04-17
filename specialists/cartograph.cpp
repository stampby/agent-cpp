// cartograph — vector recall over usearch + sqlite-vec.
//
// One job: given a query embedding, return the top-k nearest entries
// from the long-term memory store. Also accepts "remember" with an
// embedding + text and persists it.
//
// Contract:
//   listens for : "recall" (payload = JSON {embedding:[], k:int}),
//                 "remember" (payload = JSON {embedding:[], text:...})
//   emits       : "recall_result" (payload = JSON {hits:[...]})
// Storage      : $XDG_DATA_HOME/agent-cpp/memory.usearch + memory.db

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Cartograph : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: usearch index + sqlite-vec durable store. Embeddings via librocm_cpp.
    }
private:
    std::string name_ = "cartograph";
};
std::unique_ptr<Agent> make_cartograph() { return std::make_unique<Cartograph>(); }
}  // namespace
