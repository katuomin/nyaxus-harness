#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nyaxus/core.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace nyaxus {

// SHA-256 (FIPS 180-4), hex-encoded. Content addressing for attachment dedup.
std::string sha256_hex(const std::string& data);

struct StoredSession {
    std::string id;
    std::string profile = "orchestrator";
    std::string model;
    std::string title = "new conversation";
    std::string updated_at;
    std::string context_mode = "selected";
    std::string context_status = "ready";
    std::string context_summary;
    std::string context_error;
    std::string source_session_id;
    // The last message id (global ledger) the compact context covers. Turns
    // at or before it are folded into the summary and are not re-sent to the
    // provider; turns after it form the verbatim tail. 0 = never covered.
    long long context_watermark = 0;
    // The markdown file this session is mirrored to (md-log: Obsidian vault
    // link). Empty = not linked. The file must already exist; the daemon
    // appends to it and never creates one.
    std::string md_log_path;
    // The profile revision the session last adopted (0 = never pinned) and
    // the resolved snapshot it pinned, so a later run can detect and record
    // drift instead of silently adopting a changed definition.
    long long profile_revision = 0;
    std::string profile_snapshot;
    // The pack snapshot this session is pinned to (0 = never pinned) and the
    // pack revision that snapshot carries. The session keeps this document
    // for its whole life; pack edits apply to new sessions only.
    long long pack_snapshot_id = 0;
    long long pack_revision = 0;
    // Operator labels, comma-separated ("ops,urgent"). Empty = untagged.
    // The daemon exposes them as a JSON array of strings.
    std::string tags;
    // Subagents (docs/subagents.md): set when this session was spawned as a
    // worker by agent.spawn in another session; the spawner's session id.
    // Distinct from source_session_id (fork lineage, which inherits the
    // source's journey/compaction) - a spawn child inherits nothing but its
    // objective, so the fork paths must not treat it as a fork.
    std::string parent_session_id;
    // Group chat (docs/group-chat.md): the ordered speaker profile ids for a
    // group session, comma-separated. Empty = an ordinary single-profile
    // session. `profile` is the first speaker (the session's nominal profile);
    // each turn run overrides it with its own speaker.
    std::string speakers;
    // Group chat: a per-session round-budget override (0 = use the global
    // group.max_rounds setting). Validated 1-20.
    int group_rounds_override = 0;
};

struct StoredMessage {
    long long id = 0;  // daemon-assigned snowflake (monotonic in time)
    long long parent_id = 0;  // 0 = no parent (root of the conversation)
    std::string session_id;
    std::string role;
    std::string content;
    std::string run_id;
    std::string created_at;
    std::string reasoning;
    bool is_active = true;
    // Group chat: the speaker profile id for an assistant message committed
    // by a group turn run. Empty = user message or an ordinary session.
    std::string speaker;
    // A-7: the client's optimistic row id for a user message (web sends its
    // own id so snapshots/SSE replay dedupe against it). 0 = not anchored.
    long long client_id = 0;
};

struct StoredAttachment {
    long long id = 0;
    long long message_id = 0;
    std::string name;
    std::string kind;  // "text" | "image" (extensible: audio, video)
    std::string mime_type;
    long long size = 0;
    std::optional<int> width;
    std::optional<int> height;
    std::string content;  // raw text, or a base64 data URL for images
    // SHA-256 hex of the content. When a session already stores this content,
    // later rows are references: empty content plus the hash, and the readers
    // resolve the bytes from the session's canonical row.
    std::string content_hash;
};

