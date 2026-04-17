# agent-cpp

C++20 agent framework. Every agent has **one job** and is an **expert at
it**. Specialists snap in, snap out. Core services are stable.

Companion to [rocm-cpp](https://github.com/stampby/rocm-cpp) (the 1-bit
blaster). agent-cpp is the nervous system; rocm-cpp is the brain.

## Design principles (not suggestions)

1. **One agent, one job.** If you find yourself adding a second
   responsibility to a specialist, split it into two.
2. **Everything is lego blocks.** Specialists are bricks. The Runtime
   is the baseplate. No specialist reaches into another — all talk
   goes through the message bus.
3. **Easy in, easy out — except core services.** Adding or removing a
   specialist must be a single file + one CMake line. Core headers
   (`agents/agent.h`, `agents/message.h`, `agents/runtime.h`) are
   stable — PRs that change their signatures need very good reasons.
4. **No Python at runtime.** Build-time tooling fine. Inference, agent
   loop, IO, everything that runs after build completes — pure C++.
5. **BitNet / 1-bit only as the inference brain.** The default backend
   is `librocm_cpp`. No FP16 LLMs, no GGUF glue, no alternative quant
   schemes in-tree. If you want llama.cpp compat, fork this repo.
6. **MIT / BSD / Apache only.** No GPL contagion.

PRs that violate any of the above will be closed without debate.

## What's in the box today (M0 scaffold)

Core (stable — don't churn these):

| Piece | File |
|---|---|
| Message envelope | `include/agents/message.h` |
| Agent interface | `include/agents/agent.h` |
| Runtime (threads + inbox + grace shutdown) | `include/agents/runtime.h`, `src/runtime.cpp` |
| `agent_cpp` demo binary | `src/main.cpp` |
| Smoke test | `tests/test_runtime.cpp` |

Specialists — 15 stubs landed, each a single `.cpp` with a clear
`listens for` / `emits` contract in the header. Only `muse` + `stdout_sink`
produce real output in the scaffold; the rest are slot placeholders
with TODOs inside — claim one in an issue and flesh it out.

| Specialist | One job | Auth |
|---|---|---|
| **muse** | Voice persona (Man Cave). Takes `user_said`, emits `muse_reply`. | — |
| **stdout_sink** | Debug echo — prints every message it gets. | — |
| **scribe** | Session persistence + context compaction (JSONL). | — |
| **cartograph** | Vector recall over usearch + sqlite-vec. | — |
| **forge** | MCP tool dispatch (schema-validated). | — |
| **warden** | Permission-gated exec. Blocks dangerous tool calls. | — |
| **planner** | ReAct loop over librocm_cpp, GBNF-constrained JSON. | — |
| **sommelier** | Model routing (BitNet-2B / 8B / domain fine-tunes). | — |
| **echo_ear** | Whisper.cpp STT bridge (unix socket). | — |
| **echo_mouth** | Kokoro TTS bridge + visualizer hooks. | — |
| **sentinel** | Discord gateway watcher (read all channels). | `DISCORD_TOKEN` |
| **herald** | Discord poster (announcements, replies, reactions). | `DISCORD_TOKEN` |
| **quartermaster** | GitHub issue triage + labels + FAQ answers. | `GH_TOKEN` |
| **magistrate** | GitHub PR review — checks repo contribution rules. | `GH_TOKEN` |
| **librarian** | Docs + changelog upkeep, opens docs-only PRs. | `GH_TOKEN` |
| **anvil** | CI — rebuild + bench on push, posts results. | both |
| **carpenter** | Install-support helper over Discord. | both |

Read + write split on the support specialists is deliberate: `sentinel`
reads Discord, `herald` writes. `quartermaster`/`magistrate`/`librarian`
share one GH token but call different API surfaces. This way an audit
log of outbound traffic is trivial — grep for the writer's name.

## Build + run

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/agent_cpp
# type a line, press enter — muse replies via the stdout sink
```

With tests:

```bash
cmake -S . -B build -DAGENT_CPP_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Planned specialists (pick one, PR it)

Each of these is its own milestone. Claim one in an issue before starting.

| Specialist | Job |
|---|---|
| `scribe` | context compaction + JSONL session persistence |
| `cartograph` | vector recall over `usearch` + `sqlite-vec` |
| `forge` | tool dispatch + MCP client ([hkr04/cpp-mcp](https://github.com/hkr04/cpp-mcp)) |
| `warden` | permission-gated execution (file / shell / net) |
| `planner` | ReAct / tree-of-thought over librocm_cpp |
| `sommelier` | model routing across loaded checkpoints |
| `echo_ear` | Whisper.cpp STT bridge over unix socket |
| `echo_mouth` | Kokoro TTS bridge over unix socket |

## Contributor checklist

Before opening a PR that adds a specialist:

- [ ] Specialist lives in `specialists/<name>.cpp` (one file if possible)
- [ ] Exposes exactly one `make_<name>()` factory
- [ ] Does not include other specialists' headers
- [ ] Documents its `listens for` / `emits` contract in a header comment
- [ ] Adds exactly one line to `CMakeLists.txt` under "Specialists"
- [ ] Adds a tests/test_<name>.cpp if behavior is non-trivial
- [ ] License is MIT / BSD / Apache (no GPL deps, even transitively)

## Where this fits

```
          user voice / keys
                │
          ┌─────▼──────┐
          │  FTXUI TUI │  (rocm-cpp/tools/bitnet_tui, see spec 15)
          └─────┬──────┘
                │  messages
          ┌─────▼──────┐
          │ agent-cpp  │  ← you are here
          │  Runtime   │
          └─────┬──────┘
                │  calls
          ┌─────▼──────┐
          │ librocm_cpp│  (BitNet-b1.58 on gfx1151, 82 tok/s)
          └────────────┘
```

## License

MIT. See [LICENSE](LICENSE).
