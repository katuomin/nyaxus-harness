# HTTP API

The daemon serves a versioned REST API under `/api/v1` plus an SSE stream for
live run output. `GET /api/v1/routes` returns the live route table.

Authentication: when `NYAXUS_PASSWORD` is set, `POST /api/v1/auth/login`
issues a bearer token; with it unset every route is open (single-host setups).

Paths use `{param}` placeholders. Descriptions below are one line each; the
request/response shapes are asserted by the e2e suites in `harness/tests`.

## System

| route | purpose |
|---|---|
| `GET /api/v1/health` | liveness probe |
| `GET /api/v1/routes` | the live route table |
| `GET /api/v1/infra` | bounded host snapshot: RAM/swap/load, docker containers with OOM restart counters, provider leds |
| `POST /api/v1/auth/login` | exchange the password for a bearer token |

## Providers and models

| route | purpose |
|---|---|
| `GET /api/v1/providers` | list configured provider connections |
| `GET /api/v1/providers/{name}/check` | probe one connection (model reachable, capabilities) |
| `DELETE /api/v1/providers/{name}` | remove a connection |
| `GET /api/v1/provider/check` | probe the primary connection |
| `GET /api/v1/models` | models known to the configured endpoints |

## Profiles

Named configuration stacks (model, prompt pack, memory policy).

| route | purpose |
|---|---|
| `GET/POST /api/v1/profiles` | list / create |
| `GET/PATCH /api/v1/profiles/{id}` | read / update |
| `POST /api/v1/profiles/{id}/rename` | rename |
| `DELETE /api/v1/profiles/{id}` | delete |

## Sessions

| route | purpose |
|---|---|
| `GET/POST /api/v1/sessions` | list / create |
| `GET/PATCH /api/v1/sessions/{id}` | read / update |
| `POST /api/v1/sessions/{id}/rename` | set the title manually |
| `POST /api/v1/sessions/{id}/retitle` | regenerate the title |
| `GET/PUT /api/v1/sessions/{id}/tags` | read / replace tags |
| `GET /api/v1/sessions/{id}/status` | run status and counters |
| `PUT /api/v1/sessions/{id}/profile` | bind a profile |
| `DELETE /api/v1/sessions/{id}` | delete (cascades runs, messages, events) |
| `GET /api/v1/sessions/{id}/messages/{mid}` | one message |
| `DELETE /api/v1/sessions/{sid}/messages/{mid}` | delete a message |
| `GET /api/v1/sessions/{sid}/attachments/{aid}` | fetch an attachment |
| `GET /api/v1/sessions/{id}/events` | the event ledger (SSE-capable) |
| `GET /api/v1/sessions/{id}/runs` | the run tree |
| `POST /api/v1/sessions/{id}/rewind` | move the active leaf back |
| `POST /api/v1/sessions/{id}/select` | pick a branch leaf |
| `POST /api/v1/sessions/{id}/search` | full-text session search |
| `GET /api/v1/sessions/{id}/group-rounds` | group-chat rounds |
| `GET /api/v1/sessions/{id}/md-log` | the session's markdown log |
| `POST /api/v1/sessions/{id}/commands` | submit a command-plane request |

## Context management

| route | purpose |
|---|---|
| `GET /api/v1/sessions/{id}/context` | the current context budget breakdown |
| `POST /api/v1/sessions/{id}/context/compact` | microcompact now |
| `POST /api/v1/sessions/{id}/context/recompress` | recompress the summary |
| `GET /api/v1/sessions/{id}/context/decision` | the last compaction decision |

## Runs

| route | purpose |
|---|---|
| `POST /api/v1/runs/{id}/commands` | steer / cancel an in-flight run |

## Approvals

| route | purpose |
|---|---|
| `GET/POST /api/v1/approvals` | pending approvals / resolve one |
| `POST /api/v1/approvals/always/remove` | drop a remembered "always allow" rule |

## Prompt packs and snippets

Modular prompt storage with profile stacks and token verification.

| route | purpose |
|---|---|
| `GET/POST /api/v1/prompt-packs` | list / create |
| `GET/PATCH /api/v1/prompt-packs/{id}` | read / update |
| `DELETE /api/v1/prompt-packs/{id}` | delete |
| `GET/POST /api/v1/prompt-packs/{id}/files` | list / upsert pack files |
| `POST /api/v1/prompt-packs/{id}/files/delete` | delete a pack file |
| `GET /api/v1/prompt-packs/{id}/validate` | token-budget verification |
| `GET /api/v1/prompt-packs/{id}/snapshots` | list snapshots |
| `POST /api/v1/prompt-packs/preview` | preview a composed pack |
| `GET /api/v1/prompt-packs/snapshots/{id}` | one snapshot |
| `GET/POST /api/v1/prompt-snippets` | list / upsert snippets |
| `POST /api/v1/prompt-snippets/delete` | delete a snippet |

## Skills

| route | purpose |
|---|---|
| `GET /api/v1/skills` | list skills |
| `POST /api/v1/skills/delete` | delete |
| `POST /api/v1/skills/enable` | enable |
| `POST /api/v1/skills/trash` | move to trash |
| `POST /api/v1/skills/restore` | restore from trash |
| `POST /api/v1/skills/trash/delete` | delete from trash |

## Memory

The memory API mirrors the bank lifecycle (retain → recall → consolidate →
curate). See the memory section of [`configuration.md`](configuration.md).

| route | purpose |
|---|---|
| `GET /api/v1/memory/status` | provider, bank size, sweep state |
| `GET /api/v1/memory/bank` | the bank |
| `POST /api/v1/memory/recall` | one recall query |
| `POST /api/v1/memory/consolidate` | run a consolidation sweep |
| `GET /api/v1/memory/consolidations` | proposed consolidations |
| `POST /api/v1/memory/consolidations/{id}/approve` | approve one |
| `POST /api/v1/memory/consolidations/{id}/reject` | reject one |
| `GET /api/v1/memory/records` | raw memory records |
| `POST /api/v1/memory/records/{id}/approve` | approve a record |
| `POST /api/v1/memory/records/{id}/reject` | reject a record |
| `POST /api/v1/memory/records/{id}/supersede` | supersede a record |
| `GET /api/v1/memory/units/{id}` | one unit with lineage |
| `POST /api/v1/memory/units/{id}/core` | pin as core (always in context) |
| `POST /api/v1/memory/units/{id}/uncore` | unpin |
| `POST /api/v1/memory/units/{id}/invalidate` | invalidate |
| `POST /api/v1/memory/units/{id}/reactivate` | reactivate |
| `POST /api/v1/memory/units/{id}/supersede` | supersede |
| `DELETE /api/v1/memory/units/{id}/delete` | hard delete |
| `POST /api/v1/memory/migrate` | schema migration |
| `GET /api/v1/memory/journey` | the journey (always-in-context layer) |

## Misc

| route | purpose |
|---|---|
| `GET/PUT /api/v1/settings` | read / update daemon settings |
| `GET /api/v1/fs` | workspace file listing for the attachment picker |
| `GET /api/v1/search` | global search |
| `GET /api/v1/sources` | the web-research source ledger |
