# halo-mcp

C++20 **Model Context Protocol** server that bridges `agent-cpp` specialists to MCP clients. Standalone binary. Stdio/JSON-RPC 2.0. No Python, no embedded runtime — just a binary that links `libagent_cpp.a` and speaks MCP on stdin/stdout.

Full design doc: [`halo-ai-core/docs/mcp-nexus-design.md`](https://github.com/stampby/halo-ai-core/blob/main/docs/mcp-nexus-design.md)

## Status

| Phase | Scope | Status |
|---|---|---|
| **0** | Stdio JSON-RPC, tool registry, canned handler | ✅ **Shipped** |
| **1** | Bus bridge — route `tools/call` into the agent-cpp Runtime | 🔨 next |
| **2** | Nexus enrollment — advertise tool manifest via `lemonade-nexus` | ⏳ |
| **3** | Federated calls — HTTPS over WireGuard tunnel to peer nodes | ⏳ |
| **4** | Write specialists (herald/quartermaster/magistrate/anvil) over remote | ⏳ |
| **5** | Latency-aware routing when multiple peers advertise the same tool | ⏳ |

## Phase 0 — what ships today

- `halo_mcp` binary, stdio-driven, answers `initialize` / `tools/list` / `tools/call`
- **12 tools** registered across these targets:

| Target specialist | Tools |
|---|---|
| `scribe` | `scribe_recall` |
| `librarian` | `librarian_lookup`, `github_search_repo`, `github_list_prs` |
| `cartograph` | `cartograph_repo_map` |
| `sentinel` | `sentinel_discord_read` |
| `magistrate` | `github_review_pr` |
| `quartermaster` | `github_create_issue`, `github_comment` |
| `sommelier` | `google_drive_search`, `google_drive_read`, `google_gmail_search`, `google_calendar_upcoming` |

- Handler is a **stub** — returns a canned response describing which specialist/kind the call *would* route to. Phase 1 replaces this with a real `Runtime::send` + reply await.

## Phase 1 — bus bridge (next)

**Goal:** `tools/call` → real message on the agent-cpp bus → real reply back to the MCP client.

### Design

1. **`BusBridge` = an `Agent` named `halo-mcp-bridge`.** Registered with a `Runtime` that halo-mcp owns. Runtime lives on a background thread; MCP stdio loop lives on main.

2. **Request ↔ reply correlation via `correlation_id` in payload.**
   - `halo-mcp-bridge.send_request(target, kind, args_json)`:
     - generates `correlation_id = next_uuid()`
     - creates `std::promise<json>` stored in `pending_[correlation_id]`
     - emits `Message{.from="halo-mcp-bridge", .to=target, .kind=kind, .payload=json{{"correlation_id", id}, {"args", args}}.dump()}`
     - returns `future.get(timeout=30s)` — blocks the MCP worker, not the Runtime thread
   - `halo-mcp-bridge.handle(msg)`:
     - parses `correlation_id` from `msg.payload` (all specialists preserve it on reply — see specialist contract below)
     - looks up promise, fulfills with the reply payload
     - ignores messages without correlation_id (broadcast, out-of-band, etc.)

3. **Specialist contract (soft):** if `msg.payload` contains `correlation_id`, reply payload MUST include the same `correlation_id`. Already the case for most `_done`/`_error` specialists once they read it; a one-liner helper in `agent-cpp` (say `build_reply(msg, result)`) makes this trivial. Broadcast messages without correlation stay unaffected.

4. **Timeout & cleanup:**
   - 30s default per tool call (configurable via `HALO_MCP_TIMEOUT_MS`)
   - On timeout: return JSON-RPC `-32001 "tool timed out"`; drop the pending entry (if the reply arrives late, it's silently ignored)
   - On Runtime shutdown: all pending promises fulfilled with an error

### Build changes

Phase 1 `CMakeLists.txt`:

```cmake
target_link_libraries(halo_mcp PRIVATE
    agent_cpp_static              # needs to become a static target in parent
    nlohmann_json::nlohmann_json
)
target_include_directories(halo_mcp PRIVATE ${CMAKE_SOURCE_DIR}/include)
```

Parent `agent-cpp/CMakeLists.txt` will need to expose a static library target that bundles Runtime + Message + Agent + specialists (or a subset). Currently there's only `agent_cpp` as an executable.

## Phase 2+ — see the main design doc

Nexus enrollment, federated HTTPS over WireGuard, CVG policy on inbound remote calls, latency-aware routing: all covered in [`halo-ai-core/docs/mcp-nexus-design.md`](https://github.com/stampby/halo-ai-core/blob/main/docs/mcp-nexus-design.md).

## Build

```bash
cd mcp
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/halo_mcp  # reads JSON-RPC from stdin, writes to stdout
```

## Smoke test

```bash
(echo '{"jsonrpc":"2.0","id":1,"method":"initialize"}'
 echo '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
 echo '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"librarian_lookup","arguments":{"query":"bitnet"}}}'
) | ./build/halo_mcp
```

Expected: three JSON-RPC responses, one per line, `id` matching request, `result` populated.

## Register with Claude Code

```jsonc
// ~/.config/claude-code/mcp.json
{
  "servers": {
    "halo-mcp": {
      "command": "/home/bcloud/repos/agent-cpp/mcp/build/halo_mcp"
    }
  }
}
```

## Rules

- **Rule A (no Python at runtime):** ✅ pure C++
- **Rule B (C++ first):** ✅ — this is explicitly the C++ replacement for the Python MCP bridges you'd otherwise write
