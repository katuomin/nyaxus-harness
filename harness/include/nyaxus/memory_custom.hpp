#pragma once

#include <functional>
#include <set>
#include <string>
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
};

struct RetainResult {
    int stored = 0;
    int duplicate = 0;
    int ungrounded = 0;
    int invalid = 0;
    std::vector<long long> unit_ids;
};

// ADD-only: never mutates or deletes existing units. Dedupe by content
// hash and near-duplicate cosine (same scope). Candidates not grounded in
// the submitted source are rejected.
RetainResult retain(MemoryDb& db, const qwen3::Model& embed_model,
                    const RetainSpec& spec,
                    const std::function<std::string(const std::string&)>& extract,
                    std::string& error);

// Worker-side processing of one outbox retain payload - the JSON envelope
// the daemon enqueues (content/context/document_id/session_id/profile/
// run_id/tags, or direct_text/direct_type for trusted text). When
// direct_text is empty, `extract` is called with the turn content and must
// return the extractor model's raw JSON candidate list.
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
                            bool& permanent);

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

// ── Status ───────────────────────────────────────────────────────────────
json::Object status(MemoryDb& db, const qwen3::Model& embed_model,
                    const qwen3::Model& rerank_model);

}  // namespace custom
}  // namespace memory
}  // namespace nyaxus
