// daemon.hpp - private header for the daemon executable's split translation
// units (daemon_config.cpp, daemon_http_util.cpp, daemon_summary.cpp,
// daemon.cpp). Not part of nyaxus_core: it declares the daemon's own Config,
// the free helpers the TUs share, and (from the summary-pipeline split on)
// the Daemon class itself.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nyaxus/argvpolicy.hpp"
#include "nyaxus/core.hpp"
#include "nyaxus/http.hpp"
#include "nyaxus/json.hpp"
#include "nyaxus/log.hpp"
#include "nyaxus/memorydb.hpp"
#include "nyaxus/promptpacks.hpp"
#include "nyaxus/provider.hpp"
#include "nyaxus/qwen3.hpp"
#include "nyaxus/skills.hpp"
#include "nyaxus/store.hpp"
#include "nyaxus/summary.hpp"

#if defined(_WIN32)
// find_unix_shell / terminal_shell_path are in-class definitions in the
// Daemon declaration below and use the Win32 file/env APIs.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nyaxus {

// ── path/OS utils ───────────────────────────────────────────────────────────

// Directory containing the running executable ("" if it cannot be
// determined). Anchors bare model filenames to the daemon install so the same
// config works no matter which directory the daemon is launched from.
std::string exe_directory();
bool file_exists(const std::string& path);
// Resolve a configured model path to an existing file (absolute,
// CWD-relative, or bare filename under the install's models dir). Returns the
// resolved path, or "" with `error` set.
std::string resolve_model_path(const std::string& configured,
                               std::string& error);
// Fixed-width ISO-UTC (same shape as the store's now_utc) so the outbox's
// lexicographic due-date comparison stays correct.
std::string utc_timestamp(std::chrono::system_clock::time_point when);
// A human UTC stamp for a turn, from the ledger's created_at (epoch
// nanoseconds, microseconds, milliseconds, seconds, or ISO-8601). Returns ""
// when the value is not a recognizable timestamp.
std::string turn_stamp(const std::string& created_at);

// ── config ──────────────────────────────────────────────────────────────────

struct Config {
    int port = 8787;
    std::string workspace = ".";
    std::string password;              // empty = no auth (prototype)
    std::string provider_type = "fake";  // "fake" or "openai"
    provider::Config provider;
    // Name of the connection that serves as the default provider (the
    // provider.* source). Empty = use the live provider.* settings.
    std::string default_provider;
    std::optional<provider::Config> auxiliary;
    bool summary_include_reasoning = false;
    // 0 = derive the source-document budget from the auxiliary context window.
    long long summary_source_budget_bytes = 0;
    // Hard cap on a single tool command. A hung command must not be able to
    // wedge its run (and therefore its session's delete) forever.
    long long tool_timeout_ms = 60000;
    // ^ Terminal tool call budget, ms. 0 = no timeout: the shell tree runs
    // until the command exits or the operator stops the run (the same
    // cancellation the approval gate uses).
    // Memory provider (retain outbox + recall). Disabled by default; the
    // worker thread polls the outbox only while enabled.
    bool memory_enabled = false;
    // "hindsight" (external service) or "custom" (in-process Qwen3 engine +
    // local memory.db). Custom needs no endpoint - only the model paths.
    std::string memory_provider = "hindsight";
    std::string memory_endpoint;
    std::string memory_bank = "nyaxus";
    std::string memory_api_key;
    // Custom provider artifacts (paths to .nyx models; empty = not loaded).
    std::string memory_db_path = "memory.db";
    std::string memory_embed_model;
    std::string memory_rerank_model;
    // OpenAI-compatible endpoint for custom-retain memory extraction. When
    // set it is preferred over the auxiliary provider; empty = fall back to
    // auxiliary (the historical behavior).
    std::string memory_extract_endpoint;
    std::string memory_extract_model;
    std::string memory_extract_api_key;
    // Optional session-title model (.nyx, task=generation). Empty = use the
    // legacy truncated first-line title.
    std::string title_model;
    // Explicit on/off for title generation (the path alone cannot say "off"
    // without looking like "not configured").
    bool title_enabled = true;
    // Cross-encoder rerank fan-out for custom recall. Every candidate is a
    // full forward pass, so keep this small (the /memory/recall route is
    // expected to answer within ~15 s).
    long long memory_rerank_top = 8;
    // Retain every Nth completed turn (1 = every turn).
    long long memory_retain_every_n_turns = 1;
    // Idle debounce for the consolidation sweep: once the retain outbox has
    // been quiet for this long, the worker enqueues one sweep (settled sweep
    // decision: idle/switch/recovery/manual triggers, never a turn/byte
    // threshold).
    long long memory_sweep_idle_seconds = 30;
    // Minimum cosine for the sweep to call a pair a near-duplicate.
    // Deliberately high: 0.92 means "the same fact reworded", not "same
    // topic". Operator-tunable (lower it to consolidate more aggressively).
    double memory_sweep_cosine_threshold = 0.92;
    // Recall session-scoped memory into the run's context (slice 3). Off by
    // default; a provider outage degrades to the local lexical fallback.
    bool memory_recall_on_turn = false;
    // Custom-recall floor (Hillock's honest no-data): hits scoring below
    // this are dropped from recall results and injections; a result that is
    // fully suppressed collapses into an explicit "no stored memory matched"
    // line. 0 = off (default). Scale: rerank logit when the reranker is
    // active (the 2026-10 eval fixture puts the null baseline at ~0 and true
    // matches at >= 2; 1.0 leaves margin on both sides), RRF score in the
    // RRF-only fallback (use ~0.03 there).
    double memory_recall_floor = 0.0;
    // Read-only argv auto-approve (docs/readonly-argv.md): terminal calls
    // proven read-only by argvpolicy skip the approval gate. On by default
    // (the classifier is fail-closed); the operator kill-switch.
    bool terminal_readonly_auto_approve = true;
    // Spill locator: the terminal's dropped middle is
    // persisted verbatim in the session's spill store and retrievable
    // in-band with spill.read. On by default; operator kill-switch.
    bool terminal_spill_full_output = true;
    // Tier-0 microcompact (docs/compaction-invariants.md): the LLM-free pass
    // that clears old in-run tool-result bodies before spending the aux
    // model on a full compaction. On by default; operator kill-switch.
    bool context_microcompact = true;
    // Stall watchdog for run provider streams: max silence (s) tolerated
    // before the first token and between tokens. 0 = the legacy 300 s cap
    // (provider::kLegacyStallTimeoutMs, resolved at the daemon layer). A
    // stall is a provider failure: the stream is retried once when nothing
    // was delivered, and fails (partial text persisted) when content
    // already streamed.
    long long run_stall_timeout_s = provider::kDefaultStallTimeoutMs / 1000;
    // Approval gate TTL (docs/decisions/deny-guards.md): a pending gate
    // decision is a decision, not a pause. The TTL is the grace period
    // before the timeout decides for the operator: write gates auto-deny
    // (fail-closed - a dead client cannot strand a state change), input
    // gates resolve as "no answer" (the model learns and moves on). 0 =
    // the legacy 600 s cap.
    long long approval_ttl_s = 300;
    // agent.spawn wall-clock deadline (docs/subagents.md): the backstop that
    // cancels a worker that never reaches a terminal state. 0 = no deadline
    // (bounded only by the parent's cancellation). Default 600 s (10 min).
    long long spawn_deadline_s = 600;
    // Group chat (docs/group-chat.md): the round budget for a group run -
    // one round is one turn per speaker. The safety cap alongside the user
    // stop and unanimous-abstention closure. Default 3.
    long long group_max_rounds = 3;
    // Terminal sudo escalation (docs/sudo-gate.md): the "for x minutes"
    // scope on the sudo password gate. 1-120.
    int terminal_sudo_ttl_minutes = 15;
    // Comma-separated names of NYAXUS_* variables the terminal's child
    // environment may see. The shell tree normally inherits everything EXCEPT
    // the secret-shaped names (NYAXUS_*API_KEY, NYAXUS_*TOKEN,
    // NYAXUS_PASSWORD, the memory bank/DB paths) - a terminal command is
    // arbitrary model-authored code and holds a process environment the
    // approve gate never reviews. Names listed here are explicitly re-shared
    // (terminal.child_env). Empty = none.
    std::string terminal_child_env;
    // Web research (web.search / web.fetch / web.use). Enabled by default:
    // the daemon already runs with a terminal tool, so outbound reads are not
    // a new capability class - the tools add structure (text extraction,
    // bounded output, a durable source ledger), not new egress. A failed
    // fetch reports the error to the model; it never wedges the run.
    bool web_enabled = true;
    // SearXNG instance serving the JSON API (web.search). Empty = web.search
    // reports "no search endpoint configured" instead of guessing.
    std::string web_search_endpoint = "http://<deployment-lan>:8080";
    // Per-fetch transport timeout (connect + read).
    long long web_fetch_timeout_ms = 15000;
    // Cap on the raw response body before text extraction (0 = no cap).
    long long web_fetch_max_bytes = 1024 * 1024;
    // Cap on the extracted text returned to the model (0 = no cap). Bounds
    // the context a single page can burn.
    long long web_fetch_max_chars = 12000;
    // Firecrawl instance for bot-walled/JS-heavy pages the native TLS fetch
    // cannot read (403/429/challenge markers). web.search also falls back to
    // its /v1/search when SearXNG errors or returns nothing. Empty = that
    // fallback tier is off.
    std::string web_firecrawl_endpoint = "http://<deployment-lan>:3002";
    // CA bundle override for the native TLS transport (empty = auto-detect:
    // system bundle on Linux, else the vendored Mozilla set). A pin is
    // useful behind a corporate proxy or for test fixtures with their own
    // trust root.
    std::string web_ca_bundle;
    // Camofox REST server (default port 9377) for the tier-2 fetch:
    // real-Camoufox rendering of pages Firecrawl cannot read. Empty = tier
    // off; Firecrawl remains the primary escalation.
    std::string web_camofox_endpoint;
    // Bearer token for the Camofox server (CAMOFOX_ACCESS_KEY); empty = the
    // server runs without route auth.
    std::string web_camofox_token;
    // Sticky camofox session (web.camofox_sticky): keep the browser tab (and
    // the per-session userId profile: cookies, logins) alive across fetches
    // instead of open/snapshot/close per call. Off by default.
    bool web_camofox_sticky = false;
    // Idle-reaper TTL for sticky tabs: a tab the session has not used for
    // this many minutes is closed at the next sticky fetch's entry. 0 reaps
    // every idle tab at each entry (tests; aggressive ops opt-in).
    long long web_camofox_sticky_ttl_minutes = 15;
    // Observatory container scoping (docs/infra-observability.md): a
    // comma-separated allowlist of container names; empty = all containers
    // on the host. Lets one daemon watch a subset (e.g. just the llama
    // boxes) on a shared host.
    std::string infra_container_filter;
    // Test-only: once the server is listening, abort() after a short delay so
    // the crash-breadcrumb handler (crash_dump) can be exercised end to end
    // (harness/tests/crash_handler_test.sh). Not a runtime setting.
    bool selftest_crash = false;
    // Setting provenance: keys explicitly provided at startup, so the saved
    // layer never overrides them (precedence: CLI > env > saved > default).
    std::set<std::string> cli_sources;
    std::set<std::string> env_sources;
};

