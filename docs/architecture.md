# Architecture

nyaxus is a single daemon process with a thin web client. All durable state
lives in local SQLite; nothing is remote. This page maps the moving parts and
the data flow between them.

## Daemon / client split

- **Daemon** (`harness/`): owns the HTTP + SSE server, the model provider
  connection, the tool executor, the memory bank, and the SQLite store. The
  client never talks to a model or a database directly.
- **Client** (`web/`): a React app that renders sessions and runs, streams
  `text.delta`/`reasoning.delta` over SSE, and submits user actions through
  the HTTP API. All logic the client can own is pinned by node test bundles in
  `web/scripts`.

The two talk over `http://127.0.0.1:8787` by default.

## Module map

Sources live in `harness/src`, headers in `harness/include/nyaxus`. The
module boundaries are deliberate - each compiles and tests independently:

| module | responsibility |
|---|---|
| `core` | shared types, ids, the config struct, small utilities |
| `daemon` | the `Daemon` class: routes, the run loop, tool dispatch, SSE |
| `daemon_config` | `load_config` - env parsing and defaults |
| `store` | the SQLite store: sessions, runs, messages, events, settings |
| `provider` | the OpenAI-compatible client: chat, streaming, capability discovery |
| `qwen3` | the in-process Qwen3 engine (SSE2/AVX2/AVX kernels), verified against tokenizer tables |
| `memory` | the Hindsight memory client (HTTP fallback provider) |
| `memory_custom` | the in-process memory bank: retain/recall/consolidate/curate |
| `memorydb` | the memory bank's SQLite schema (append-only versions) |
| `promptpacks` | modular prompt storage, profile stacks, token verification |
| `skills` | the skill loader and envelope |
| `http` | HTTP/TLS request plumbing |
| `json` | the JSON value type and parser |
| `log` | leveled logging |
| `infra` | the bounded host snapshot behind `GET /api/v1/infra` |
| `argvpolicy` | the read-only argv policy for terminal children |
| `commandplane` | the command-plane request model |
| `denyguard` | deny rules for tool/argument gating |
| `gatepolicy` | approval-gate policy (once/ttl/session scopes) |
| `fingerprint` | the fingerprint rulebook for stable ids |
| `frozen` | frozen-section bookkeeping |
| `loopguard` | runaway-loop tripwires |
| `streamrules` | stream shaping rules |
| `summary` | context summarization |

## State

Two SQLite files under the workspace directory (default `.`):

- **`nyaxus.db`** - sessions, runs, messages, the event ledger, settings,
  profiles, prompt packs, attachments.
- **`memory.db`** - the memory bank (units, versions, sources, links,
  feedback), only when the custom provider is active.

The **event ledger** is append-only and is the source of truth for what
happened in a run. The client and the tests both read it; the daemon never
mutates history, it appends.

## Run lifecycle

A **session** is a durable conversation. Each user turn starts a **run**:

`run.queued → run.started → (tool calls, `run.waiting_approval`,
`run.waiting_input`) → run.completed | run.failed | run.cancelled`.

Runs form a tree. A session has an **active leaf**; `rewind` and `select`
move it, which is how branching and rewind work. New runs anchor on the
current leaf, so steering a conversation forks from the leaf rather than
mutating it.

## Web research

`web.fetch` escalates across tiers until it gets a readable page: a native
TLS fetch first, then an optional Firecrawl endpoint, then a browser-render
service. `web.search` hits a search endpoint and returns a per-session source
ledger that `web.use` cites. See [`tools.md`](tools.md#web-research).
