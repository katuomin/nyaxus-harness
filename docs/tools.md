# Tools

An agent run calls tools through the provider's tool-calling protocol. The
daemon dispatches each call in `Daemon::dispatch_tool`; unknown names fall
through to `memory.save`. Tools that touch the outside world (terminal, web)
run behind approval gates - see [`architecture.md`](architecture.md).

Parameters are listed as the daemon reads them.

## Execution

| tool | parameters | what it does |
|---|---|---|
| `terminal` | `command` | run a shell command in the workspace; output is reduced (head/tail) or spilled to a file |

Terminal children get an explicit environment and a read-only argv policy for
auto-approved commands.

## Memory

| tool | parameters | what it does |
|---|---|---|
| `memory.recall` | `query` | query the memory bank; results are injected as historical evidence |
| `memory.save` | `text` | record a new memory (also the dispatch fallback) |
| `memory.use` | `ids` | mark offered memories as used this turn |
| `memory.act` | `ids` | mark memories as acted on (a concrete action was taken) |

Recalled content is always wrapped as past evidence, never as a current
instruction. The daemon validates that `use`/`act` ids were actually offered.

## Web research

| tool | parameters | what it does |
|---|---|---|
| `web.search` | `query` | hit the search endpoint; results land in the per-session source ledger |
| `web.fetch` | `url`, `via`, `page` | fetch a page through the escalation chain (native TLS → Firecrawl → browser-render); `page` reads a specific page of a big result |
| `web.use` | `ids` | cite ledger sources for grounded output |

The source ledger (`GET /api/v1/sources`) is per-session and append-only;
citations reference it so claims stay grounded.

## Orchestration

| tool | parameters | what it does |
|---|---|---|
| `agent.spawn` | `profile`, `objective`, `constraints` | spawn a child run on a worker profile |
| `tasks.create` | `tasks` | create a task checklist for the run |
| `tasks.update` | `task`, `status` | update a task's status |
| `status.set` | `state` | set the run/profile status |
| `group.abstain` | - | abstain from a group-chat turn |

## Interaction

| tool | parameters | what it does |
|---|---|---|
| `ask_user_question` | `question`, `options` | pause the run and ask the user (resumes on `run.waiting_input`) |
| `quiz` | `question`, `options`, `correct`, `explanation` | emit a multiple-choice quiz to the client |
| `chat.title` | `title` | set the conversation title |

## Context

| tool | parameters | what it does |
|---|---|---|
| `session.search` | `mode`, `session_id`, `query`, `count` | full-text search across sessions |
| `spill.read` | `id`, `offset`, `limit` | read a window of a spilled tool output |
| `prompt.edit` | `path`, `content`, `reason` | edit a prompt-pack file in place |
| `skill` | `name` | load a skill's instructions |