// CLI-arg + environment config (precedence: CLI > environment > saved
// settings > defaults; the saved layer is applied later by the daemon).
Config load_config();

// ── typed settings registry ─────────────────────────────────────────────────
// Operator-facing daemon settings. Precedence: CLI > environment > saved
// settings > defaults. `restart_required` settings are persisted and applied
// at the next startup; the rest are applied to the live config immediately.

struct SettingSpec {
    const char* key;
    const char* type;  // "string" | "integer" | "number" | "boolean"
    bool secret = false;
    bool restart_required = false;
    std::function<std::string(const Config&)> read;
    std::function<bool(const std::string&, std::string&)> validate;
    std::function<void(Config&, const std::string&)> apply;
};

void ensure_auxiliary_config(Config& config);
const std::vector<SettingSpec>& settings_registry();
const SettingSpec* find_setting(const std::string& key);

// ── HTTP helpers ────────────────────────────────────────────────────────────

std::string bearer_token(const http::Request& request);
bool decode_base64(const std::string& encoded, std::string& decoded);
bool supported_image_mime(const std::string& mime_type);
std::string url_encode(const std::string& value);
std::string html_title(const std::string& html);
std::string html_to_text(const std::string& html, long long max_chars);
bool parse_image_data_url(const std::string& data_url,
                          std::string& mime_type,
                          std::string& encoded_payload,
                          std::string* decoded_bytes = nullptr);

// Shared string munging (used by the helpers above and by route handlers).
inline std::string lower_ascii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}
inline std::string trim_ascii(std::string value) {
    const auto whitespace = [](char character) {
        return character == ' ' || character == '\t' || character == '\r' ||
               character == '\n';
    };
    while (!value.empty() && whitespace(value.front())) value.erase(value.begin());
    while (!value.empty() && whitespace(value.back())) value.pop_back();
    return value;
}

// ── tuning constants + prompt policy text ───────────────────────────────────

constexpr std::size_t kMaxAttachments = 9;
constexpr long long kMaxTextAttachmentBytes = 200LL * 1024;
constexpr long long kMaxImageEncodedBytes = 4LL * 1024 * 1024;
constexpr long long kMaxAttachmentPayloadBytes = 8LL * 1024 * 1024;
// Verbatim tail of the source conversation, injected with the compact
// context so the primary model always sees the most recent user intent.
constexpr std::size_t kMaxLatestUserRequestBytes = 2000;

// Deterministic fallback: the auxiliary path is unavailable (provider down,
// unconfigured, or its output rejected), so the compact context is built
// mechanically from the source transcript. The tail budget stays far below
// summary::kMaxDocumentBytes even after JSON escaping, and the turn count
// stays below summary::kMaxItemsPerField.
constexpr std::size_t kFallbackTailBytes = 16 * 1024;
constexpr std::size_t kFallbackMaxTurns = 32;

// Re-compression cooldown: below this tail the auxiliary model is not
// bothered and the existing checkpoint is reused untouched. Decided by
// content delta (active-path turns after the coverage watermark), never by
// clock time.
constexpr std::size_t kRecompressMinTailMessages = 4;
constexpr std::size_t kRecompressMinTailBytes = 8 * 1024;

// Context budget guard: before each provider call the serialized request is
// token-estimated and, if it would overflow the provider window, the oldest
// turns are compacted into the session checkpoint (the beginning is
// summarized, never trimmed) so the request fits. The most recent tail is
// kept verbatim.
constexpr double kContextBudgetTailRatio = 0.40;  // the tail keeps 40% of the window
constexpr long long kContextBudgetMinTailTokens = 4096;
constexpr long long kContextBudgetSafetyTokens = 1024;  // chat template overhead
constexpr long long kContextBudgetMinResponseTokens = 2048;  // room for the reply
constexpr int kContextBudgetMaxCompactions = 2;  // per run; after that the provider 400 is honest
// Auto-compact circuit breaker: after this many consecutive budget
// compaction failures the guard stops trying and lets the provider's own 400
// be the honest failure - a compaction storm is the failure mode, not one
// more doomed summary call. A failure is a compaction that produced no
// checkpoint (chain too short, no usable split, empty source); the
// deterministic fallback is a success, not a failure - it produces a working
// mechanical checkpoint - and a cancelled run is not a failure either. A
// successful compaction resets the streak.
constexpr int kCompactionFailureBreaker = 3;
// Tier-0 microcompact (docs/compaction-invariants.md): when an in-run
// request is over its context budget, clear OLD tool-result bodies oldest-
// first, just until the request fits again - no fixed keep target (the old
// 5-newest floor covered barely one tool batch, so the model "forgot what
// it ran two turns ago"), only an emergency bound past which clearing
// stops even when the budget is still exceeded (the aux model takes over).
// Cleared bodies keep a head snippet so the command that produced each
// stays identifiable; the full output remains recoverable in-band via the
// spill locator where one exists.
constexpr std::size_t kMicrocompactEmergencyKeep = 2;
constexpr std::size_t kMicrocompactSnippetBytes = 512;
constexpr long long kMicrocompactMinSavingsTokens = 2048;
// Tool-calling iterations per run. The final iteration drops the tools and
// nudges the model for a final response, so a run that exhausts its budget
// ends with a progress summary instead of stopping mid-task.
constexpr int kMaxToolIterations = 20;

// Prompt-cache tripwire (docs/decisions/kv-cache-miss-tripwire.md): a
// provider call whose prompt is at least this long and whose usage reports
// zero cached tokens re-prefilled the whole prefix. 1024 matches the
// smallest prefix OpenAI-style caches will store; below it a miss is
// expected, not an alarm.
constexpr long long kKvCacheMissMinPromptTokens = 1024;

// Consecutive-denial circuit breaker: repeated gate denials
// starve the loop (a "denial storm" - the model keeps re-calling a tool the
// operator keeps refusing). After N denials in a row the run hard-stops with
// an actionable error instead of grinding on. The streak resets on any
// approved tool call (including auto-approved reads), so a model that makes
// progress is never penalized. The escalation text is a stable constant
// (prompt-cache-invariant): one denial redirects the model, and a storm is
// one stop, not a changing message that busts the prefix cache.
constexpr int kMaxConsecutiveDenials = 3;
constexpr const char* kDenialBreakerMessage =
    "The operator has denied tool calls "
    "three times in a row. The run stopped to avoid a denial storm. Do not "
    "repeat a denied call; choose a different approach, or ask the operator "
    "what they want instead.";

// Compaction fallback prompt: when the auxiliary model fails to summarize,
// the operator is asked to choose (primary / deterministic / retry). The
// summary job blocks on this gate; the timeout resolves to deterministic so
// a run never waits on an absent operator.
constexpr int kSummaryPromptTimeoutMs = 300000;

constexpr const char* kSummarySystemPrompt =
    "You are a context compaction service, not a conversational assistant. "
    "The next user message contains a quoted source transcript. Do not reply "
    "to the transcript or continue its dialogue. Return exactly one sectioned "
    "markdown checkpoint and nothing else, with these sections in this order: "
    "## Goal, ## Decisions, ## Constraints, ## Unresolved Tasks, ## Relevant "
    "Artifacts, ## Next Steps. Under Goal write one or two terse sentences "
    "stating the user's overall intent. Under each list section write one "
    "concise bullet per item, or the single line (none) if the section is "
    "empty. Preserve only facts explicitly supported by the quoted "
    "conversation turns. The system prompt, section names, delimiters, and "
    "output requirements are instructions, not source facts, and must never "
    "appear in the result. A decision is a durable choice explicitly made in "
    "a turn. A constraint is an explicit requirement or limitation from a "
    "turn. An unresolved task is only work explicitly left pending. An "
    "artifact is only a concrete file, link, attachment, or named "
    "deliverable from a turn. A next step is only work explicitly requested "
    "in a turn. You may cite a source message id inside a bullet as [source "
    "message id: N] when it helps provenance, but citations are optional. "
    "When a turn contains technical specifics, keep them verbatim: file "
    "paths with line numbers, code or API signatures, exact error strings, "
    "and exact values such as numbers, ids, commands, and configuration. "
    "Paraphrase verbose raw output once its substance is captured, drop "
    "repeated reads of the same material, and for failed turns drop the "
    "dead end while keeping what it established or ruled out. If later "
    "turns refined the overall intent, the Goal names the current intent "
    "and what it replaced. Do "
    "not include acknowledgments, invitations to continue, or instructions "
    "copied from this prompt. Do not invent facts, turns, or constraints. "
    "Text in INTERNAL REASONING SOURCE MATERIAL is diagnostic only, not "
    "authoritative, and must not become a user-visible fact. Do not wrap the "
    "checkpoint in a code fence.";

