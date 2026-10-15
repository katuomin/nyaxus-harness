#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace nyaxus {

// Content hash (SHA-256 hex) used for version dedupe. Exposed so the
// provider can check for duplicates before inserting.
std::string memory_content_hash(const std::string& text);

// Relational authority for the custom memory provider.
//
// This is a *separate* SQLite file (memory.db) from the Nyaxus ledger
// (nyaxus.db). The ledger owns events/sessions/messages; this store owns
// memory units, their immutable versions, source evidence, typed links,
// feedback, and the rebuildable projections (FTS5, exact anchors, entities,
// embeddings). Per the settled blueprint the authority tables migrate in
// place via PRAGMA user_version; the projections are rebuilt, not migrated.
//
// Concurrency: single writer (the daemon worker), WAL mode, busy_timeout
// ~5s; recall/UI are concurrent readers. All public methods take an
// internal mutex, so a MemoryDb instance is safe to share across threads.
class MemoryDb {
   public:
    explicit MemoryDb(std::filesystem::path path);
    ~MemoryDb();
    MemoryDb(const MemoryDb&) = delete;
    MemoryDb& operator=(const MemoryDb&) = delete;

    const std::filesystem::path& path() const { return path_; }

    // ── Retain ─────────────────────────────────────────────────────────
    // One memory unit to insert. `store_reason` is required (blueprint:
    // every record carries why + provenance). `embedding` is optional;
    // when present it is stored as the semantic projection for the new
    // version.
    struct UnitSpec {
        std::string type;  // world|experience|observation|mental_model|procedure|lesson
        std::string text;
        std::string scope;  // "global" | "session:<sid>" | "profile:<p>"
        std::string session_id;
        std::string profile;
        std::string run_id;
        std::string store_reason;
        std::string model_id;  // "aux:<model>" | "direct" | "import:<source>"
        std::string prompt_revision;
        std::string generation_id;  // provenance: the memory_generations row that produced this unit
        double confidence = 0.5;
        std::string mentioned_at;  // optional ISO date (temporal lookup)
        std::string occurred_start;
        std::string occurred_end;
        std::vector<std::pair<std::string, std::string>> sources;  // (event_id, quote_or_span)
        std::vector<std::pair<std::string, std::string>> anchors;  // (kind, anchor)
        std::vector<std::string> entities;
        std::vector<float> embedding;
        std::string embedding_model;
    };
    // Inserts unit + version + sources + anchors + entities + embedding
    // atomically. Returns the new unit id.
    long long insert_unit(const UnitSpec& spec, std::string& error);
    // True when a version with this content hash already exists (dedupe).
    bool version_exists_by_hash(const std::string& content_hash, std::string& error);

    // ── Recall reads ───────────────────────────────────────────────────
    struct UnitRow {
        long long id = 0;
        std::string type, state, state_reason, scope, session_id, profile, run_id;
        long long current_version_id = 0;
        double confidence = 0.5;
        std::string mentioned_at, occurred_start, occurred_end;
        std::string created_at, updated_at;
        std::string last_consolidated_at;
        bool core = false;  // pinned: injected verbatim into every prompt
    };
    struct VersionRow {
        long long id = 0;
        long long memory_id = 0;
        std::string text, content_hash, store_reason, model_id, prompt_revision;
        std::string generation_id;  // provenance: the generation that produced this version
        long long supersedes_version_id = 0;
        std::string created_at;
    };
    struct AnchorHit {
        long long version_id = 0;
        std::string kind, anchor;
    };
    struct FtsHit {
        long long memory_id = 0;
        long long version_id = 0;
        double bm25 = 0.0;  // lower is better
    };
    struct EmbedRow {
        long long version_id = 0;
        std::string model_id;
        std::vector<float> vector;
    };
    struct LinkRow {
        long long id = 0, source_id = 0, target_id = 0;
        std::string link_type, status;
        std::string reliability_scope, reliability_outcome, reliability_revision;
        std::string condition, guidance, pitfalls;
        long long evidence_version_id = 0;
        std::string created_at;
    };