struct StoredProfile {
    std::string id;
    std::string name;
    std::string description;
    std::string identity_prompt;  // empty = add no identity text
    std::string base_model;       // empty = inherit the provider's model
    // Name of the provider (connection) this profile routes to. Empty = the
    // default provider (the live provider.* settings).
    std::string provider;
    bool vision = false;
    // false = unset: the resolved provider's vision capability decides
    // (explicit true enables, explicit false disables regardless of provider)
    bool has_vision = false;
    bool has_temperature = false; // false = inherit the provider's temperature
    double temperature = 0.0;
    std::string created_at;
    std::string updated_at;
    // Bumped on every definition edit; runs pin the revision they started
    // with so profile edits never mutate an already-executed turn.
    long long revision = 1;
    // Prompt-pack association. prompt_pack_id empty = legacy behavior
    // (identity_prompt is the whole system text). When set, the modular
    // prompt plan is authoritative: prompt_plan_json is the ordered module-id
    // array (JSON, e.g. ["shared.agent_policy","nyaxus.core"]) resolved
    // against the pack's manifest at run time.
    std::string prompt_pack_id;
    std::string prompt_plan_json;
    // Subagents (docs/subagents.md): the profile ids this profile may spawn
    // as workers via agent.spawn, comma-separated. Empty = may not spawn.
    // Profile-scoped by design: the operator wires "who can spawn whom" once
    // here, not per tool call.
    std::string child_profile_allowlist;
};

// A user-managed collection of logical Markdown files plus a manifest.json
// that describes the modules (ids, categories, slots, requirements). Stored
// in the main store; independent of sessions, messages, and the memory bank.
struct PromptPack {
    std::string id;
    std::string name;
    std::string description;
    long long revision = 1;
    // valid | invalid | unchecked - the last validation verdict.
    std::string validation_status = "unchecked";
    // SHA-256 over all file contents (path-sorted) + manifest, so a content
    // change is detectable without re-reading every file.
    std::string fingerprint;
    std::string created_at;
    std::string updated_at;
};

struct PromptPackFile {
    std::string pack_id;
    std::string logical_path;  // "/"-separated label inside the pack, e.g. shared/agent-policy.md
    std::string content;
    std::string content_hash;  // SHA-256 hex of content
    long long revision = 1;
    std::string created_at;
    std::string updated_at;
};

// A frozen resolved [PROMPT PACK] document at one pack revision. Sessions
// reference a snapshot id instead of carrying a copy of the prompt, so many
// conversations can share one source. A session keeps its pinned snapshot
// for its whole life; edits (self-edit or operator) land in new sessions.
struct PackSnapshot {
    long long id = 0;
    std::string pack_id;
    long long revision = 0;
    std::string document;     // the resolved [PROMPT PACK] prompt
    std::string fingerprint;  // resolution fingerprint at pin time
    std::string created_at;
};

// A recorded character-state transition (the status system). Session-scoped,
// timestamped, and ordered so the daemon can surface the most recent one and
// apply a TTL. The state label is stored verbatim (the prompt constrains the
// vocabulary; the daemon does not need to know it).
struct StatusEvent {
    long long id = 0;
    std::string session_id;
    std::string profile;
    std::string state;
    std::string note;
    std::string created_at;
};

// A fetched web source (the grounded-citations ledger). Session-scoped:
// web.fetch records one row per page, the model cites it as [src:<id>], and
// web.use marks which of the session's sources the answer actually relied on.
struct Source {
    long long id = 0;
    std::string session_id;
    std::string run_id;
    std::string url;
    std::string title;
    std::string content_hash;
    long long bytes_fetched = 0;
    std::string status;  // "ok" | "error"
    std::string error;
    bool used = false;
    std::string created_at;
};

// A named model endpoint (a saved provider connection): an OpenAI-compatible
// URL plus the sampling parameters to send to it. Profiles reference a
// provider by name; the default provider is the live provider.* settings.
struct StoredProvider {
    std::string name;
    std::string endpoint;
    std::string model;
    std::string api_key;
    // Sampling parameters as a JSON object (temperature, max_tokens, top_p,
    // top_k, min_p, penalties, seed, vision, no_thinking, plus any custom
    // passthrough keys). Stored as text so new parameters need no migration.
    std::string params;
    std::string created_at;
    std::string updated_at;
};