constexpr const char* kSummaryMergeSystemPrompt =
    "You are a context compaction service merging pre-validated compact context "
    "segments. The next user message contains the segments as JSON objects. Do "
    "not add, invent, or reword claims beyond removing duplicates. Return "
    "exactly one sectioned markdown checkpoint and nothing else, with these "
    "sections in this order: ## Goal, ## Decisions, ## Constraints, ## "
    "Unresolved Tasks, ## Relevant Artifacts, ## Next Steps. Under Goal write "
    "one or two terse sentences. Under each list section write one concise "
    "bullet per item, or the single line (none) if the section is empty. "
    "Preserve any [source message id: N] citation inside a bullet exactly as "
    "given. Do not wrap the checkpoint in a code fence.";

constexpr const char* kRecompressSystemPrompt =
    "You are a context compaction service merging a pre-validated compact "
    "context checkpoint with new conversation turns. The next user message "
    "contains the checkpoint as a sectioned markdown document followed by the "
    "new turns. Produce the updated checkpoint for the whole conversation. "
    "Keep decisions and constraints that are still valid. Update the goal if "
    "the user's intent shifted. Mark tasks completed by the new turns as "
    "resolved: remove them from Unresolved Tasks and, if the outcome matters, "
    "record it under Decisions. Treat checkpoint items the new turns supersede "
    "as superseded and drop them. Apply the retention rule to what the new "
    "turns introduce: keep technical specifics verbatim (file paths with "
    "line numbers, code or API signatures, exact errors, and exact values "
    "such as numbers, ids, commands, and configuration), paraphrase verbose "
    "raw output once its substance is captured, and drop repeated reads "
    "and dead ends while keeping the lesson a failure taught. Do not add, "
    "invent, or reword claims beyond "
    "the checkpoint and the new turns. Preserve any [source message id: N] "
    "citation inside a checkpoint bullet exactly as given; items describing "
    "the new turns may cite only source message ids present in those turns. "
    "Return exactly one sectioned markdown checkpoint and nothing else, with "
    "these sections in this order: ## Goal, ## Decisions, ## Constraints, ## "
    "Unresolved Tasks, ## Relevant Artifacts, ## Next Steps. Under Goal write "
    "one or two terse sentences. Under each list section write one concise "
    "bullet per item, or the single line (none) if the section is empty. Do "
    "not wrap the checkpoint in a code fence.";

// Memory tool policy block: injected as a stable-tier system message whenever
// the memory bank is reachable (gated identically in execute_run and the
// prompt-pack preview, so the preview shows exactly what a run receives).
constexpr const char* kMemoryToolPolicy =
    "You have long-term memory. Use memory.recall to search it "
    "before answering anything that may depend on prior context "
    "or decisions, and memory.save to store durable facts "
    "(decisions, preferences, constraints, important context) "
    "that should persist across sessions. memory.recall is "
    "read-only; memory.save stores the fact directly. "
    "Store one self-contained fact per memory.save call, and "
    "prefer recalling over re-asking the user for something "
    "already remembered. When your answer relies on recalled "
    "memories, cite them inline as [mem:N] next to the claim, "
    "and call memory.use with the ids (mem:N) you actually "
    "used; when you take a concrete action based on a recalled "
    "memory, call memory.act with its id (mem:N).";

// Session search policy block: raw transcript access, the complement to
// memory. Memory holds distilled facts; this reads the verbatim past
// conversations, so exact wording, a session's head/tail, and "did this
// ever come up" checks do not depend on what retain happened to store.
// Injected unconditionally - the tool is in the base manifest, so every
// run can call it (same condition as the tool declaration).
constexpr const char* kSessionSearchPolicy =
    "You can read past conversations directly with session.search. "
    "mode=list shows every recorded session (id, title, last activity, "
    "message count); mode=head and mode=tail return the verbatim start "
    "or end of a conversation; mode=grep finds exact text inside one "
    "(leave session_id empty to search the current session). Memory "
    "stores distilled facts, so when you need the exact wording of a "
    "prior exchange, what a session began or ended with, or proof that "
    "a phrase ever appeared, search the conversation itself instead of "
    "guessing from memory.";

// chat.title guardrails (see chat_title_tool): the tool is declared only
// while the session title is provisional (fast title model / truncated
// first line) and the session is within its first few turns, or while a
// re-title is pending. Once the title is final the tool leaves the
// manifest and further calls are no-ops, so the model never carries the
// titling burden on steady-state turns.
constexpr int kTitleToolMaxTurns = 3;
constexpr const char* kTitleToolPolicy =
    "You can set the conversation title with chat.title. Call it once, "
    "when the topic is clear, with a concise descriptive title (a few "
    "words, no trailing punctuation). After the title is set the tool is "
    "gone and further calls are ignored, so do not keep thinking about it.";

// Web research policy block: injected as a stable-tier system message
// whenever the web tools are enabled (gated identically in execute_run and
// the prompt-pack preview, so the preview shows exactly what a run receives).
// The citation protocol is the grounded-citations core: claims must trace to
// a fetched, ledgered source, and unsupported claims must be labeled as
// inference rather than fact.
constexpr const char* kWebToolPolicy =
    "You can research the web. Use web.search to find candidate "
    "sources for a question, then web.fetch to read the pages that "
    "look most relevant. Rely only on what your fetched sources "
    "actually support: when you use a source, cite it inline as "
    "[src:N] next to the claim, where N is the source id the tool "
    "returned. If your sources disagree, say so and cite both. If "
    "you could not find support for a claim, mark it as your own "
    "inference rather than a fact. When your answer relies on "
    "fetched sources, call web.use with the source ids (src:N) you "
    "actually used so the session's source ledger records the "
    "provenance.";

// Detached threads are the daemon's whole availability: an exception
// escaping one fastfails the entire process (0xc0000409, e.g. an uncaught
// std::bad_alloc under memory pressure), taking every session down with it
// and leaving no log behind. Guard every background thread: log the
// exception and let that one job die instead of the daemon.
template <typename Fn>
void run_guarded(const char* name, Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        log::error(std::string("uncaught exception in ") + name + ": " + e.what());
    } catch (...) {
        log::error(std::string("uncaught unknown exception in ") + name);
    }
}

// ── Daemon ────────────────────────────────────────────────────────────────

class Daemon {
   public:

Daemon(Config config, std::shared_ptr<SqliteStore> store, std::unique_ptr<provider::Provider> provider, std::unique_ptr<provider::Provider> auxiliary_provider);

void probe_endpoints_async();

static long long now_ms();

std::string workspace_skill_root();

std::string workspace_trash_root();

std::string workspace_snippets_root();

void load_skills();

bool memory_custom_enabled();

long long enqueue_sweep_job(const std::string& trigger, const std::string& key_suffix);

void start_memory_worker();

bool ensure_custom_memory(std::string& error);

bool ensure_custom_memory_locked(std::string& error);

bool ensure_title_model();
// Caller holds title_model_mutex_. Single-lock invariant: every title path
// (estimate_request_tokens, generate_title) takes the mutex exactly once and
// uses this helper for the load-or-check part.
bool ensure_title_model_locked();

static std::string strip_ungrounded_leading_year(const std::string& message, std::string title);

static std::string strip_ungrounded_leading_count(const std::string& message, std::string title);

static bool has_non_ascii(const std::string& s);

std::string generate_title(const std::string& message);

std::string run_memory_extraction(const std::string& content);

// The extraction prompt, in one place: run_memory_extraction sends it, the
// provenance log references it (id + revision + sha256). The sha is computed
// from the same text, so it stays in sync with the prompt.
static const std::string& memory_extraction_prompt();
static std::string memory_extraction_prompt_sha256();
static constexpr const char* kMemoryExtractionPromptId = "memory-extraction";
static constexpr const char* kMemoryExtractionPromptRevision = "memory-extraction:v2";

// The LLM semantic-dedup judge: batch-compares new extract candidates against
// a recalled pool of existing units and decides store/skip/update/merge.
std::string run_memory_dedup(const std::string& user_prompt);
static const std::string& memory_dedup_prompt();
static std::string memory_dedup_prompt_sha256();
static constexpr const char* kMemoryDedupPromptId = "memory-dedup";
static constexpr const char* kMemoryDedupPromptRevision = "memory-dedup:v1";

    // L2 scene refresh + L3 persona: the same small-model endpoint as
    // extraction/dedup (structured tasks, no thinking). The prompts own the
    // contract; the daemon applies the model's decisions to the bank.
    std::string run_memory_scenes(const std::string& user_prompt);
    std::string run_memory_persona(const std::string& user_prompt);
    static const std::string& memory_scenes_prompt();
    static std::string memory_scenes_prompt_sha256();
    static constexpr const char* kMemoryScenesPromptId = "memory-scenes";
    static constexpr const char* kMemoryScenesPromptRevision = "memory-scenes:v1";
    static const std::string& memory_persona_prompt();
    static std::string memory_persona_prompt_sha256();
    static constexpr const char* kMemoryPersonaPromptId = "memory-persona";
    static constexpr const char* kMemoryPersonaPromptRevision = "memory-persona:v1";

    static constexpr std::size_t kJourneyExistingCapBytes = 8192;

    static constexpr std::size_t kJourneyTurnsCapBytes = 8192;