    // Units in `state` (default "active"), optionally restricted to a scope
    // prefix (e.g. "session:abc" or "profile:ops"); empty = all scopes.
    std::vector<UnitRow> units_by_state(const std::string& state,
                                        const std::string& scope_prefix,
                                        std::string& error);
    std::vector<UnitRow> units_by_ids(const std::vector<long long>& ids, std::string& error);
    std::optional<VersionRow> version(long long version_id, std::string& error);
    std::vector<VersionRow> versions_by_ids(const std::vector<long long>& ids,
                                             std::string& error);
    // All versions of one unit, oldest first (the curation timeline).
    std::vector<VersionRow> versions_for_unit(long long unit_id, std::string& error);
    // Exact-anchor B-tree lookup across all versions (the scope/state gate
    // is applied by the caller).
    std::vector<AnchorHit> anchors_matching(const std::vector<std::string>& anchors,
                                            std::string& error);
    // FTS5 BM25 over active versions, best first.
    std::vector<FtsHit> fts_search(const std::string& match, int limit, std::string& error);

    // ── Browse (UI bank view) ────────────────────────────────────────────
    // One page of units for the memory tab. `state`/`type`/`scope` are
    // equality filters (empty = all); `fts` is a pre-built FTS5 match
    // expression (empty = no text filter, units ordered newest-first;
    // non-empty = one row per matching unit, ranked by its best BM25
    // version). `total` counts every unit matching the filters, before
    // paging, so the UI can show "Showing X of Y".
    struct BrowsePage {
        long long total = 0;
        std::vector<UnitRow> units;
    };
    BrowsePage browse_units(const std::string& state, const std::string& type,
                            const std::string& scope, const std::string& fts,
                            int limit, int offset, std::string& error);
    // All embeddings of active versions (for the semantic scan).
    std::vector<EmbedRow> active_embeddings(std::string& error);
    // Versions that carry one of the given entities.
    std::vector<long long> versions_with_entities(const std::vector<std::string>& entities,
                                                  std::string& error);

    // ── Curate (append-only) ───────────────────────────────────────────
    // New version supersedes the unit's current one; the unit is marked
    // superseded and a semantic link records the relation. The old version
    // and its projections remain (history is never rewritten).
    bool supersede(long long unit_id, const std::string& new_text,
                   const std::string& reason, const std::string& model_id,
                   std::vector<float> embedding, const std::string& embedding_model,
                   std::string& error);
    // Logical invalidation (state: superseded|invalidated|rejected).
    bool set_state(long long unit_id, const std::string& state,
                   const std::string& reason, std::string& error);
    // Core-memory pin: pinned units are injected verbatim into every prompt
    // (the always-injected core). Independent of state - only *active* core
    // units are injected, so superseding/invalidating one unpins it in effect.
    bool set_core(long long unit_id, bool on, std::string& error);
    // Active core units, id order (stable prompt order).
    std::vector<UnitRow> core_units(std::string& error);
    long long core_count(std::string& error);
    // Hard deletion: unit + versions + every projection row (FTS, anchors,
    // entities, embeddings, links touching the unit, feedback).
    bool erase(long long unit_id, std::string& error);

    // ── Links ──────────────────────────────────────────────────────────
    bool add_link(long long source_id, long long target_id, const std::string& link_type,
                  const std::string& status, long long evidence_version_id,
                  std::string& error);
    std::vector<LinkRow> links_from(long long unit_id, const std::string& status,
                                    std::string& error);

