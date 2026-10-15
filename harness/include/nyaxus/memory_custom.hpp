#pragma once

#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "nyaxus/json.hpp"
#include "nyaxus/memorydb.hpp"
#include "nyaxus/qwen3.hpp"

namespace nyaxus {
namespace memory {
namespace custom {

// ── Shared helpers ───────────────────────────────────────────────────────
// Deterministic exact-anchor extraction: URLs, file paths, hashes, UUIDs,
// model names. High precision by design - the anchor stage must never
// resurrect an unrelated memory.
struct Anchor {
    std::string kind;   // url|path|hash|uuid|model
    std::string value;
};
std::vector<Anchor> extract_anchors(const std::string& text);

// Token count via the Qwen BPE tokenizer (the main model is Qwen-family, so
// this is a faithful budget measure).
int count_tokens(const qwen3::Model& model, const std::string& text);

// The controlled unit-type taxonomy (world, experience, observation,
// mental_model, procedure, lesson). Single source of truth: retain() drops
// candidates with any other type, and the daemon's direct-write paths
// (memory.save, record approval, Hindsight import) normalize through it.
bool valid_type(const std::string& type);

// ── Retain ───────────────────────────────────────────────────────────────
// One retain request (durable outbox job payload). When `direct_text` is
// non-empty the candidate is trusted as-is (approval queue / migration /
// UI). Otherwise `extract` is called with the turn content and must return
// the aux model's raw JSON candidate list (ADD-only extraction contract).
struct RetainSpec {
    std::string content;
    std::string context;
    std::string document_id;  // nyaxus-turn:<run>
    std::string session_id;
    std::string profile;
    std::string run_id;
    std::vector<std::string> tags;
    std::string direct_text;
    std::string direct_type;
    std::string store_reason;
    std::string model_id;  // e.g. "aux:Qwen3-4B" | "direct" | "import:hindsight"
    std::string prompt_revision;
    std::string generation_id;  // provenance log ref (gen:<kind>:<ref>)
    std::string prompt_id;
    std::string prompt_sha256;
};

struct RetainResult {
    int stored = 0;
    int duplicate = 0;
    int ungrounded = 0;
    int invalid = 0;
    int skipped = 0;  // LLM dedup judged the candidate already covered
    std::vector<long long> unit_ids;
};

// ADD-first: exact content-hash duplicates are rejected; near-duplicates are
// left to the consolidation sweep. When `dedup` is provided (the extract
// path), candidates are additionally batch-judged by the LLM (store/skip/
// update/merge): skip discards the candidate, update/merge store the merged
// text as a new unit and propose retiring the superseded units (approval
// gate). Any dedup failure falls back to store-all. Candidates not grounded
// in the submitted source are rejected.
RetainResult retain(MemoryDb& db, const qwen3::Model& embed_model,
                    const RetainSpec& spec,
                    const std::function<std::string(const std::string&)>& extract,
                    std::string& error,
                    const std::function<std::string(const std::string&)>& dedup = nullptr);

// Worker-side processing of one outbox retain payload - the JSON envelope
// the daemon enqueues (content/context/document_id/session_id/profile/
// run_id/tags, or direct_text/direct_type for trusted text). When
// direct_text is empty, `extract` is called with the turn content and must
// return the extractor model's raw JSON candidate list; `dedup` (optional)
// is the LLM dedup judge for those candidates.
//
// Returns true on terminal success (units stored, all candidates known
// duplicates, or nothing worth remembering); `operation_id` then carries a
// summary for the job record. Returns false with `error` set otherwise;
// `permanent` is true for a malformed payload (fail the job) and false for
// a transient problem (extractor unavailable/failed, unparseable output,
// store error - retry with backoff).
bool process_retain_payload(MemoryDb& db, const qwen3::Model& embed_model,
                            const std::string& payload_json,
                            const std::function<std::string(const std::string&)>& extract,
                            std::string& operation_id, std::string& error,
                            bool& permanent,
                            const std::function<std::string(const std::string&)>& dedup = nullptr);

// ── Recall ───────────────────────────────────────────────────────────────
struct RecallHit {
    long long unit_id = 0;
    long long version_id = 0;
    std::string text;
    std::string type;
    std::string scope;
    std::string session_id;
    std::string profile;
    std::string run_id;
    std::string store_reason;
    std::vector<std::string> signals;
    double rrf_score = 0.0;
    float rerank_score = 0.0f;
    bool reranked = false;
    int tokens = 0;
    json::Object reason;  // deterministic score breakdown (required)
};

struct RecallRequest {
    std::string query;
    std::string session_id;  // scope gate; empty = whole bank
    std::string profile;     // scope gate
    int budget_tokens = 2048;  // injected into the main model (configurable)
    int semantic_top = 50;
    int lexical_top = 50;
    int rerank_top = 50;
    int rrf_pool = 100;
    double rrf_k = 60.0;
    int per_item_cap_tokens = 512;
    // Wall-clock budget for the whole recall (0 = no deadline). Checked at
    // stage boundaries: on expiry the remaining expensive stages (the
    // cross-encoder rerank) are skipped and the result is returned partial
    // with `timed_out` set - partial results in RRF order instead of a
    // failed recall.
    int deadline_ms = 0;
    std::set<std::string> suppressed;  // bare ids / document ids / tags
    bool skip_rerank = false;
    int link_expansion_max = 5;
    double link_expansion_budget_ratio = 0.20;
    // Recall floor (Hillock's honest no-data): hits scoring below this are
    // dropped instead of priming the model with near-misses. 0 = off.
    // Scale-relative by construction: it compares against the rerank logit
    // (logit_true - logit_false, e.g. 1.0) when the reranker ran, and the
    // RRF score (e.g. 0.03) in the RRF-only fallback. The 2026-10 eval
    // fixture (memory_recall_eval.cpp) prints the null baseline for tuning.
    double recall_floor = 0.0;
};

struct RecallResult {
    bool ok = false;
    std::string error;
    std::vector<RecallHit> hits;
    bool reranker_used = false;
    int tokens_used = 0;
    int pool_size = 0;
    int suppressed_below_floor = 0;  // hits dropped by recall_floor (0 = off)
    // Budget hardening: hits skipped because they did not fit the remaining
    // token budget, and hits whose text was truncated to the per-item cap
    // (the cut is marked in the text so the model sees the bound).
    int budget_dropped = 0;
    int truncated = 0;
    // Structured timeout: the deadline expired at a stage boundary and the
    // result is partial (rerank skipped, or no results at all).
    bool timed_out = false;
};

// Bounded pipeline: exact anchors -> semantic (embedding scan) -> lexical
// (FTS5/BM25) -> entity -> temporal -> scope/state gate -> RRF merge ->
// cross-encoder rerank (top-~50; RRF-only fallback if unavailable) ->
// token budget -> required reason -> bounded 1-hop link expansion.
RecallResult recall(MemoryDb& db, const qwen3::Model& embed_model,
                    const qwen3::Model& rerank_model, const RecallRequest& req);

// ── Browse (UI bank view) ────────────────────────────────────────────────
// Paginated listing of stored units for the memory tab. `state` defaults
// to "active" (pass "" for all states); `type` and `scope` are equality
// filters (empty = all). `text` becomes an FTS5 filter ranked by BM25;
// empty text lists units newest-first. `total` is the number of units
// matching the filters before paging.
struct BrowseRequest {
    std::string state = "active";
    std::string type;
    std::string scope;
    std::string text;
    int limit = 50;
    int offset = 0;
};
struct BrowseUnit {
    long long unit_id = 0;
    std::string type, state, state_reason, scope, session_id, profile, run_id;
    std::string text;
    std::string store_reason;
    std::string model_id;
    double confidence = 0.5;
    std::string mentioned_at;
    std::string created_at, updated_at;
    bool core = false;  // pinned: injected verbatim into every prompt
};
struct BrowseResult {
    bool ok = false;
    std::string error;
    long long total = 0;
    std::vector<BrowseUnit> units;
};
BrowseResult browse(MemoryDb& db, const BrowseRequest& req);

// ── Curate (append-only) ─────────────────────────────────────────────────
bool supersede(MemoryDb& db, const qwen3::Model& embed_model, long long unit_id,
               const std::string& new_text, const std::string& reason,
               std::string& error);
bool set_state(MemoryDb& db, long long unit_id, const std::string& state,
               const std::string& reason, std::string& error);
// Hard deletion: unit + versions + every projection row.
bool erase(MemoryDb& db, long long unit_id, std::string& error);

// ── Consolidation sweep (deterministic) ──────────────────────────────────
// Targeted selection (units updated since the last sweep watermark,
// per-memory 24h cooldown), near-duplicate collapse (cosine > 0.92, same
// scope), fixpoint stop, sweep log. Append-only: older unit is marked
// superseded and linked to the survivor; content is never rewritten.
// `full_scan` ignores the watermark and scans the whole active bank - used
// by the manual trigger, which is an explicit operator request; the
// automatic triggers (recovery/idle) stay incremental. `threshold` is the
// minimum cosine for a near-duplicate pair (operator-tunable via
// memory.sweep_cosine_threshold).
//
// The sweep only *proposes*: pairs above the threshold become pending
// consolidation candidates (memory_consolidations) that the operator
// approves or rejects. Nothing is superseded until approved.
struct SweepResult {
    int selected = 0;
    int proposed = 0;
    int conflicts_proposed = 0;  // proposed of the kind=conflict proposals
    bool fixpoint = false;
    // Diagnostics for operator-facing logs/reports.
    int candidates = 0;  // active units passing the watermark filter
    int in_cooldown = 0;  // candidates skipped by the 24h per-unit cooldown
    int active_total = 0;  // active units in the bank (regardless of watermark)
    // One line per proposal, e.g. "unit 42 proposed for unit 43 (cosine 0.941)".
    std::vector<std::string> proposed_lines;
    // The closest pairs that stayed under the threshold (up to 3), so an
    // empty sweep can be explained with real numbers, e.g.
    // "unit 12 x unit 34 (cosine 0.871)".
    std::vector<std::string> closest_below;
};
SweepResult consolidate_sweep(MemoryDb& db, const qwen3::Model& embed_model,
                              const std::string& trigger, std::string& error,
                              bool full_scan = false, double threshold = 0.92);
// The approval gate: apply or discard a proposed consolidation. Approving
// supersedes the older unit (append-only) and links it to the survivor;
// rejecting records the decision so the pair is never re-proposed.
bool decide_consolidation(MemoryDb& db, long long candidate_id, bool approve,
                          std::string& error);

// ── L2 scene refresh + L3 persona (derived distillation) ─────────────────
// Layered distillation (L1 units -> L2 scenes -> L3 persona): the aux model
// integrates the bank's new units into the scene set (topic-level narrative
// blocks) and rewrites the persona (the single core document injected into
// every prompt) from the scenes. Both are derived artifacts - any LLM failure leaves the existing
// state unchanged (the L1 units are the source of truth), so these paths
// never lose a memory. The LLM callbacks return the raw model text; the
// prompts are built here (daemon owns the prompt constants, like dedup).
struct SceneDecision {
    std::string action;  // create | update | merge
    std::string name;    // slug: lowercase letters, digits, hyphens
    std::string summary;  // one line
    int heat = 1;
    std::string content;  // full markdown narrative (rewritten, not a diff)
    std::vector<std::string> retire;  // merge only: slugs absorbed into it
};
struct SceneRefreshResult {
    bool ran = false;  // the LLM was called (enough new units since watermark)
    bool changed = false;
    int new_units = 0;  // units the refresh covered
    std::vector<long long> unit_ids;  // the covered unit ids (provenance)
    int created = 0, updated = 0, merged = 0;
    int dropped = 0;  // decisions rejected by validation (never stored)
    std::vector<std::string> applied_names;  // scenes upserted this run
    std::string error;
    std::vector<std::string> lines;  // one line per applied decision (logging)
};
// Watermark: the newest scene's updated_at ("" = first refresh, whole bank
// capped). Refreshes when at least kSceneMinNewUnits (5) new units exist.
// The scene set is capped (kSceneMaxScenes = 20); decisions that would push
// past the cap are dropped, not merged by the engine (the LLM owns merges).
SceneRefreshResult refresh_scenes(
    MemoryDb& db,
    const std::function<std::string(const std::string&)>& scenes_llm,
    const std::string& generation_id, std::string& error);
struct PersonaRefreshResult {
    bool ran = false;
    bool changed = false;
    std::vector<std::string> scene_names;  // scenes fed to the model (provenance)
    std::string error;
};
// Runs when scenes exist. Rewrites the single-row persona from the scene
// set (all scenes, heat-first, content capped); a failed call keeps the
// current persona. `changed` is false when the model returned the same text.
PersonaRefreshResult refresh_persona(
    MemoryDb& db,
    const std::function<std::string(const std::string&)>& persona_llm,
    const std::string& generation_id, std::string& error);
// Prompt builders + parsers (exposed for the standalone unit test).
std::string build_scenes_user_prompt(
    const std::vector<MemoryDb::SceneRow>& scenes,
    const std::vector<MemoryDb::UnitRow>& units,
    const std::unordered_map<long long, std::string>& texts);
std::vector<SceneDecision> parse_scene_decisions(const std::string& raw);
std::string build_persona_user_prompt(
    const std::vector<MemoryDb::SceneRow>& scenes,
    const std::string& existing_persona);
// Empty string on non-JSON / missing / empty / over-cap persona.
std::string parse_persona(const std::string& raw);

// ── Status ───────────────────────────────────────────────────────────────
json::Object status(MemoryDb& db, const qwen3::Model& embed_model,
                    const qwen3::Model& rerank_model);

}  // namespace custom
}  // namespace memory
}  // namespace nyaxus