    static constexpr std::size_t kJourneyMaxBytes = 16384;

std::string run_journey_update(const std::string& session_id);

long long enqueue_journey_job(const std::string& session_id);

int process_custom_journey_jobs(int max_jobs = 2);

int process_custom_retain_jobs(int max_jobs = 8);

int process_custom_sweep_jobs(int max_jobs = 1);

http::Response memory_consolidate(const http::Request& request);

http::Response memory_journey_get(const http::Request& request);

http::Response memory_generations_get(const http::Request& request);

// L2 scene blocks + L3 persona (GET /memory/scenes, ?full=1 for content).
http::Response memory_scenes_get(const http::Request& request);

http::Response sources_route(const http::Request& request);

json::Array skills_json_array();

// A snapshot of the skills the model may see (operator-disabled skills
// excluded). Feeds the skill catalog and the skill tool's availability.
std::vector<skills::Skill> enabled_skill_snapshot();

http::Response skills_route(const http::Request& request);

static bool safe_skill_name(const std::string& name);

http::Response save_skill_route(const http::Request& request);

http::Response delete_skill_route(const http::Request& request);

json::Array skill_trash_array();

http::Response skill_trash_list_route(const http::Request& request);

http::Response restore_skill_route(const http::Request& request);

http::Response delete_skill_trash_route(const http::Request& request);

http::Response enable_skill_route(const http::Request& request);

json::Array prompt_snippets_json_array();

http::Response prompt_snippets_route(const http::Request& request);

http::Response save_prompt_snippet_route(const http::Request& request);

http::Response delete_prompt_snippet_route(const http::Request& request);

    std::mutex md_log_mutex_;

static std::vector<std::string> md_log_lines(const std::string& text);

static std::string md_log_callout(const std::string& kind, const std::string& text);

void md_log_append_raw(const std::string& path, const std::string& block);

void md_log_append(const std::string& sid, const std::string& block);

void md_log_backfill(const std::string& sid, const std::string& path);

http::Response md_log_route(const http::Request& request);

http::Response fs_list_route(const http::Request& request);

http::Response memory_journey_update(const http::Request& request);

json::Value settings_payload() const;

http::Response settings_error(std::string message);

http::Response get_settings(const http::Request& request);

http::Response put_settings(const http::Request& request);

http::Response check_provider(const http::Request& request);

void install(http::Server& server);

   private:

    struct Session {
        std::string id;
        std::string profile = "orchestrator";
        std::string model;
        std::string title = "new conversation";
        // Set when the opening message was too thin to title from (or a
        // manual re-title was requested); the chat.title tool is declared
        // while it holds, and the first completed run re-titles from the
        // opening exchange as the deterministic fallback. A manual rename
        // clears it. In-memory only: after a restart nothing is pending.
        bool retitle_pending = false;
        // Owner of the current title: "auto" = provisional (fast title model
        // or truncated first line), "model" = the main model set it via
        // chat.title, "manual" = user rename, "final" = loaded from the
        // ledger (final after a restart). The chat.title tool is declared
        // while the title is provisional within the first few turns, or
        // while a re-title is pending. In-memory only, like retitle_pending.
        std::string title_source = "final";
        std::string updated_at;
        std::vector<provider::ChatMessage> history;
        std::string context_mode = "selected";
        std::string context_status = "ready";
        std::string context_summary;
        std::string context_error;
        std::string source_session_id;
        // Last message id the compact context covers (see StoredSession).
        long long context_watermark = 0;
        // Compaction fallback prompt gate (see prompt_summary_fallback): set
        // when the auxiliary summary fails, resolved by
        // POST /api/v1/sessions/:id/context/decision ("primary",
        // "deterministic", or "retry"). In-memory only: the gate lives while
        // the summary job waits, never across a daemon restart. Guarded by
        // context_mutex.
        std::string context_prompt;
        std::string context_decision;
        // Auto-compact circuit breaker: consecutive budget
        // compaction failures (compactions that produced no checkpoint; the
        // deterministic fallback and a cancelled run are not failures). At
        // kCompactionFailureBreaker the budget guard stops trying and lets
        // the provider's own 400 be the honest failure, instead of a
        // compaction storm. Reset on a successful compaction. In-memory only:
        // a streak that survives a daemon restart starts fresh (the breaker
        // is a runtime tripwire, not durable state). Guarded by context_mutex.
        int consecutive_compaction_failures = 0;
        // Profile revision pinning and drift guard (see execute_run): the
        // revision and resolved snapshot this session last adopted.
        long long profile_revision = 0;
        std::string profile_snapshot;
        // Pack snapshot pinning (see execute_run): the resolved pack document
        // this session is pinned to. The session keeps it for its whole life;
        // pack edits apply to new sessions only.
        long long pack_snapshot_id = 0;
        long long pack_revision = 0;
        // md-log: the markdown file (Obsidian vault note) this session is
        // mirrored to. Empty = not linked. Guarded by context_mutex.
        std::string md_log_path;
        // Operator labels, comma-separated (see StoredSession). Set via
        // POST /api/v1/sessions/{id}/tags.
        std::string tags;
        // Set when this session was spawned as a worker (see StoredSession).
        std::string parent_session_id;
        // Group chat (docs/group-chat.md): the ordered speaker profile ids
        // for a group session, comma-separated (see StoredSession). Empty =
        // an ordinary single-profile session.
        std::string speakers;
        // Per-session round-budget override (the banner's rounds control):
        // 0 = use the global group.max_rounds setting. Read at each group
        // run's start, so changes apply from the next message.
        int group_rounds_override = 0;
        std::mutex context_mutex;
        std::condition_variable context_cv;
        CancellationSource context_cancellation;
        std::atomic<bool> deleted{false};
        // Tools the operator approved with "allow session" scope: the tool
        // name alone. A subsequent call of the same tool (with any
        // arguments) skips the gate - the operator approved the tool itself
        // for the rest of the session; a different tool always re-prompts, so
        // one approval never silently
        // covers a different command. In-memory only: it expires with the
        // session (daemon reload or a new conversation re-prompts).
        std::mutex auto_approve_mutex;
        std::set<std::string> auto_approved_tools;
        // Completed turns since the last memory retain, for the
        // memory.retain_every_n_turns throttle.
        std::atomic<long long> turns_since_retain{0};
    };

    // Group chat prompt cache (docs/group-chat.md): one instance per group
    // run, shared by every speaker turn. Two caches live here:
    // - front: per speaker, the prompt's stable tier (pack document, group
    //   framing, residual/prev-session state, policy blocks, skill catalog,
    //   core memory, compact context) - it depends only on (profile, session
    //   state), so a speaker pays construction once per run, not per turn.
    //   Invalidated wholesale when the compact-context state changes.
    // - chain: the attributed transcript walk (root → the leaf the turn
    //   extends), reused across turns by appending the just-committed turn
    //   instead of re-walking the branch from the DB.
    struct GroupPromptCache {
        // One converted transcript message: the provider-view ChatMessage in
        // its BASE role ("user" for genuine user messages - never flipped;
        // "assistant" for speaker turns, with the attributed "Name: "
        // content), plus the speaker id (empty for user messages). The
        // reader's role flip (peer turns present as user input) happens per
        // build, since it depends on who is reading.
        struct ChainEntry {
            provider::ChatMessage message;
            std::string speaker;
        };
        std::mutex mutex;
        std::map<std::string, std::vector<provider::ChatMessage>> front;
        std::string front_ctx_key;  // compact-context state the front was built against
        std::vector<ChainEntry> chain;
        long long chain_last_id = 0;  // newest message id the chain reaches
        long long chain_watermark = 0;
        bool chain_valid = false;     // false after a steer or a rebuild miss
    };