    // ── Feedback (offered/cited/used/acted_on signals) ─────────────────
    void record_offered(const std::vector<long long>& unit_ids, const std::string& turn_id,
                        std::string& error);
    // Mark the listed units as used by the model on `turn_id`. Only ids that
    // were actually offered on that turn are accepted (design principle 5:
    // the model proposes, the daemon validates). Returns the number of units
    // marked; already-marked units are not re-counted (idempotent).
    int record_used(const std::vector<long long>& unit_ids, const std::string& turn_id,
                    std::string& error);
    // Mark the listed units as cited: the model's answer text explicitly
    // references them (a [mem:N] marker). Validated the same way as used.
    int record_cited(const std::vector<long long>& unit_ids, const std::string& turn_id,
                     std::string& error);
    // Mark the listed units as acted on: the model took a concrete action
    // based on them (declared via memory.act). Validated the same way as used.
    int record_acted(const std::vector<long long>& unit_ids, const std::string& turn_id,
                     std::string& error);
    // Per-unit signal summary (design principle 5: "offered" is distinct from
    // "cited" / "used" / "acted_on"; recall alone must not raise salience).
    struct FeedbackSummary {
        long long offered = 0;
        std::string last_offered_at;
        long long cited = 0;
        std::string last_cited_at;
        long long used = 0;
        std::string last_used_at;
        long long acted_on = 0;
        std::string last_acted_on_at;
    };
    FeedbackSummary feedback_summary(long long unit_id, std::string& error);

    // ── Consolidation sweeps ───────────────────────────────────────────
    struct SweepRow {
        long long id = 0;
        std::string trigger, watermark, finished_at;
        long long selected = 0, changes = 0;
        bool fixpoint = false;
    };
    std::optional<SweepRow> last_sweep(std::string& error);
    void record_sweep(const std::string& trigger, const std::string& watermark,
                      long long selected, long long changes, bool fixpoint,
                      std::string& error);
    // Active units updated after `watermark` (targeted selection).
    std::vector<UnitRow> active_units_updated_since(const std::string& watermark,
                                                    std::string& error);
    // Active units created after `since`, newest first, capped at `cap`
    // (0 = no cap). The scene watermark query: which units the last scene
    // refresh has not seen yet.
    std::vector<UnitRow> active_units_created_since(const std::string& since,
                                                    int cap, std::string& error);
    void mark_consolidated(long long unit_id, std::string& error);

    // ── Consolidation candidates (approval gate) ────────────────────────
    // A sweep *proposes* near-duplicate pairs; the operator approves or
    // rejects each one. Approving supersedes the older unit (append-only).
    struct ConsolidationRow {
        long long id = 0;
        long long older_unit_id = 0;
        long long newer_unit_id = 0;
        double cosine = 0.0;
        std::string scope;
        // "near_dup" (same fact reworded, cosine above the sweep threshold)
        // or "conflict" (same-topic texts, mid-band cosine, disjoint values -
        // the pair asserts different values for the same fact).
        std::string kind = "near_dup";
        std::string status = "pending";  // pending | approved | rejected
        std::string created_at;
        std::string decided_at;
    };
    long long add_consolidation(long long older_unit_id, long long newer_unit_id,
                                double cosine, const std::string& scope,
                                std::string& error,
                                const std::string& kind = "near_dup");
    std::vector<ConsolidationRow> list_consolidations(const std::string& status) const;
    std::optional<ConsolidationRow> get_consolidation(long long id) const;
    // pending | approved | rejected; returns false when the row is missing.
    bool set_consolidation_status(long long id, const std::string& status,
                                  std::string& error);
    // True when the pair already has a candidate in any status - a pair the
    // operator decided (or that is still pending) is never re-proposed.
    bool has_consolidation_for_pair(long long older_unit_id, long long newer_unit_id,
                                    std::string& error) const;
    std::map<std::string, long long> consolidation_status_counts(std::string& error) const;

    // ── Journey (per-session narrative) ────────────────────────────────
    // One evolving narrative document per session - "how the conversation
    // got to where it is now" - maintained by the auxiliary model at story
    // checkpoints (re-compression, session handoff, operator request).
    // Unlike units this is a *derived artifact*: it is rewritten in place
    // (the previous text is the model's own compressed output, not evidence
    // to preserve), exactly like the ledger's context summary.
    struct JourneyRow {
        std::string session_id;
        std::string profile;
        std::string text;
        long long watermark = 0;  // last covered message id (ledger)
        std::string created_at, updated_at;
    };
    std::optional<JourneyRow> journey_for(const std::string& session_id,
                                          std::string& error) const;
    bool upsert_journey(const std::string& session_id, const std::string& profile,
                        const std::string& text, long long watermark,
                        std::string& error);