// A durable provider-work job. The outbox is the local authoritative record
// of memory-provider writes: a provider outage leaves the job retryable, and
// the idempotency key makes a lost acknowledgement safe to re-send.
struct MemoryOutboxJob {
    long long id = 0;
    std::string idempotency_key;
    std::string provider = "hindsight";
    std::string operation;   // e.g. "retain"
    std::string payload;     // operation-specific JSON
    std::string status = "pending";  // pending | completed | failed
    int retry_count = 0;
    std::string next_attempt_at;  // ISO UTC; empty = due immediately
    std::string last_error;
    std::string provider_operation_id;
    std::string created_at;
    std::string updated_at;
};

// A durable memory record (the approved-memory queue). Status lifecycle:
//   candidate  → approved   (user accepted the proposal)
//   candidate  → rejected   (user declined the proposal)
//   approved   → superseded (a newer record replaces it)
// Records are Nyaxus-owned authority: they survive their source session and
// are the promotion target for model-proposed memories. Provenance and source
// references are stored as JSON text so new fields need no migration.
struct MemoryRecord {
    long long id = 0;
    std::string status = "candidate";  // candidate | approved | rejected | superseded
    std::string type = "fact";         // fact | preference | decision | ...
    std::string text;
    std::string scope = "session";     // session | project | profile | user
    std::string session_id;
    std::string profile;
    std::string run_id;
    std::string source_message_ids;    // JSON array of message ids
    std::string provenance;            // JSON object (model, revision, notes, ...)
    long long contradicts_id = 0;      // record this conflicts with (0 = none)
    long long superseded_by_id = 0;    // record that replaced this (0 = none)
    std::string created_at;
    std::string updated_at;
    std::string decided_at;            // set on approve/reject/supersede
};

// A durable suppression key. A key is a bare id (run id, session id) or an
// exact provider document_id / tag string. When set, recall results matching
// the key are hidden from injection and the UI. Suppression is local and
// durable: it does not depend on the provider applying any deletion, and it
// is the guard that keeps deleted/rewound/edited/regenerated sources from
// leaking through provider recall.
struct MemorySuppression {
    long long id = 0;
    std::string key;
    std::string reason;  // session_deleted | rewound_trimmed | rewound_inactive |
                         // message_deleted | edited | regenerated
    std::string created_at;
};

// SQLite-backed durable store for sessions, messages, and the event ledger.
// Replaces the earlier JSONL EventLedger.
class SqliteStore {
   public:
    explicit SqliteStore(std::filesystem::path path);
    ~SqliteStore();
    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    const std::filesystem::path& path() const;

    // ── events ────────────────────────────────────────────────────────────
    Event append_event(std::string session_id,
                       std::string run_id,
                       std::string parent_run_id,
                       std::string event_type,
                       std::map<std::string, std::string> payload = {});

    std::vector<Event> replay_events(const std::string& run_id,
                                     std::uint64_t after_sequence = 0) const;

    std::vector<Event> replay_session_events(const std::string& session_id) const;
    // Session events with a global ledger id greater than `after_row_id`, in
    // ledger order. The basis for resumable SSE replay.
    std::vector<Event> replay_session_events_after(const std::string& session_id,
                                                   std::uint64_t after_row_id) const;
    // Remove a run's streaming delta rows (reasoning.delta / text.delta).
    // Called when the run terminates and its deltas are collapsed into a
    // single coalesced event; the complete message is already persisted, so
    // the per-chunk rows (kept only for mid-stream SSE resume) are dropped.
    void delete_run_delta_events(const std::string& run_id);
    // Run ids in a session whose NEWEST run-lifecycle event is non-terminal
    // (run.accepted / run.queued / run.started) - i.e. a run that was in
    // flight when the daemon died and never reached completed/cancelled/
    // failed. Used at startup to close the orphan so client event replay
    // terminates instead of re-seeding a run that can never finish.
    std::vector<std::string> inflight_run_ids(const std::string& session_id) const;
    // The newest run-lifecycle event type for one run ("" if it has none).
    // Distinguishes a started run (salvage + close at restart) from an
    // accepted-never-started one (revive at restart).
    std::string latest_run_event_type(const std::string& session_id,
                                      const std::string& run_id) const;
    // The run.continued payload for one run ("" if it has none). A
    // continuation run (continue/retry of a failed turn) carries
    // {"failed":"<run id>"} linking it to the run it continues; the restart
    // salvage uses it to anchor a continuation's salvaged partial onto the
    // continued turn instead of the current leaf.
    std::string continued_payload(const std::string& session_id,
                                  const std::string& run_id) const;
    // The user row a run was started against (nullopt if it has none).
    // Unlike message_by_run it does not prefer the assistant row, so it
    // finds the prompt even when the run also streamed a partial - the
    // restart salvage uses it to anchor a continuation's partial onto the
    // right turn.
    std::optional<StoredMessage> user_message_by_run(const std::string& run_id) const;