    struct Run {
        std::string id;
        std::string session_id;
        // Group chat (docs/group-chat.md): when set, this run is one speaker's
        // turn inside a group run and executes with this profile instead of
        // the session's nominal profile. Empty = ordinary run.
        std::string profile_override;
        // True for a group turn run: it executes inside the group run's held
        // session slot, so it must not re-acquire the slot (it would wait on
        // itself). Set by the group loop, read by execute_run.
        bool skip_slot = false;
        // Group chat: set by the group.abstain tool when the speaker has
        // nothing to add. The turn's tool loop breaks and the floor passes to
        // the next speaker. The group loop reads it to count abstentions.
        std::atomic<bool> abstained{false};
        long long parent_message_id = 0;  // the user message this run responds to
        // True when this run appended its own user message (a fresh turn), as
        // opposed to a regenerate that answers an existing one.
        bool appended_user_message = false;
        // True when the appended user message is anchored at an explicit
        // branch point (an edit): it must not be re-anchored on the current
        // leaf when its slot arrives, and a pre-slot cancel restores the
        // pre-edit active path instead of just dropping the message.
        bool anchored_user_message = false;
        long long pre_anchor_leaf = 0;  // active leaf before an anchored edit
        std::atomic<RunStatus> status{RunStatus::Accepted};
        // Token usage accumulated across the run's provider calls (tool
        // iterations). Per-call usage still flows through metrics.snapshot;
        // the totals are emitted once in the terminal event so clients can
        // show true per-run numbers, also after a page reload.
        long long total_prompt_tokens = 0;
        long long total_completion_tokens = 0;
        long long provider_calls = 0;
        // Prompt-cache tripwire state: cumulative cached-prompt tokens
        // (reported in the terminal event alongside the other totals), and
        // whether the previous provider call in this run was served from
        // the cache - a miss after a hit is a "regression" (the cache broke
        // mid-run), a miss from the start is just "cold".
        long long total_cached_tokens = 0;
        bool last_provider_call_cached = false;
        // Shared so a CancellationToken can alias the same flag: cancelling a
        // run (or deleting its session) then also stops the provider stream.
        std::shared_ptr<std::atomic<bool>> cancel_flag =
            std::make_shared<std::atomic<bool>>(false);
        // Steer: when set, the run ends at the next message boundary (after
        // the current generation and its tool batch finish) instead of
        // running to completion. A queued steering message then starts its
        // own run on the released slot.
        std::atomic<bool> end_at_boundary{false};
        std::mutex gate_mutex;
        std::condition_variable gate;
        bool decided = false;
        bool approved = false;
        std::string pending_tool_name;
        std::string pending_tool_args;
        std::string pending_tool_call_id;
        // The fingerprint label derived for an "always" approval (set by the
        // command handler, ledgered on approval.resolved for provenance).
        std::string pending_fingerprint;
        // The approval scope chosen on the card (once/turn/session/always);
        // the structured decision ledgeres it with the rule id.
        std::string pending_scope;
        // Turn-bound grants: tools the operator approved with
        // "turn" scope - any call of the same tool in this run skips the
        // gate. The lifetime is exactly the run: the set dies with the Run
        // object, so nothing leaks into later turns. The missing middle
        // scope between allow-once and allow-session. Guarded by gate_mutex.
        std::set<std::string> turn_approved_tools;
        // Queued→steer promotion (docs/queue-semantics.md): the
        // command handler on the ACTIVE run sets steer_live_pending to a
        // queued run's id; at this run's next message boundary the queued
        // message is delivered to the model and this run CONTINUES with it
        // in context instead of ending. 0 = none. Atomic: written while the
        // run thread waits at a gate or inside a provider call.
        std::atomic<long long> steer_live_pending{0};
        // Consumed at the boundary: the promoted user message id. This run's
        // answer then chains after it (stored parent + rebuild walk), and it
        // stays set until the run ends.
        std::atomic<long long> steered_live_message_id{0};
        // Set on the QUEUED run whose message was promoted into another
        // run's live steer: the queued run cancels, but its message is NOT
        // withdrawn (it now belongs to the promoting conversation). Read by
        // this run's own thread after its slot wait aborts; written before
        // cancel_flag flips, so the cv's re-check gives the ordering.
        std::atomic<bool> promoted_away{false};
        // Set when the steer_live command aborts a gate the run was parked
        // on (approval or input): the pending call resolves as a deny /
        // no-answer with "aborted by operator delivery" provenance, so a
        // delivery can never hang on a parked tool. Read once by the loop
        // when crafting the denial result. An abort never grants anything
        // (deny-flavored, like the TTL timeout).
        std::atomic<bool> steer_aborted{false};
        // Pending interactive input (quiz / ask_user_question): the
        // presented-option permutation (presented[i] = original[order[i]])
        // and the user's choice of a presented index (-1 = "I don't know").
        // Guarded by gate_mutex, like the approval fields above.
        std::vector<int> input_shuffled_order;
        int input_choice = -1;
        // Sudo password gate (terminal tool): open when a command failed on
        // the headless TTY check and no cached grant covers it. The answer
        // rides the run's "sudo" command (POST /runs/<id>/commands).
        // Guarded by gate_mutex; the password is consumed once and never
        // ledgered, logged, or persisted.
        bool sudo_gate = false;
        std::string sudo_password;
        std::string sudo_scope;
        // Only one terminal call may park on the sudo gate at a time: the
        // run carries a single pending-call slot, so a concurrent wave call
        // waits its turn here instead of clobbering the gate's state.
        std::mutex sudo_gate_mutex;
        // @-mentioned paths (exact spans from the composer, so they may
        // contain spaces): verified and injected as a per-turn note so the
        // model sees what exists at each referenced path.
        std::vector<std::string> mentions;
        // Command plane (docs/command-plane.md): ids of steer/cancel
        // commands whose outcome the host (this run) already wrote. A
        // re-delivered id evaluates to Skip, so idempotent retries and
        // restart replays have no double effect. Guarded by gate_mutex.
        std::set<std::string> processed_command_ids;
        // Steering into a group run (docs/group-chat.md): the group run has
        // no provider loop of its own - the LIVE SPEAKER TURN is the run
        // whose boundaries consume steers. execute_group_run parks the active
        // turn here; the steer/steer_live commands resolve it. Null between
        // turns. Guarded by gate_mutex.
        std::shared_ptr<Run> current_turn;
        // The prompt cache all speaker turns of this group run share (see
        // GroupPromptCache). Null on ordinary runs.
        std::shared_ptr<GroupPromptCache> group_cache;
        // Retry = continue (docs/stream-rules): this run resumes an
        // interrupted turn - its parent is the failed run's persisted
        // partial answer and build_history appends the [CONTINUATION] note
        // at the tail. No user message of its own.
        bool continuation = false;
        std::thread thread;
    };

    Config config_;

    std::shared_ptr<SqliteStore> store_;

    std::unique_ptr<provider::Provider> provider_;

    std::unique_ptr<provider::Provider> auxiliary_provider_;

    std::jthread memory_worker_;

    std::mutex skills_mutex_;

    std::vector<skills::Skill> skills_;

    std::mutex custom_memory_mutex_;

    std::unique_ptr<MemoryDb> custom_db_;

    qwen3::Model custom_embed_;

    qwen3::Model custom_rerank_;  // .ok == false -> RRF-only recall

    std::mutex title_model_mutex_;

    qwen3::Model title_model_;  // .ok == false -> truncated first-line title

    std::mutex sessions_mutex_;

    mutable std::mutex config_mutex_;

    std::string unix_shell_;

    bool unix_shell_resolved_ = false;

    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;

    std::mutex runs_mutex_;

    std::unordered_map<std::string, std::shared_ptr<Run>> runs_;

    struct SessionSlot {
        std::mutex mutex;
        std::condition_variable cv;
        int active = 0;
        std::deque<std::string> waiters;
    };

    std::mutex slots_mutex_;

    std::unordered_map<std::string, std::shared_ptr<SessionSlot>> slots_;

std::shared_ptr<SessionSlot> session_slot(const std::string& session_id);

    std::mutex subscribers_mutex_;

    std::unordered_map<std::string, std::set<http::SseWriter*>> subscribers_;

    std::string active_session_id_ = "session-1";

std::shared_ptr<Session> get_or_create_session(const std::string& id);

std::shared_ptr<Session> find_session(const std::string& id);

std::shared_ptr<Run> find_run(const std::string& id);

bool session_has_inflight_run(const std::string& session_id);

// ── sudo escalation (docs/sudo-gate.md) ──────────────────────────────
// The sudo password gate: a headless terminal command that sudo rejected
// for lack of a TTY parks the run on an input gate ("<command> needs the
// sudo password"); the operator's answer retries the command through an
// askpass helper, with the scope the operator chose:
//   once     - this command only (the pwfile is deleted right after),
//   ttl      - usable from any session for terminal.sudo_ttl_minutes,
//   session  - this session, until the daemon process ends.
// The secret never enters argv, the transcript, the ledger, or disk
// outside a /dev/shm 0600 file the askpass helper reads; askpass itself
// is the documented sudo path, so the real sudo binary is untouched.
struct SudoGrant {
    std::string pw_path;       // /dev/shm/nyaxus-sudo-<n>.pw (0600)
    std::string askpass_path;  // /dev/shm/nyaxus-sudo-<n>.askpass (0700)
    std::string session_id;    // empty = any session
    bool once = false;
    std::chrono::steady_clock::time_point expires_at;  // max() = no expiry
};

std::mutex sudo_mutex_;

std::optional<SudoGrant> sudo_grant_;

long long sudo_seq_ = 0;

// Installs a grant for the answered password: writes the pwfile + askpass
// helper, then caches it (invalidating any previous grant). scope is
// "once" | "ttl" | "session". Returns "" on success and fills the paths;
// non-empty = the operator-facing error.
std::string sudo_grant_install(const std::string& password,
                               const std::string& scope,
                               const std::string& sid,
                               std::string& pw_path,
                               std::string& askpass_path);

// The cached grant covering this session, else false. Lazily deletes an
// expired grant (its files included).
bool sudo_grant_match(const std::string& sid, std::string& pw_path,
                      std::string& askpass_path);

// Drops a once-use grant's files after its command finished.
void sudo_grant_use_once(const std::string& pw_path, const std::string& askpass_path);

// Clears the cache (and its files): wrong password, or teardown.
void sudo_grant_revoke();

// ── sticky camofox session (web.camofox_sticky) ──────────────────────
// The web.fetch tier-2 keeps its browser alive per session instead of
// open/snapshot/close per call: a per-session userId gives the browser
// profile durable cookies (logins survive), and tabs persist so a repeated
// fetch navigates the existing tab. Concurrency: a busy tab is never
// navigated - a fetch that arrives while another uses the tab opens its own
// (one tab per in-flight FETCH, so tool waves never fight over navigation);
// the idle reaper closes tabs unused for web.camofox_sticky_ttl minutes.
struct StickyCamofoxTab {
    std::string tab_id;
    std::string url;
    std::chrono::steady_clock::time_point last_used;
    bool busy = false;
};

std::mutex camofox_tabs_mutex_;

std::map<std::string, std::vector<StickyCamofoxTab>> camofox_tabs_;

// Closes (returns the ids of) the session's stale sticky tabs: idle past
// the TTL and not busy. The registry drops them; the caller issues the
// DELETEs against the camofox server outside the lock.
std::vector<std::string> camofox_reap_stale_tabs(const std::string& sid,
                                                 long long ttl_minutes);

// Takes the session's idle sticky tab out for use (marks it busy + touches
// last_used); "" when every tab is busy or none is registered.
std::string camofox_take_idle_tab(const std::string& sid);

// Registers a freshly opened tab as the session's busy sticky tab.
void camofox_tab_register(const std::string& sid, const std::string& tab_id,
                          const std::string& url);

// Marks the session's tab finished (idle again + last_used touched).
void camofox_tab_release(const std::string& sid, const std::string& tab_id);

// Drops one sticky tab from the registry (a tab that failed to navigate or
// failed to open; the caller deletes it server-side).
void camofox_tab_drop(const std::string& sid, const std::string& tab_id);

// Forgets the session's sticky tabs (no DELETEs): returns the dropped tab
// ids so the caller can close them server-side (a session delete).
std::vector<std::string> camofox_tabs_forget(const std::string& sid);

bool authorized(const std::string& token) const;

provider::Config provider_snapshot() const;

static void apply_provider_params(provider::Config& config, const std::string& params_json);

provider::Config resolved_default_provider() const;

provider::Config resolve_provider_config(const StoredProfile& profile) const;

static bool vision_enabled(const std::optional<StoredProfile>& profile, const provider::Config& provider_config);

std::optional<provider::Config> auxiliary_snapshot() const;

bool summary_reasoning_enabled() const;

long long tool_timeout_ms_snapshot() const;
std::string terminal_child_env_snapshot() const;

bool auxiliary_configured() const;

provider::Config summary_provider_config() const;

void publish(const Event& event);

StoredSession session_record_locked(const std::shared_ptr<Session>& session) const;

StoredSession session_record(const std::shared_ptr<Session>& session) const;

void persist_session_locked(const std::shared_ptr<Session>& session);

void persist_session(const std::shared_ptr<Session>& session);

void load_persisted_sessions();

static std::string now_iso();

http::Response health();

http::Response memory_status();

http::Response infra_report(const http::Request& request);

http::Response migrate_memory(const http::Request& request);

