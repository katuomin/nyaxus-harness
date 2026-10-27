# Configuration

All configuration is via environment variables (a `.env` file or a service
manager's environment). Unset variables fall back to built-in defaults, so an
empty file is a valid configuration. `scripts/publish/dotenv.example` is the
canonical, commented list; this page groups it.

Settings can also be read and written at runtime through
`GET/PUT /api/v1/settings`.

## Daemon

| variable | default | purpose |
|---|---|---|
| `NYAXUS_PORT` | `8787` | HTTP + SSE port |
| `NYAXUS_WORKSPACE` | `.` | working dir for durable state (`nyaxus.db`, `memory.db`, spills) |
| `NYAXUS_PASSWORD` | empty | password gate for the API; empty = all routes open |

## Primary provider

The engine is any OpenAI-compatible endpoint (llama.cpp, vLLM, …) or the
built-in `fake` echo engine (default, no network).

| variable | purpose |
|---|---|
| `NYAXUS_PROVIDER` | `openai` or `fake` |
| `NYAXUS_LLAMA_URL` | base URL of the endpoint |
| `NYAXUS_MODEL` | model name |
| `NYAXUS_API_KEY` | bearer key (remote providers) |
| `NYAXUS_TEMPERATURE` | sampling temperature |
| `NYAXUS_MAX_TOKENS` | max completion tokens |
| `NYAXUS_AUXILIARY_VISION` | connection-level capability override |
| `NYAXUS_AUXILIARY_NO_THINKING` | disable reasoning output |
| `NYAXUS_DEFAULT_PROVIDER` | connection the UI's profile picker opens by default |
| `NYAXUS_TITLE_MODEL` | dedicated fast model for titles; `NYAXUS_TITLE_ENABLED=0` turns it off |

## Auxiliary model

A separate, cheaper model for context compaction (summaries).

| variable | purpose |
|---|---|
| `NYAXUS_AUXILIARY_URL` | endpoint |
| `NYAXUS_AUXILIARY_MODEL` | model name |
| `NYAXUS_AUXILIARY_API_KEY` | bearer key |
| `NYAXUS_AUXILIARY_TEMPERATURE` | temperature |
| `NYAXUS_AUXILIARY_MAX_TOKENS` | max tokens |

## Memory

`custom` = the in-process bank (`memory.db`); `hindsight` = an HTTP service.
`NYAXUS_MEMORY_ENABLED=0` turns memory off entirely.

| variable | purpose |
|---|---|
| `NYAXUS_MEMORY_PROVIDER` | `custom` or `hindsight` |
| `NYAXUS_MEMORY_URL` | Hindsight service URL |
| `NYAXUS_MEMORY_API_KEY` | Hindsight key |
| `NYAXUS_MEMORY_BANK` | bank name |
| `NYAXUS_MEMORY_DB_PATH` | SQLite path for the custom bank |
| `NYAXUS_MEMORY_RETAIN_EVERY_N_TURNS` | retain cadence |
| `NYAXUS_MEMORY_RECALL_FLOOR` | recall relevance floor |
| `NYAXUS_MEMORY_SWEEP_COSINE_THRESHOLD` | near-duplicate bar for consolidation |
| `NYAXUS_MEMORY_SWEEP_IDLE_SECONDS` | idle sweep interval |
| `NYAXUS_MEMORY_EXTRACT_URL` / `_MODEL` / `_API_KEY` | extraction model |
| `NYAXUS_MEMORY_EMBED_MODEL` | embedding model |
| `NYAXUS_MEMORY_RERANK_MODEL` / `_TOP` | reranker and top-k |
| `NYAXUS_MEMORY_RECALL_ON_TURN` | per-turn recall injection |

## Web research

`NYAXUS_WEB_ENABLED=0` removes the web tool set.

| variable | purpose |
|---|---|
| `NYAXUS_WEB_CA_BUNDLE` | CA bundle for the native TLS tier |
| `NYAXUS_WEB_FETCH_MAX_BYTES` | fetch byte cap |
| `NYAXUS_WEB_FETCH_MAX_CHARS` | fetch char cap |
| `NYAXUS_WEB_FETCH_TIMEOUT_MS` | fetch timeout |
| `NYAXUS_WEB_FIRECRAWL_ENDPOINT` | second escalation tier |
| `NYAXUS_WEB_SEARCH_ENDPOINT` | search endpoint for `web.search` |
| `NYAXUS_WEB_CAMOFOX_ENDPOINT` / `_TOKEN` | browser-render tier |
| `NYAXUS_WEB_CAMOFOX_STICKY` | sticky per-session browser (1 = on) |
| `NYAXUS_WEB_CAMOFOX_STICKY_TTL_MINUTES` | sticky idle-tab reaper TTL (default 15) |

## Summaries, context, terminal

| variable | purpose |
|---|---|
| `NYAXUS_SUMMARY_SOURCE_BUDGET_BYTES` | summary source budget |
| `NYAXUS_SUMMARY_INCLUDE_REASONING` | include reasoning in summaries |
| `NYAXUS_CONTEXT_MICROCOMPACT` | microcompact toggle |
| `NYAXUS_TERMINAL_READONLY_AUTO_APPROVE` | auto-approve read-only terminal commands |
| `NYAXUS_TERMINAL_SPILL_FULL_OUTPUT` | spill full output (off = head/tail reduction) |

## Engine tuning and diagnostics

| variable | purpose |
|---|---|
| `NYAXUS_ENGINE_THREADS` | in-process engine worker threads |
| `NYAXUS_NO_AVX2` | `1` disables the AVX2 kernels |
| `NYAXUS_LOG_LEVEL` | log verbosity |
| `NYAXUS_DEBUG_LAYERS` | render-layer diagnostics |
| `NYAXUS_MEM_TIMING` | memory timing diagnostics |