    // ── sessions ──────────────────────────────────────────────────────────
    std::vector<StoredSession> list_sessions() const;
    std::optional<StoredSession> get_session(const std::string& id) const;
    // Most recent session for a profile, excluding one id (the [PREV SESSION]
    // pointer - the profile's last conversation before this one). Empty when
    // the profile has no other session.
    std::optional<StoredSession> latest_session_for_profile(
        const std::string& profile, const std::string& exclude_id) const;
    void upsert_session(const StoredSession& session);
    void delete_session(const std::string& id);
    // Returns false when the session does not exist.
    bool rename_session(const std::string& id, const std::string& title);
    // Replace the session's tags (comma-separated). Does not touch
    // updated_at: relabeling is not conversation activity, and bumping it
    // would reorder the session list.
    // Returns false when the session does not exist.
    bool set_session_tags(const std::string& id, const std::string& tags);

    // ── profiles ──────────────────────────────────────────────────────────
    std::vector<StoredProfile> list_profiles() const;
    std::optional<StoredProfile> get_profile(const std::string& id) const;
    void upsert_profile(const StoredProfile& profile);
    void delete_profile(const std::string& id);
    // Rename a profile's id and re-point every session that references it.
    // Returns false when the source profile does not exist.
    bool rename_profile(const std::string& old_id, const std::string& new_id);

    // ── prompt packs (modular prompt storage) ─────────────────────────────
    // Returns false when the pack id already exists.
    bool create_pack(const std::string& id, const std::string& name,
                     const std::string& description);
    std::vector<PromptPack> list_packs() const;
    std::optional<PromptPack> get_pack(const std::string& id) const;
    // Name/description edit; does not bump the revision (content did not change).
    void update_pack(const std::string& id, const std::string& name,
                     const std::string& description);
    // Bumped by the daemon after every file change (upsert or delete).
    void bump_pack_revision(const std::string& id);
    void set_pack_validation(const std::string& id, const std::string& status,
                             const std::string& fingerprint);
    // Returns false when the pack does not exist.
    bool delete_pack(const std::string& id);
    // Insert or replace a logical file and bump its own revision.
    void upsert_pack_file(const std::string& pack_id, const std::string& logical_path,
                          const std::string& content, const std::string& content_hash);
    std::optional<PromptPackFile> get_pack_file(const std::string& pack_id,
                                                const std::string& logical_path) const;
    // Path-sorted, so pack fingerprints are stable.
    std::vector<PromptPackFile> list_pack_files(const std::string& pack_id) const;

    // Pack snapshots: get-or-create keyed on (pack_id, revision), so the same
    // revision is stored once and shared by every session that pins it.
    // 0 on failure.
    long long get_or_create_pack_snapshot(const std::string& pack_id,
                                          long long revision,
                                          const std::string& document,
                                          const std::string& fingerprint);
    std::optional<PackSnapshot> get_pack_snapshot(long long id) const;
    std::vector<PackSnapshot> list_pack_snapshots(const std::string& pack_id) const;
    bool set_session_pack_snapshot(const std::string& session_id,
                                   long long snapshot_id,
                                   long long pack_revision);
    // Returns false when the file does not exist.
    bool delete_pack_file(const std::string& pack_id, const std::string& logical_path);
    // Deletion guard: true when any profile's prompt plan points at the pack.
    bool profile_references_pack(const std::string& pack_id) const;