    struct MemoryRecallOutcome {
        std::string source;  // "hindsight" or "local"
        bool fallback = false;
        json::Array results;
        int suppressed_below_floor = 0;  // custom recall: hits dropped by the floor
        // Custom recall budget hardening (partial-result metadata): the
        // deadline expired at a stage boundary (results are partial, in RRF
        // order, rerank skipped), hits dropped by the token budget, and hits
        // truncated to the per-item cap.
        bool timed_out = false;
        int budget_dropped = 0;
        int truncated = 0;
    };

MemoryRecallOutcome memory_recall_core(const std::string& query, const std::string& session_id, const std::string& profile, long long max_tokens, long long timeout_ms);

http::Response memory_recall(const http::Request& request);

http::Response memory_bank(const http::Request& request);

static long long unit_id_from_path(const http::Request& request);

static http::Response unit_error(int status, const char* reason, const std::string& message);

MemoryDb* custom_db_or_null(std::string& error);

std::string clock_block();

    static constexpr long long kStatusTtlSeconds = 600;

bool parse_iso_utc(const std::string& text, std::time_t& out) const;

bool within_status_ttl(const std::string& created_at) const;

bool parse_session_timestamp(const std::string& text, std::time_t& out) const;

std::string prev_session_block(const std::string& profile_id, const std::string& current_sid) const;

std::string core_memory_block(std::string& error);

std::string journey_block(const std::string& sid, std::string& error);

static std::string journey_excerpt(const std::string& text);

void finalize_prev_session_journey(const std::string& profile_id, const std::string& current_sid);

void record_offered_from_outcome(const MemoryRecallOutcome& outcome, const std::string& turn_id);

void record_cited_from_text(const std::string& text, const std::string& run_id);

http::Response memory_unit_detail(const http::Request& request);

http::Response memory_unit_set_state(const http::Request& request, const std::string& state);

http::Response memory_unit_set_core(const http::Request& request, bool on);

http::Response memory_unit_supersede(const http::Request& request);

http::Response memory_unit_delete(const http::Request& request);

int suppress_memory(const std::vector<std::string>& keys, const std::string& reason, const std::string& session_id);

std::set<std::string> suppressed_memory_set() const;

static json::Object memory_record_json(const MemoryRecord& record);

http::Response list_memory_records(const http::Request& request);

http::Response create_memory_record(const http::Request& request);

http::Response decide_memory_record(const http::Request& request, const std::string& action);

http::Response list_consolidations(const http::Request& request);

http::Response decide_consolidation_route(const http::Request& request, bool approve);

http::Response login(const http::Request& request);

static void add_context_fields(json::Value& result, const StoredSession& session, bool include_summary);

bool resolve_fork_chain(const std::string& fork_from, const json::Value& path_value, std::vector<StoredMessage>& fork_chain, http::Response& response);

http::Response create_session(const http::Request& request);

long long summary_budget_bytes() const;

long long estimate_request_tokens(const std::string& serialized);

long long context_budget_tokens(const provider::Config& cfg) const;

long long mechanical_document_budget() const;

static long long microcompact_tool_results( std::vector<provider::ChatMessage>& history, std::size_t& cleared, long long deficit_tokens);

bool compact_chain_to_checkpoint(const std::shared_ptr<Session>& session, const provider::Config& provider_config, std::vector<StoredMessage> chain, long long estimated_tokens, CancellationToken cancellation, std::string& detail);

bool compact_session_for_budget(const std::shared_ptr<Session>& session, const std::shared_ptr<Run>& run, const provider::Config& provider_config, long long estimated_tokens, std::string& detail);

bool recompress_session_for_budget(const std::shared_ptr<Session>& session, const std::shared_ptr<Run>& run, std::string& detail);

bool build_source_segment(const std::vector<StoredMessage>& messages, const std::string& header_note, provider::ChatMessage& source_document, std::string& error);

bool chunk_source_chain(const std::vector<StoredMessage>& source_chain, long long budget, std::vector<std::vector<StoredMessage>>& chunks, std::string& error);

void build_summary_request(const provider::ChatMessage& source_document, bool chunked, std::vector<provider::ChatMessage>& history);

void build_merge_request(const std::vector<std::string>& segment_docs, std::vector<provider::ChatMessage>& history);

bool stream_summary(const std::shared_ptr<Session>& session, bool via_primary, const std::vector<provider::ChatMessage>& history, int chunk, int chunks, CancellationToken cancellation, std::string& out_text, std::string& error);

bool finish_context(const std::shared_ptr<Session>& session, const std::string& status, const std::string& summary, const std::string& error);

bool validated_summary_call(const std::shared_ptr<Session>& session, bool via_primary, std::vector<provider::ChatMessage> history, int chunk, int chunks, const std::set<long long>& valid_ids, const std::set<long long>& required_ids, CancellationToken cancellation, summary::Document& out_doc, std::string& error);

