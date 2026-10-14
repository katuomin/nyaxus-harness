# nyaxus

A single-binary C++20 agent harness with a React/Vite web client: durable
sessions, tool use with approval gates, prompt-cache-aware context
management, memory, and web research - sustained entirely by its own
end-to-end test suite.

This repository is a **curated export** of an actively developed private
repository. Public history is a series of dated snapshot commits, not the
raw development log. The coordination notes and design discussions stay
private; a design reference (architecture, API, tools, configuration) ships
in `docs/`.

## What is here

| path | contents |
|---|---|
| `harness/` | C++20 daemon (`harness/src`), HTTP/TLS plumbing, vendored deps, 88 shell e2e suites + 29 C++ unit programs |
| `web/` | React 19 + Vite client (`web/src`), client-side logic pinned by node test bundles (`web/scripts`) |
| `scripts/` | build & deploy script (`update.sh`), systemd units |
| `docs/` | design reference: architecture, HTTP API, agent tools, configuration |

Feature surface (see `docs/architecture.md` for details): streaming with
SSE resume, branching/rewind, approval gates (sudo scope), prompt packs,
group-chat runs, task checklists, subagent spawns, memory (custom
provider), web fetch escalation chain (native TLS → Firecrawl →
browser-render), attachments (text/image/document with OOXML text
extraction), output spill locator, prompt-cache tripwires.

## Build

The daemon builds with the zig cc toolchain (Linux and Windows use the
same sources and flags) - `scripts/update.sh` compiles it against the
vendored sqlite/mbedtls/miniz under `harness/third_party/` and builds the
web client. A CMake path exists under `harness/CMakeLists.txt` for IDE
integration.

```bash
bash scripts/update.sh        # daemon + web; see script header for details
```

Prerequisites: a recent zig, Node.js ≥ 20, and on Windows the MSYS
toolchain for the test scripts (bash, curl, mktemp). The web client runs
from `web/` (`npm install && npm run dev`), expecting the daemon on
`http://127.0.0.1:8787`.

## Testing

End-to-end tests start their own daemon + mock provider on private ports
and assert ledger/transcript/API behavior byte-for-byte:

```bash
bash harness/tests/attachments_test.sh      # one suite; there are many more
node web/scripts/build_markdown_test.mjs    # client bundles are tested too
```

## Configuration

Copy `.env.example` to `.env` and fill in provider keys. All state
(sessions, events, memory, settings) lives in a local SQLite database;
no daemon state is remote.

## License

This repository's code is Apache-2.0 (see `LICENSE`). The vendored
dependencies under `harness/third_party/` keep their own licenses:
mbed TLS (Apache-2.0 or GPL-2.0-or-later, dual), miniz (MIT), and
SQLite (public domain). The engine implements the Qwen3 architecture
and is verified against its tokenizer tables, but contains no upstream
model or inference code; Qwen3 model weights themselves are not
included here and are licensed separately by their authors.

## Status

Actively developed. This mirror is refreshed from the private repo
periodically; the newest snapshot is the latest commit on `main`.