    // ── status events (character-state transitions) ───────────────────────
    // Records a state change for a session (from the status.set tool). Returns
    // the new row id.
    long long add_status_event(const std::string& session_id,
                               const std::string& profile,
                               const std::string& state,
                               const std::string& note);
    // Most recent state for a session (empty when none recorded).
    std::optional<StatusEvent> latest_status_event(const std::string& session_id) const;
    // Most recent state for a profile across all sessions (the residual source
    // for the mood TTL - a fresh session can still be "a little" in that state).
    std::optional<StatusEvent> latest_status_event_for_profile(const std::string& profile) const;
    // Newest-first history for a session, capped.
    std::vector<StatusEvent> list_status_events(const std::string& session_id, long long limit) const;

    // ── sources (grounded-citations ledger) ───────────────────────────────
    // Record a fetched web source. Deduped by (session_id, url): re-fetching
    // the same page in a session updates the row instead of adding one, so a
    // page's [src:<id>] id stays stable for the session's whole life.
    long long add_source(const std::string& session_id,
                         const std::string& run_id,
                         const std::string& url,
                         const std::string& title,
                         const std::string& content_hash,
                         long long bytes_fetched,
                         const std::string& status,
                         const std::string& error);
    std::optional<Source> get_source(long long id) const;
    // Lookup by the ledger's dedupe key (session_id + url): returns the one
    // row that a successful fetch keeps (the "ok" version), for the fetch
    // path's cache-served refetch. The url must compare exactly (same
    // scheme/host/trailing slash) - the tool layer normalizes.
    std::optional<Source> find_source(const std::string& session_id,
                                      const std::string& url) const;
    // All of a session's sources, oldest first (the citation order the model
    // saw them in).
    std::vector<Source> list_sources(const std::string& session_id) const;
    // Mark the given sources as used by the answer (web.use). Only ids that
    // belong to `session_id` are marked, so a model cannot mark another
    // session's sources. Returns how many were marked.
    int mark_sources_used(const std::vector<long long>& ids,
                          const std::string& session_id);
    // Drop a session's ledger rows (session deletion).
    void delete_sources_for_session(const std::string& session_id);
    // Prune the sources rows left behind by removed work (a Trim rewind or
    // a message delete) - the sources table outlives the transcript rows
    // otherwise, and the web references list would show ghosts.
    void delete_sources_for_runs(const std::string& session_id,
                                 const std::vector<std::string>& run_ids);

    // ── providers (named model connections) ───────────────────────────────
    std::vector<StoredProvider> list_providers() const;
    std::optional<StoredProvider> get_provider(const std::string& name) const;
    void upsert_provider(const StoredProvider& provider);
    void delete_provider(const std::string& name);

    // ── settings ──────────────────────────────────────────────────────────
    // Typed, persisted daemon settings (the saved layer). Values are stored
    // as text and validated against the daemon's registry before use.
    std::optional<std::string> get_setting(const std::string& key) const;
    void set_setting(const std::string& key, const std::string& value);
    std::map<std::string, std::string> all_settings() const;

    // ── attachments ───────────────────────────────────────────────────────
    // Attachments belong to a message and cascade-delete with it.
    // Inserting content the session already stores keeps a reference row
    // instead of duplicating the bytes (content-addressed dedup).
    void insert_attachment(const StoredAttachment& attachment);
    // last_insert_rowid shim for the caller that writes a document row's
    // serialized file (the row id names the file deterministically).
    long long last_attachment_id();
    std::vector<StoredAttachment> attachments_for_message(long long message_id) const;
    // Fetch an attachment only when its owning message belongs to the session.
    std::optional<StoredAttachment> get_attachment(long long attachment_id,
                                                    const std::string& session_id) const;