    enum class SummaryFallbackChoice { Primary, Deterministic, Retry };

SummaryFallbackChoice prompt_summary_fallback( const std::shared_ptr<Session>& session, const std::string& reason, CancellationToken cancellation);

bool validated_summary_chain(const std::shared_ptr<Session>& session, const std::vector<provider::ChatMessage>& history, int chunk, int chunks, const std::set<long long>& valid_ids, const std::set<long long>& required_ids, CancellationToken cancellation, summary::Document& out_doc, std::string& error);

bool deterministic_fallback_document( const std::vector<StoredMessage>& source_chain, long long doc_budget, summary::Document& out, std::string& error);

void prepare_summary(const std::shared_ptr<Session>& session, std::vector<StoredMessage> source_chain, CancellationToken cancellation);

void start_summary_job(const std::shared_ptr<Session>& session, const std::vector<StoredMessage>& source_chain);

static void collect_cited_ids(const summary::Document& doc, std::set<long long>& out);

bool build_recompress_request(const summary::Document& checkpoint, const std::vector<StoredMessage>& tail, std::vector<provider::ChatMessage>& history, std::string& error);

bool mechanical_recompress_document(const summary::Document& old_doc, const std::vector<StoredMessage>& tail, long long doc_budget, summary::Document& out, std::string& error);

bool mechanical_merge_document(const std::vector<summary::Document>& segments, long long doc_budget, summary::Document& out, std::string& error);

bool finish_recompression(const std::shared_ptr<Session>& session, const std::string& summary, long long watermark);

void prepare_recompression(const std::shared_ptr<Session>& session, std::vector<StoredMessage> tail, CancellationToken cancellation);

void start_recompression_job(const std::shared_ptr<Session>& session, std::vector<StoredMessage> tail);

void manual_recompress(const std::shared_ptr<Session>& session, http::Response& response);

http::Response recompress_context(const http::Request& request);

http::Response compact_context(const http::Request& request);

http::Response context_decision_route(const http::Request& request);

http::Response update_context(const http::Request& request);

static std::string session_id_from_path(const std::string& path);

http::Response get_session(const http::Request& request);

static std::string lowercase_copy(const std::string& in);

static std::string url_decode(const std::string& in);

static std::string query_param(const std::string& query, const std::string& key);

static std::string make_snippet(const std::string& content, std::size_t pos, std::size_t match_len);

http::Response status_events_route(const http::Request& request);

http::Response search_session(const http::Request& request);

http::Response search_all(const http::Request& request);

http::Response get_attachment(const http::Request& request);

http::Response list_sessions(const http::Request& request);

static bool starts_with(const std::string& text, const std::string& prefix);

http::Response create_run(const http::Request& request);

static json::Value profile_json(const StoredProfile& profile);

static bool prompt_plan_from_body(const json::Value& body, std::string& plan_json);

static bool child_allowlist_from_body(const json::Value& body, std::string& allowlist);

static std::string profile_fingerprint(const StoredProfile& profile);

static std::string profile_snapshot_json(const StoredProfile& profile);

static std::vector<std::string> profile_drift_fields(const std::string& previous, const StoredProfile& current);

http::Response list_profiles(const http::Request& request);

http::Response create_profile(const http::Request& request);

http::Response list_models(const http::Request& request);

static std::string profile_id_from_path(const std::string& path);

http::Response update_profile(const http::Request& request);

http::Response delete_profile(const http::Request& request);

void retag_sessions(const std::string& old_profile, const std::string& new_profile);

static bool valid_profile_slug(const std::string& id);

static std::string pack_id_from_path(const std::string& path);

static json::Value pack_json(const PromptPack& pack);

static json::Value pack_file_json(const PromptPackFile& file);

static json::Value report_json(const promptpacks::Report& report);

static std::map<std::string, std::string> pack_files_map( const std::vector<PromptPackFile>& files);

promptpacks::Report validate_pack_now(const std::string& pack_id);

http::Response list_prompt_packs(const http::Request& request);

http::Response create_prompt_pack(const http::Request& request);

http::Response get_prompt_pack(const http::Request& request);

http::Response update_prompt_pack(const http::Request& request);

http::Response upsert_pack_file_route(const http::Request& request);

http::Response delete_pack_file_route(const http::Request& request);

http::Response delete_prompt_pack_route(const http::Request& request);

static json::Value pack_snapshot_json(const PackSnapshot& snapshot, bool include_document);

http::Response list_pack_snapshots_route(const http::Request& request);

http::Response get_pack_snapshot_route(const http::Request& request);

http::Response validate_prompt_pack_route(const http::Request& request);

http::Response preview_prompt_pack(const http::Request& request);

http::Response rename_profile_route(const http::Request& request);

static std::string provider_name_from_path(const std::string& path);

static json::Value provider_json(const StoredProvider& provider);

http::Response list_providers(const http::Request& request);

http::Response upsert_provider(const http::Request& request);

http::Response delete_provider_route(const http::Request& request);

http::Response check_provider_route(const http::Request& request);

http::Response select_profile(const http::Request& request);

http::Response delete_session_route(const http::Request& request);

http::Response rename_session_route(const http::Request& request);

http::Response set_session_tags_route(const http::Request& request);

http::Response set_session_group_rounds_route(const http::Request& request);

http::Response set_session_speakers_route(const http::Request& request);

http::Response retitle_session_route(const http::Request& request);

http::Response select_iteration(const http::Request& request);

void sync_history(const std::shared_ptr<Session>& session);

bool wait_for_context(const std::shared_ptr<Session>& session, const std::shared_ptr<Run>& run, std::string& error);

http::Response rewind_session(const http::Request& request);

http::Response delete_message_route(const http::Request& request);

http::Response edit_message(const http::Request& request);

http::Response run_command(const http::Request& request);

http::Response approvals_route(const http::Request& request);

http::Response approval_always_remove_route(const http::Request& request);

// SESSION-scoped steering (docs/queue-semantics.md): the web's steer /
// deliver-now / send-now buttons no longer guess at a run id - the client
// posts to /sessions/<sid>/commands and the daemon resolves the run that
// holds the session slot (a plain Running run, or - in a group - the group
// run, whose live-turn propagation applies the boundary flags). The run-
// scoped commands keep their semantics; this only removes the stale-client
// tracking from the loop.
void apply_steer_flag(const std::shared_ptr<Run>& run);
http::Response apply_deliver_now(http::Response response,
                                 const std::shared_ptr<Run>& run,
                                 const std::string& queued_run_id);
http::Response session_command_route(const http::Request& request);

http::Response regenerate_run(http::Response response, const std::shared_ptr<Run>& target);

// Retry = CONTINUE (docs/stream-rules): the operator's retry on a failed
// run resumes the interrupted turn instead of regenerating it. The run's
// partial answer (persisted by persist_partial_assistant on the failure
// path) is the continuation's parent - the transcript keeps it, and the
// reply finishes the thought after a [CONTINUATION] note. Nothing was
// persisted (the failure hit before the first payload) → falls back to
// regenerate_run(Failed run target).
http::Response continue_run(http::Response response, const std::shared_ptr<Run>& target);

// Pair a freshly-started continuation with the run it resumes: publishes
// run.continued{failed: <failed run id>} so the web renders the resumed
// turn inside the interrupted turn's block (no duplicate profile header).
// Both continue paths share this - the live-run handler and the
// restart-recovery route (whose target run no longer has a live object).
void publish_continued_pairing(http::Response& response, const std::string& session_id,
                               const std::string& failed_run_id);

http::Response start_run(http::Response response, const std::shared_ptr<Session>& session, const std::string& message, bool append_user, long long parent_message_id, std::vector<StoredAttachment> attachments = {}, bool anchored = false, std::vector<std::string> mentions = {}, bool continuation = false, long long client_message_id = 0);

#if defined(_WIN32)
static std::wstring to_wide(const std::string& s);

// Unix-shell discovery for the terminal tool: the model's natural bash
// vocabulary and unix output conventions work as-is under git-bash, so
// a /bin/sh is preferred on Windows; cmd.exe stays the last-resort
// fallback for boxes without one. Candidates: sh.exe on PATH, then the
// standard Git for Windows install locations.
static std::string find_unix_shell() {
        const auto is_file = [](const std::wstring& path) {
            const DWORD attrs = GetFileAttributesW(path.c_str());
            return attrs != INVALID_FILE_ATTRIBUTES &&
                   (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
        };
        const auto env_a = [](const char* name) -> std::string {
            const DWORD len = GetEnvironmentVariableA(name, nullptr, 0);
            if (len == 0) return std::string();
            std::vector<char> buf(len);
            GetEnvironmentVariableA(name, buf.data(), len);
            return std::string(buf.data(), static_cast<std::size_t>(len - 1));
        };
        const std::string path_env = env_a("PATH");
        std::size_t start = 0;
        while (start <= path_env.size()) {
            const std::size_t end = path_env.find(';', start);
            const std::string dir = path_env.substr(
                start,
                end == std::string::npos ? std::string::npos : end - start);
            // Absolute entries only: a bare "." or a relative PATH component
            // resolves against the DAEMON's current directory, so a stray
            // sh.exe dropped next to wherever the daemon was started would
            // silently become the model's shell. The standard install
            // locations below are the sanctioned fallback instead.
            if (!dir.empty() && (dir[0] == '\\' || dir.length() > 2 &&
                                 dir[1] == ':' && (dir[2] == '\\' ||
                                                   dir[2] == '/') ||
                                 (dir.rfind("\\\\", 0) == 0))) {
                if (is_file(to_wide(dir + "\\sh.exe"))) {
                    return dir + "\\sh.exe";
                }
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        const char* candidates[] = {
            "C:\\Program Files\\Git\\bin\\sh.exe",
            "C:\\Program Files (x86)\\Git\\bin\\sh.exe",
        };
        for (const char* candidate : candidates) {
            if (is_file(to_wide(candidate))) return candidate;
        }
        const std::string local_appdata = env_a("LOCALAPPDATA");
        if (!local_appdata.empty()) {
            const std::string per_user =
                local_appdata + "\\Programs\\Git\\bin\\sh.exe";
            if (is_file(to_wide(per_user))) return per_user;
        }
        return std::string();
    }

#endif

    // The shell that will execute the next terminal call: a unix /bin/sh if
    // one was found (git-bash on Windows, /bin/sh on POSIX), else the
    // platform default (cmd.exe on Windows). Discovery is cached.
    std::string terminal_shell_path() {
#if defined(_WIN32)
        std::lock_guard<std::mutex> lock(config_mutex_);
        if (!unix_shell_resolved_) {
            unix_shell_ = find_unix_shell();
            unix_shell_resolved_ = true;
        }
        return unix_shell_;
#else
        return std::string();
#endif
    }

argvpolicy::Shell terminal_shell_flavor();

std::string run_terminal(const std::string& unix_shell, const std::string& workspace, const std::string& command, long long timeout_ms, const CancellationToken& cancellation, const std::string& sid, const std::string& rid);

    struct ToolParam {
        const char* name;
        const char* description;
    };

    struct ToolSpec {
        const char* name;
        const char* description;
        const ToolParam* params;  // array of param_count entries
        int param_count;
        bool read_only;  // skips the approval gate
    };

static json::Value tool_definition(const ToolSpec& spec);

bool memory_available_now();

static const std::vector<ToolSpec>& tool_manifest(bool memory_available, bool web_enabled, bool title_tool, bool spawn_tool, bool group_tool = false);

bool web_enabled_now();

    struct WebConfig {
        std::string search_endpoint;
        long long fetch_timeout_ms = 15000;
        long long fetch_max_bytes = 1024 * 1024;
        long long fetch_max_chars = 12000;
        // Read-budget pager (per call, not a setting): which 1-based page of
        // the extracted text this fetch returns. Page 1 (the default) is the
        // original read; the result's page note names the next page.
        int fetch_page = 1;
        std::string firecrawl_endpoint;
        // CA bundle override for the native TLS transport (web.ca_bundle);
        // empty = the client auto-detects (system bundle on Linux, vendored
        // Mozilla set anywhere else).
        std::string ca_bundle;
        // Camofox server (Firefox-fork browser REST, port 9377) - the tier-2
        // escalation after Firecrawl fails: opens a tab, takes the a11y
        // snapshot, returns the bounded text. Bearer token when the server
        // runs with an access key; empty = no auth header.
        std::string camofox_endpoint;
        std::string camofox_token;
        // Sticky camofox session (web.camofox_sticky): the fetch tier keeps
        // one live browser per session - a per-session userId (cookies and
        // logins persist in the browser profile), tabs stay open across
        // fetches (navigated instead of open+closed), an idle reaper closes
        // them after web.camofox_sticky_ttl minutes. Off = the ephemeral
        // open/snapshot/close flow.
        bool camofox_sticky = false;
        long long camofox_sticky_ttl_minutes = 15;
    };

WebConfig web_config_snapshot();

json::Array build_run_tools(bool memory_available, bool web_enabled, bool title_tool, bool spawn_tool, bool group_tool = false);

static const ToolSpec* find_tool(const std::string& name, bool memory_available, bool web_enabled, bool title_tool, bool spawn_tool, bool group_tool = false);

static bool is_read_only_tool(const std::string& name);

static bool is_input_tool(const std::string& name);

bool is_terminal_read_only_call(const std::string& name, const std::string& arguments);

bool parse_input_args(const std::string& name, const std::string& arguments, std::string& question, std::vector<std::string>& options, std::string& error);

std::string memory_recall_tool(const std::string& query, const std::string& sid, const std::string& profile_id, const std::string& run_id);

std::string session_search_tool(const std::string& mode_text, const std::string& session_id_text, const std::string& query, const std::string& count_text, const std::string& current_sid);

// Spill locator: the terminal's dropped middle stays
// retrievable. run_terminal persists the full output verbatim in the
// session's spill store (spills/<sid>/<n>.txt) once it passes the reduce
// target, and the omission marker carries an opaque locator + exact byte
// count + in-band retrieval guidance. Lossless: on storage failure the
// marker degrades to the legacy form (no locator), never a lossy
// downgrade. Publishes `tool.spilled` for provenance.
// Read one spill slice for the spill.read tool. `locator` is opaque and
// travels in-band ("spill:<sid>:<n>"); only the CURRENT session's own
// spills are readable.
std::string spill_read_tool(const std::string& locator, const std::string& offset_text, const std::string& limit_text, const std::string& current_sid);

std::string memory_use_tool(const std::string& ids, const std::string& run_id);

std::string memory_act_tool(const std::string& ids, const std::string& run_id);

std::string memory_save_tool(const std::string& text, const std::string& type, const std::string& sid, const std::string& profile_id);

std::string status_set_tool(const std::string& state, const std::string& sid, const std::string& profile_id);

// Visible task checklist: projects the list to the event ledger
// (tasks.created / tasks.updated); the web reconstructs it from the
// stream, so it survives reloads like the rest of the ledger.
std::string tasks_create_tool(const std::string& tasks_json, const std::string& sid, const std::string& run_id);
std::string tasks_update_tool(const std::string& task, const std::string& status, const std::string& sid, const std::string& run_id);

void harvest_title_pair(const std::string& sid, const std::string& user_message, const std::string& title);

std::string chat_title_tool(const std::string& raw_title, const std::string& sid);

// agent.spawn (docs/subagents.md): spawn a worker session bound to a target
// profile (validated against the parent profile's child_profile_allowlist),
// run the objective to completion, and return the bounded narrative. Blocks
// the calling run's tool phase; the child runs the ordinary pipeline in its
// own session slot, so N spawns run concurrently.
std::string agent_spawn_tool(const std::string& profile_id, const std::string& objective, const std::string& constraints, const std::string& sid, const std::string& run_id, const CancellationToken& cancellation);

// Group chat (docs/group-chat.md): the recorded abstention - sets
// run->abstained, ledgeres a group.abstained event for the speaker, and
// tells the model to end the turn.
std::string group_abstain_tool(const std::string& sid, const std::string& run_id, const std::string& profile_id);

std::string prompt_edit_tool(const std::string& path, const std::string& content, const std::string& reason, const std::string& sid, const std::string& profile_id, const std::string& run_id);

    struct WebSearchResult {
        std::string url;
        std::string title;
        std::string snippet;
    };

static std::string render_search_results( const std::string& query, const std::vector<WebSearchResult>& results);

std::string web_search_tool(const std::string& query, const CancellationToken& cancellation);

std::string web_fetch_via_firecrawl(const std::string& url, const WebConfig& wc, const std::string& sid, const std::string& run_id);

std::string web_fetch_via_camofox(const std::string& url, const WebConfig& wc, const std::string& sid, const std::string& run_id, const std::string& tool_id);

std::map<std::string, std::string> web_fetch_headers();

// Extract + ledger + format a successful native fetch into the [src:N] block.
std::string web_fetch_finish(const std::string& url, int status, const std::string& body, const WebConfig& wc, const std::string& sid, const std::string& run_id, const std::string& via_note);

std::string web_fetch_tool(const std::string& url, const std::string& via, const WebConfig& wc, const std::string& sid, const std::string& run_id, const std::string& tool_id, const CancellationToken& cancellation);

// Cache-served refetch (web fetch resilience): on a failed live fetch, the
// session's own prior ledger row (status ok) plus the persisted extracted
// text under spills/<sid>/src-<id>.txt is returned with the original src id
// and a cache marker, instead of an error. Empty string when the session
// holds no compatible successful fetch (or the persisted copy is gone).
std::string web_fetch_cache(const std::string& url, const std::string& sid, const std::string& fail_note);

// THE failure path for every fetch tier: consult the cache BEFORE any error
// row is ledgered - the sources ledger dedupes by session+url, so an
// error-row write would clobber a prior ok row (the ok state is what makes
// the cache reachable on the NEXT failure). A cache hit returns without
// touching the ledger; a miss records the failure here.
std::string web_fetch_failure_result(const std::string& url, const std::string& sid, const std::string& run_id, const std::string& fail_note);

// Persist a successful fetch's extracted text for the cache path above.
void web_fetch_persist(const std::string& sid, long long id, const std::string& text);

std::string web_use_tool(const std::string& ids, const std::string& sid);

std::string skill_tool(const std::string& name);

std::string quiz_tool(const std::string& question, const std::string& options_text, const std::string& correct_text, const std::string& explanation, const std::string& run_id);

std::string ask_user_question_tool(const std::string& question, const std::string& options_text, const std::string& run_id);

std::string dispatch_tool(const std::string& name, const std::string& arguments, const std::string& sid, const std::string& profile_id, const std::string& run_id, const std::string& tool_id, const CancellationToken& cancellation);

void withdraw_queued_user_message(const std::shared_ptr<Run>& run, const std::shared_ptr<Session>& session);

// Queued→steer promotion boundary: deliver the queued run's user message
// to the model (pushed onto the live history), cancel the queued run
// WITHOUT withdrawing its message, re-anchor the message into this run's
// chain, and mark this run's answer to chain after it. Returns false when
// the state moved underneath (the queued run started, the message is
// gone); the run then simply continues without it.
bool promote_queued_message(const std::shared_ptr<Run>& run, const std::shared_ptr<Session>& session, long long queued_run_message_id, std::vector<provider::ChatMessage>& history);

static std::map<std::string, std::string> usage_payload(const Run& run);

void persist_partial_assistant(const std::shared_ptr<Run>& run, const std::string& sid, const std::string& rid, const std::string& assistant_text, const std::string& reasoning_text, std::size_t emitted_content = 0, std::size_t emitted_reasoning = 0);

static bool looks_like_abs_path(const std::string& path);

static std::string format_path_bytes(long long bytes);

std::string referenced_paths_note(const std::shared_ptr<Run>& run);

void execute_run(std::shared_ptr<Run> run, std::shared_ptr<Session> session);

// Stream salvage (docs/stream-rules): a run's streamed narrative outlives
// the process - the consolidated per-step text/reasoning events plus the
// in-flight step's not-yet-coalesced deltas all live in the ledger. Used by
// the startup orphan closure, where the in-memory accumulation died with
// the process, so a daemon restart no longer erases an interrupted turn
// (the salvaged partial becomes the retry's continuation anchor).
std::string salvage_run_narrative(const std::string& rid, bool reasoning);

// Restart queue revival: a prompt accepted (queued) while an earlier run was
// streaming is durable at restart, but it never started - the slot was held
// by the run that died. Revive it: re-instantiate the run from its queued
// user row, re-anchor that row onto the post-restart leaf (the salvaged
// partial), and start it against the freed slot, so the prompt does not
// silently die and the operator does not re-type it into a sibling fork.
void revive_queued_run(std::shared_ptr<Session> session, const std::string& run_id);

// Restart salvage anchor for a continuation run (continue/retry of a failed
// turn): such a run has no user row of its own, so message_by_run cannot
// anchor its salvaged partial. Follow the run.continued chain to the run that
// owns a user row and return that row's id (0 if unresolvable), so the
// partial lands on the continued turn even if a later message moved the leaf.
long long continuation_anchor(const std::string& session_id, const std::string& run_id);

// A prompt sent while a reply was streaming anchors on the current leaf (the
// run's user row, or an earlier reply when that row queued behind one). Once
// the reply lands those prompts must chain AFTER it, not sit as siblings of
// the run's user row - otherwise the transcript forks into a branch. Re-anchor
// them, in send order, onto the reply so the active path stays one chain.
void reparent_queued_prompts(const std::string& session_id, long long assistant_id, long long user_row_id);

// Group chat (docs/group-chat.md): the serialized turn loop for a group
// session (Regime 1, np=1). Runs one speaker turn at a time - each turn is
// a full execute_run with a profile override - round-robin with an @-address
// nudge, terminated by the user stop, the round budget (group.max_rounds),
// or unanimous abstention in a round. Called from execute_run for the group
// run itself (which holds the session slot for the whole loop).
void execute_group_run(std::shared_ptr<Run> run, std::shared_ptr<Session> session);

// The prompt's stable front tier: pack document (session pin for ordinary
// runs; per-speaker resolution for group turns - a pinned document was
// resolved for ANOTHER speaker's plan and name), identity fallback, the
// [GROUP] framing (group turns), residual + prev-session state, policy
// blocks, skill catalog, core memory, and the compact-context tier. Shared
// by the build_history fast path (group turns cache the result per speaker
// on the run's GroupPromptCache) and the ordinary path. Returns empty with
// *error set when the pack is missing/invalid; the caller publishes
// run.failed exactly as the inline code used to.
std::vector<provider::ChatMessage> build_prompt_front(
    const std::string& sid, const std::string& profile_id,
    const std::string& speakers, const StoredSession& context_state,
    bool group_turn, bool worker_session, bool title_now,
    const std::optional<StoredProfile>& profile, std::string* error);

// One stored message in the provider's view: thinking stripped, speaker
// turns attributed ("Name: "), attachments folded in (or rejected for
// invalid images via the out-flags). Used by build_history's chain walk and
// by the group chain-cache extension.
GroupPromptCache::ChainEntry convert_chain_message(
    const StoredMessage& message, bool* has_image, bool* invalid_image);

void enqueue_memory_retain(const std::shared_ptr<Run>& run, const std::shared_ptr<Session>& session, const std::string& sid, const std::string& rid, const std::string& assistant_text);

void enqueue_approved_record_retain(const MemoryRecord& record, long long id);

static std::string path_segment(const std::string& path, const std::string& prefix, const std::string& suffix);

static std::uint64_t parse_cursor(const std::string& text);

void stream_events(http::SseWriter& writer, const http::Request& request);
};


}  // namespace nyaxus
