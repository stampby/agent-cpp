// forge — MCP tool dispatch.
//
// One job: expose every tool the agent stack can invoke (shell, web,
// file, Discord, GitHub, etc.) through the Model Context Protocol. The
// model emits a JSON tool call, forge validates against the tool's
// schema, runs it via the appropriate backend, returns the result.
//
// Contract:
//   listens for : "tool_call"  (payload = JSON { name, args })
//   emits       : "tool_result" (payload = JSON { id, ok, data, error })
// Dep          : hkr04/cpp-mcp (MIT) for the MCP client/server glue
//                valijson       (BSD, header-only) for schema checks

#include "agents/agent.h"
#include "agents/runtime.h"
#include <memory>

namespace rocm_cpp::agents::specialists {
class Forge : public Agent {
public:
    const std::string& name() const override { return name_; }
    void handle(const Message& /*msg*/, Runtime& /*rt*/) override {
        // TODO: cpp-mcp client, tool registry, dispatch to warden for exec.
    }
private:
    std::string name_ = "forge";
};
std::unique_ptr<Agent> make_forge() { return std::make_unique<Forge>(); }
}  // namespace