    // ── memory outbox (durable provider-work queue) ───────────────────────
    // Insert a job. A duplicate idempotency key returns the existing job's id
    // instead of inserting a second copy.
    long long enqueue_memory_job(const std::string& idempotency_key,
                                 const std::string& provider,
                                 const std::string& operation,
                                 const std::string& payload);
    // Pending jobs whose next_attempt_at has passed, oldest first.
    std::vector<MemoryOutboxJob> due_memory_jobs(long long limit) const;
    // Fetch a single job by id regardless of status (empty when absent).
    std::optional<MemoryOutboxJob> get_memory_job(long long id) const;
    void mark_memory_job_sending(long long id);
    void mark_memory_job_completed(long long id, const std::string& provider_operation_id);
    // Retryable failure: the job stays pending, its retry counter advances,
    // and it becomes due again at `next_attempt_at` (ISO UTC).
    void retry_memory_job(long long id, const std::string& error,
                          const std::string& next_attempt_at);
    // Permanent failure: the job is terminal and operator-visible.
    void fail_memory_job(long long id, const std::string& error);
    // Job counts by status, for the memory status endpoint.
    std::map<std::string, long long> memory_job_counts() const;

    // ── memory suppression (local invalidation layer) ─────────────────────
    // Add a suppression key. Idempotent: a duplicate key returns the existing
    // row's id instead of inserting a second copy.
    long long suppress_memory_key(const std::string& key,
                                  const std::string& reason);
    // All active suppression keys (the set used to filter recall results).
    std::vector<std::string> memory_suppression_keys() const;
    // Suppression keys grouped by reason, for the status endpoint and audit.
    std::map<std::string, long long> memory_suppression_counts() const;

    // ── always-allowed tools (tiered approval, "always" scope) ────────────
    // Tools the operator approved with the "always" scope: they run without
    // the approval gate in every session and survive daemon restarts.
    // `pattern` is the serialized fingerprint (fingerprint::serialize_patterns;
    // "" = the whole tool, any arguments). Idempotent: re-allowing a tool
    // keeps the original row (the first approval's pattern wins).
    void add_always_allowed(const std::string& tool,
                            const std::string& pattern);
    // The stored rule for a tool. `present` is false when the tool has no
    // always-allow row; `pattern` empty means whole-tool.
    struct AlwaysAllowedRule {
        bool present = false;
        std::string pattern;
    };
    AlwaysAllowedRule always_allowed_rule(const std::string& tool) const;
    // True when `tool` is whole-tool always-allowed (no pattern): a direct
    // gate bypass with no arguments to match against.
    bool is_always_allowed(const std::string& tool) const;
    // Remove a tool from the set (no-op when absent).
    void remove_always_allowed(const std::string& tool);
    // The full set (tool, serialized pattern), for the approvals endpoint
    // and the settings view.
    std::vector<std::pair<std::string, std::string>> list_always_allowed() const;

    // ── skill enable state (per-skill operator toggle) ────────────────────
    // Operator enable/disable state for installed skills, keyed by skill
    // name. A skill with no row is enabled (default-on): the table only
    // records explicit operator choices, so installing a new skill needs no
    // migration. Disabled skills stay installed and visible in the UI but
    // are hidden from the model (catalog + skill tool).
    void set_skill_enabled(const std::string& name, bool enabled);
    // The stored state for a skill; true when the skill has no row.
    bool skill_enabled(const std::string& name) const;
    // The full set (name, enabled), for the skills endpoint.
    std::vector<std::pair<std::string, bool>> list_skill_enabled() const;

    // ── message-tree run-id collection (for suppression hooks) ────────────
    // Distinct non-empty run_ids of every message that is a strict descendant
    // of `ancestor_id` within the session (the branch an edit/rewind deletes).
    std::vector<std::string> run_ids_of_descendants(const std::string& session_id,
                                                    long long ancestor_id) const;
    // Distinct non-empty run_ids in the session that are NOT on the active
    // path from the root to `active_leaf_id` (the branches a non-trim rewind
    // makes inactive).
    std::vector<std::string> run_ids_off_active_path(const std::string& session_id,
                                                     long long active_leaf_id) const;

