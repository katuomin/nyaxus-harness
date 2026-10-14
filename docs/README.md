# nyaxus - reference

How nyaxus works: the architecture, the HTTP API, the agent tools, and
configuration. This is the public design reference; it describes shipped
behavior, not in-flight plans.

## Start here

1. [`architecture.md`](architecture.md) - the daemon/client shape, the module
   map, the SQLite store, and the event ledger.
2. [`api.md`](api.md) - the full HTTP API, grouped by area.
3. [`tools.md`](tools.md) - the tools an agent run can call.
4. [`configuration.md`](configuration.md) - every setting and the `.env`
   surface.

## What nyaxus is

A single-binary C++20 agent harness with a React/Vite web client. It runs
model-backed agent runs with durable sessions, tool use behind approval
gates, prompt-cache-aware context management, a pluggable memory bank, and
grounded web research - all sustained by an end-to-end test suite that
drives a real daemon and asserts on the ledger byte-for-byte.

- `harness/` - the daemon: C++ sources in `harness/src`, headers in
  `harness/include/nyaxus`, vendored dependencies in `harness/third_party`.
- `web/` - the client: React source in `web/src`, client-side logic pinned by
  node test bundles in `web/scripts`.
- `docs/` - this reference.

## Build

```bash
bash scripts/update.sh        # build daemon + web client
```

Prerequisites: a recent `zig`, Node.js ≥ 20, and (on Windows) the MSYS
toolchain for the test scripts. The web client runs from `web/`
(`npm install && npm run dev`) against the daemon on `http://127.0.0.1:8787`.

## Testing

```bash
bash harness/tests/attachments_test.sh      # one of many e2e suites
node web/scripts/build_markdown_test.mjs    # client bundles are tested too
```

Each e2e suite starts its own daemon and a mock provider on private ports and
asserts on ledger, transcript, and API behavior.