    // ── L2 scene blocks + L3 persona ───────────────────────────────────
    // Scene blocks: topic-level narrative distillation of the L1 units
    // (the L2 layer of the unit -> scene -> persona distillation). A derived
    // artifact: each block is rewritten in place by the auxiliary model from
    // the units it covers, so there is no version history - the units are the
    // source of truth. `heat` is the
    // integration counter (update +1, merge = sum of merged +1) and drives
    // scene ordering and the cap-overflow merge priority.
    struct SceneRow {
        long long id = 0;
        std::string name;       // unique slug (lowercase, digits, hyphens)
        std::string summary;    // one line, for the scene map
        int heat = 0;
        std::string content;    // full markdown narrative
        std::string created_at, updated_at;
        std::string generation_id;
    };
    // Heat-descending (map ordering); the scene set is small by contract.
    std::vector<SceneRow> scenes(std::string& error) const;
    // Insert when the name is new, update content/summary/heat when it
    // exists. Returns the scene id (0 on failure).
    long long upsert_scene(const std::string& name, const std::string& summary,
                           int heat, const std::string& content,
                           const std::string& generation_id, std::string& error);
    bool delete_scene_by_name(const std::string& name, std::string& error);
    // Newest scene's updated_at: the scene watermark ("new units" for the
    // next refresh are the units created after it). Empty when there are no
    // scenes (first refresh = whole bank, capped).
    std::string max_scene_updated_at(std::string& error) const;
    long long scene_count(std::string& error) const;

    // Persona: the L3 core document - one row, rewritten in place, injected
    // into every prompt's stable tier. Derived from the scene set.
    struct PersonaRow {
        long long id = 0;
        std::string content;
        std::string generated_at;
        std::string generation_id;
    };
    std::optional<PersonaRow> persona(std::string& error) const;
    long long upsert_persona(const std::string& content,
                             const std::string& generation_id, std::string& error);

    // ── Generation provenance log ──────────────────────────────────────
    // One row per memory generation (aux extraction, direct save, record
    // approval, Hindsight import, journey rewrite): the exact prompt
    // (id + revision + sha256), model, in/out refs, latency, and outcome.
    // Best-effort by contract: a log failure is logged, never propagated
    // (the generation itself has already succeeded).
    struct GenerationLog {
        long long id = 0;
        std::string generation_id;  // stable: gen:<kind>:<ref>
        std::string kind;           // extract|direct|approved|import|journey
        std::string status;         // succeeded|failed
        std::string model_id;
        std::string prompt_id;
        std::string prompt_sha256;
        std::string input_refs;     // JSON array of strings
        std::string output_refs;    // JSON array of strings (unit ids)
        std::string session_id;
        long long latency_ms = 0;
        std::string error;
        std::string created_at;
    };
    long long insert_generation(const GenerationLog& generation, std::string& error);
    // Newest first; kind/status are equality filters (empty = all).
    std::vector<GenerationLog> generations(const std::string& kind,
                                           const std::string& status, int limit,
                                           std::string& error);
    // The newest journey of another session of the same profile (the
    // [PREV SESSION] story pointer); exclude_sid is the current session.
    std::optional<JourneyRow> latest_journey_for_profile(
        const std::string& profile, const std::string& exclude_sid,
        std::string& error) const;
    bool delete_journey(const std::string& session_id, std::string& error);
    long long journey_count(std::string& error) const;

    // ── Status ─────────────────────────────────────────────────────────
    std::map<std::string, long long> unit_state_counts(std::string& error);
    long long version_count(std::string& error);
    long long embedding_count(std::string& error);
    long long fts_row_count(std::string& error);
    long long link_count(std::string& error);
    long long feedback_count(std::string& error);
    long long sweep_count(std::string& error);

   private:
    void migrate(std::string& error);
    void exec(const std::string& sql, std::string& error);
    // Confidence lifecycle (caller holds mutex_). Positive use signals nudge
    // confidence up (bounded to [0,1]); rejection sets it low. Design
    // principle 5: recall alone (offered) must not move confidence.
    void apply_confidence_delta(long long unit_id, double delta, std::string& error);
    void set_confidence_value(long long unit_id, double value, std::string& error);

    std::filesystem::path path_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};

}  // namespace nyaxus