    // ── memory records (approved-memory queue) ─────────────────────────────
    // Insert a record and return its new id.
    long long add_memory_record(const MemoryRecord& record);
    // Fetch a single record by id (empty when absent).
    std::optional<MemoryRecord> get_memory_record(long long id) const;
    // List records, newest first. An empty `status` filter returns all.
    std::vector<MemoryRecord> list_memory_records(const std::string& status) const;
    // Transition a record's status (approve/reject/supersede) and stamp the
    // decision time (updated_at and decided_at both advance to now). Returns
    // false when the record does not exist.
    bool set_memory_record_status(long long id, const std::string& status);
    // Record counts by status, for the status endpoint.
    std::map<std::string, long long> memory_record_counts() const;

    // ── messages ──────────────────────────────────────────────────────────
    std::vector<StoredMessage> session_messages(const std::string& session_id) const;
    // Only the active iteration per turn; this is what the provider sees.
    std::vector<StoredMessage> active_messages(const std::string& session_id) const;
    // Append a message and return its new id.
    long long append_message(const StoredMessage& message);
    // Fetch a single message by id.
    std::optional<StoredMessage> get_message(long long id) const;
    // Latest transcript row carrying this run id (assistant preferred over
    // user). Lets regenerate recover a turn whose run object was lost to a
    // daemon restart but whose ledger rows survive.
    std::optional<StoredMessage> message_by_run(const std::string& run_id) const;
    // Rewrite a message's content. Used when the operator edits a user turn;
    // the caller truncates the active path at that message afterwards.
    void update_message_content(long long message_id, const std::string& content);
    // Re-parent a message. Used when a queued turn is re-anchored on the
    // current active leaf just before it runs.
    void set_message_parent(long long message_id, long long parent_id);
    // True when `start`'s parent chain reaches `ancestor` (i.e. `start` is a
    // descendant of `ancestor`). The walk is bounded, so a pre-existing
    // cycle cannot hang the caller.
    bool is_descendant(long long start, long long ancestor) const;
    // Delete a message and its attachments. Children are re-parented onto the
    // deleted message's parent so the branch stays connected. Used when a
    // queued turn is withdrawn before it runs.
    void delete_message(long long id);
    // Permanently delete every descendant of the checkpoint, including all
    // inactive branches and their attachments. The checkpoint itself remains.
    // message_id == 0 clears the entire session tree.
    bool trim_descendants(const std::string& session_id, long long message_id);
    // Permanently delete a message, all of its descendants (active and
    // inactive branches, attachments included), and re-anchor the active
    // branch at the message's parent (clearing it if the message was the
    // root). Sibling branches under the parent remain.
    bool delete_message_tail(const std::string& session_id, long long message_id);
    // The id of the deepest message on the current active path (0 if none).
    long long active_leaf_id(const std::string& session_id) const;
    // Mark the path from root to leaf_id as the active branch; everything else inactive.
    // keep_leaf_subtree: preserve is_active for the subtree below the new
    // leaf. The run-persist path needs it: a queued turn's user message hangs
    // off the reply being persisted (it was appended at send time, parented on
    // the then-active leaf), and the blanket deactivate would detach the
    // pending conversation tail, so the next queued send would parent off the
    // reply itself and the two queued turns become sibling branches.
    void set_active_leaf(const std::string& session_id, long long leaf_id,
                         bool keep_leaf_subtree = false);
    // Make the given message (and its latest continuation) the active leaf.
    // Used to navigate user-side branches created by edits.
    void select_message(const std::string& session_id, long long message_id);
    // Make the given run's assistant the active leaf of its branch.
    void select_iteration(const std::string& session_id, const std::string& run_id);

   private:
    bool prepare(const char* sql, sqlite3_stmt** stmt) const;
    std::uint64_t next_sequence(const std::string& run_id) const;
    // Fill dedup reference rows (empty content, hash set) with the session's
    // canonical content. The caller must hold mutex_.
    void fill_deduped_contents(const std::string& session_id,
                               std::vector<StoredAttachment>& rows) const;

    std::filesystem::path path_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};

}  // namespace nyaxus