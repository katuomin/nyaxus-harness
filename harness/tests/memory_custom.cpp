// Custom memory provider lifecycle test (Phase 1 core).
//
// Uses the real Qwen3 engine (embedding + reranker) against a throwaway
// memory.db in a temp directory. The aux model is not required: retain uses
// the direct (trusted) path, which is the same store path the aux extractor
// feeds.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "nyaxus/json.hpp"
#include "nyaxus/memory_custom.hpp"
#include "nyaxus/memorydb.hpp"
#include "nyaxus/qwen3.hpp"

#include "sqlite3.h"

using namespace nyaxus;
namespace custom = nyaxus::memory::custom;

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
    std::cout << (cond ? "  PASS  " : "  FAIL  ") << what << "\n";
    std::cout.flush();
    if (!cond) g_fail++;
}

static void progress(const std::string& what) {
    std::cout << "  ... " << what << "\n";
    std::cout.flush();
}

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : def;
}

static bool hit_present(const custom::RecallResult& result, long long unit_id) {
    for (const custom::RecallHit& hit : result.hits) {
        if (hit.unit_id == unit_id) return true;
    }
    return false;
}

static bool hit_signal(const custom::RecallResult& result, long long unit_id,
                       const std::string& signal) {
    for (const custom::RecallHit& hit : result.hits) {
        if (hit.unit_id != unit_id) continue;
        for (const std::string& s : hit.signals) {
            if (s == signal) return true;
        }
        return false;
    }
    return false;
}

static custom::RetainSpec direct_spec(const std::string& text, const std::string& run_id,
                                      const std::string& type = "world",
                                      const std::string& session_id = "") {
    custom::RetainSpec spec;
    spec.direct_text = text;
    spec.direct_type = type;
    spec.run_id = run_id;
    spec.session_id = session_id;
    spec.store_reason = "test: direct retain (lifecycle suite)";
    spec.model_id = "direct";
    return spec;
}

int main() {
    const std::string embed_path = env_or("NYAXUS_EMBED_MODEL", "");
    const std::string rerank_path = env_or("NYAXUS_RERANK_MODEL", "");
    if (embed_path.empty()) {
        std::cerr << "NYAXUS_EMBED_MODEL is not set; cannot run the custom memory test\n";
        return 2;
    }

    progress("loading embedding model: " + embed_path);
    std::string error;
    qwen3::Model embed_model = qwen3::load(embed_path, error);
    if (!embed_model.ok) {
        std::cerr << "could not load embedding model: " << error << "\n";
        return 2;
    }
    qwen3::Model rerank_model;
    if (!rerank_path.empty()) {
        progress("loading reranker model: " + rerank_path);
        rerank_model = qwen3::load(rerank_path, error);
        if (!rerank_model.ok) {
            std::cout << "note: reranker not loaded (" << error << "); RRF-only mode\n";
            std::cout.flush();
        }
    }

    // Throwaway database in a temp directory.
    const auto stamp =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const std::filesystem::path db_path =
        std::filesystem::temp_directory_path() / ("nyaxus-memtest-" + std::to_string(stamp) +
                                                  ".db");
    progress("opening " + db_path.string());
    MemoryDb db(db_path);

    // ── Retain (direct path) ────────────────────────────────────────────
    std::cout << "retain:\n";
    std::cout.flush();
    long long u_paris = 0, u_dog = 0, u_build = 0, u_model = 0, u_lex = 0, u_sess = 0;
    long long u_order_world = 0, u_order_obs = 0;
    {
        custom::RetainResult r = custom::retain(db, embed_model,
                                                direct_spec("The capital of France is Paris.",
                                                            "run-paris"),
                                                nullptr, error);
        check(r.stored == 1 && error.empty(), "retain paris (stored=1)");
        u_paris = r.unit_ids.empty() ? 0 : r.unit_ids[0];
    }
    {
        custom::RetainResult r = custom::retain(db, embed_model,
                                                direct_spec("Dogs are loyal four-legged "
                                                            "animals that bark.",
                                                            "run-dog"),
                                                nullptr, error);
        check(r.stored == 1, "retain dog");
        u_dog = r.unit_ids.empty() ? 0 : r.unit_ids[0];
    }
    {
        custom::RetainResult r = custom::retain(
            db, embed_model,
            direct_spec("The build script failed on commit 9f8e7d6c5b4a3210 because of a "
                        "missing dependency.",
                        "run-build", "experience"),
            nullptr, error);
        check(r.stored == 1, "retain build-failure (anchor)");
        u_build = r.unit_ids.empty() ? 0 : r.unit_ids[0];
    }
    {
        custom::RetainResult r = custom::retain(
            db, embed_model,
            direct_spec("The Qwen3-0.6B model was used for the embedding engine.",
                        "run-model"),
            nullptr, error);
        check(r.stored == 1, "retain model-name (entity)");
        u_model = r.unit_ids.empty() ? 0 : r.unit_ids[0];
    }
    {
        custom::RetainResult r = custom::retain(
            db, embed_model,
            direct_spec("The zephyrprotocol handshake completed successfully.", "run-lex",
                        "experience"),
            nullptr, error);
        check(r.stored == 1, "retain lexical-only token");
        u_lex = r.unit_ids.empty() ? 0 : r.unit_ids[0];
    }
    {
        custom::RetainResult r = custom::retain(
            db, embed_model,
            direct_spec("In this session we agreed to use SQLite for the ledger.", "run-sess",
                        "world", "sess-1"),
            nullptr, error);
        check(r.stored == 1, "retain session-scoped memory");
        u_sess = r.unit_ids.empty() ? 0 : r.unit_ids[0];
    }
    {
        // Ordering-pair fixtures: a world unit that is the genuine answer and
        // an observation unit that merely shares a word with the query. Both
        // are lexical hits; the world unit is also the semantic match, so it
        // must outrank the observation under relevance-first sorting.
        custom::RetainResult r = custom::retain(
            db, embed_model,
            direct_spec("The answer to the widget calibration question is forty-two.",
                        "run-order-world"),
            nullptr, error);
        check(r.stored == 1, "retain ordering world unit");
        u_order_world = r.unit_ids.empty() ? 0 : r.unit_ids[0];
        custom::RetainResult ro = custom::retain(
            db, embed_model,
            direct_spec("The widget inventory was reviewed during the audit.",
                        "run-order-obs", "observation"),
            nullptr, error);
        check(ro.stored == 1, "retain ordering observation unit");
        u_order_obs = ro.unit_ids.empty() ? 0 : ro.unit_ids[0];
    }
    {
        custom::RetainResult r = custom::retain(
            db, embed_model,
            direct_spec("The capital of France is Paris.", "run-paris-dup"), nullptr, error);
        check(r.duplicate == 1 && r.stored == 0, "identical text is a content-hash duplicate");
    }

    // ── Recall ──────────────────────────────────────────────────────────
    std::cout << "recall:\n";
    std::cout.flush();
    auto base_request = [](const std::string& query) {
        custom::RecallRequest req;
        req.query = query;
        // Cap the rerank fan-out: every candidate is a full cross-encoder
        // forward pass (~15-25 s each on the dev VM), and the production
        // default (50) would make this integration test take hours.
        req.rerank_top = 8;
        return req;
    };

    {
        progress("semantic recall (capital of France)");
        const long long t0 = now_ms();
        custom::RecallRequest req = base_request("What is the capital of France?");
        custom::RecallResult result = custom::recall(db, embed_model, rerank_model, req);
        progress("done in " + std::to_string(now_ms() - t0) + "ms");
        check(result.ok, "semantic recall ok");
        check(!result.hits.empty(), "semantic recall returned hits");
        if (!result.hits.empty()) {
            check(result.hits[0].unit_id == u_paris,
                  "capital-of-France query ranks the Paris memory first (got unit " +
                      std::to_string(result.hits[0].unit_id) + ")");
            check(result.hits[0].reason.count("signals") > 0,
                  "hit carries a reason with signals");
            check(hit_signal(result, u_paris, "semantic"), "Paris hit has the semantic signal");
            if (rerank_model.ok) {
                check(result.reranker_used, "reranker was used");
                check(result.hits[0].reranked, "top hit was reranked");
            }
        }
    }
    {
        progress("exact-anchor recall (commit hash)");
        custom::RecallRequest req = base_request("commit 9f8e7d6c5b4a3210 failed");
        custom::RecallResult result = custom::recall(db, embed_model, rerank_model, req);
        check(result.ok && !result.hits.empty(), "exact-anchor recall returned hits");
        if (!result.hits.empty()) {
            check(result.hits[0].unit_id == u_build,
                  "hash-anchor query ranks the build-failure memory first");
            check(hit_signal(result, u_build, "exact"), "build hit has the exact signal");
        }
    }
    {
        progress("lexical recall (zephyrprotocol)");
        custom::RecallRequest req = base_request("zephyrprotocol handshake status");
        custom::RecallResult result = custom::recall(db, embed_model, rerank_model, req);
        check(result.ok && hit_present(result, u_lex), "lexical token is recalled");
        check(hit_signal(result, u_lex, "lexical"), "zephyr hit has the lexical signal");
    }
    {
        progress("entity recall (Qwen3-0.6B)");
        custom::RecallRequest req =
            base_request("Which Qwen3-0.6B model powers the embedding engine?");
        custom::RecallResult result = custom::recall(db, embed_model, rerank_model, req);
        check(result.ok && hit_present(result, u_model), "entity (model name) is recalled");
        check(hit_signal(result, u_model, "entity"), "model-name hit has the entity signal");
    }
    {
        progress("suppression check");
        custom::RecallRequest req = base_request("loyal animals that bark");
        req.suppressed.insert("run-dog");
        custom::RecallResult result = custom::recall(db, embed_model, rerank_model, req);
        check(result.ok && !hit_present(result, u_dog), "suppressed run id is hidden");
    }
    {
        // Budget: ten long notes, 2048-token budget must hold. Each note has
        // a distinct topic so the near-duplicate guard does not (correctly)
        // reject them.
        const char* topics[] = {"riverbank", "lighthouse", "greenhouse",
                                "windmill",   "observatory", "quarry",
                                "botanical",  "aquarium",    "granary",
                                "foundry"};
        int stored_notes = 0;
        for (int i = 1; i <= 10; ++i) {
            std::string filler = "Note number " + std::to_string(i) +
                                 " concerns the " + topics[i - 1] + ": ";
            for (int k = 0; k < 20; ++k) {
                filler += "the survey of the " + std::string(topics[i - 1]) +
                          " recorded measurements on day " + std::to_string(k + 1) +
                          " with the instruments calibrated ";
            }
            custom::RetainResult r =
                custom::retain(db, embed_model,
                               direct_spec(filler, "run-note-" + std::to_string(i),
                                          "experience"),
                               nullptr, error);
            if (r.stored != 1) {
                check(false, "retain note " + std::to_string(i) + " (" + error +
                                  ", duplicate=" + std::to_string(r.duplicate) +
                                  ")");
                break;
            }
            stored_notes++;
        }
        check(stored_notes == 10, "all ten distinct long notes retained");
        progress("budgeted recall (10 long notes)");
        custom::RecallRequest req = base_request("note number three greenhouse");
        req.budget_tokens = 2048;
        // Keep the test runtime sane: the expensive part is reranking long
        // documents, and the budget logic under test is independent of how
        // many candidates get reranked.
        req.rerank_top = 10;
        custom::RecallResult result = custom::recall(db, embed_model, rerank_model, req);
        check(result.ok && !result.hits.empty(), "budgeted recall returned hits");
        check(result.tokens_used <= 2048,
              "token budget respected (used " + std::to_string(result.tokens_used) + ")");
        for (const custom::RecallHit& hit : result.hits) {
            if (hit.tokens > req.per_item_cap_tokens) {
                check(false, "per-item cap respected");
                break;
            }
        }
    }
    {
        progress("RRF-only fallback");
        custom::RecallRequest req = base_request("What is the capital of France?");
        req.skip_rerank = true;
        custom::RecallResult result = custom::recall(db, embed_model, rerank_model, req);
        check(result.ok && !result.reranker_used, "RRF-only fallback works without the reranker");
        check(hit_present(result, u_paris), "RRF-only fallback still recalls the Paris memory");
    }
    {
        progress("scope gate");
        custom::RecallRequest req = base_request("SQLite ledger agreement");
        req.session_id = "sess-1";
        custom::RecallResult in = custom::recall(db, embed_model, rerank_model, req);
        check(in.ok && hit_present(in, u_sess), "session-scoped memory visible in its session");
        req.session_id = "sess-2";
        custom::RecallResult out = custom::recall(db, embed_model, rerank_model, req);
        check(out.ok && !hit_present(out, u_sess),
              "session-scoped memory hidden from other sessions");
        // Regression: every model run carries the session's profile (default
        // "orchestrator"), but operator-recorded units have no profile. A
        // profile-less unit must stay visible under a profile request -
        // profile is provenance metadata, not a hard isolation boundary.
        req.session_id = "sess-1";
        req.profile = "orchestrator";
        custom::RecallResult prof = custom::recall(db, embed_model, rerank_model, req);
        check(prof.ok && hit_present(prof, u_sess),
              "profile-less unit visible under a profile request");
    }
    {
        // Relevance-first ordering: the world unit is the semantic answer and
        // the stronger lexical match, so it must rank at or above the
        // observation unit that only shares the word "widget". RRF-only keeps
        // this deterministic (no reranker-score dependence). Regression gate
        // for the old hard observation-type class that buried high-relevance
        // non-observation rows.
        progress("relevance-first ordering (world answer vs observation distractor)");
        custom::RecallRequest req = base_request("What is the answer to the widget calibration question?");
        req.skip_rerank = true;
        custom::RecallResult result = custom::recall(db, embed_model, rerank_model, req);
        check(result.ok && hit_present(result, u_order_world) && hit_present(result, u_order_obs),
              "both ordering fixtures are recalled");
        long long idx_world = -1, idx_obs = -1;
        for (std::size_t i = 0; i < result.hits.size(); i++) {
            if (result.hits[i].unit_id == u_order_world) idx_world = static_cast<long long>(i);
            if (result.hits[i].unit_id == u_order_obs) idx_obs = static_cast<long long>(i);
        }
        check(idx_world >= 0 && idx_obs >= 0 && idx_world < idx_obs,
              "higher-relevance world unit outranks the observation distractor "
              "(world idx " + std::to_string(idx_world) + ", obs idx " + std::to_string(idx_obs) + ")");
    }

    // ── Curate ────────────────────────────────────────────────────────────────
    std::cout << "curate:\n";
    std::cout.flush();
    {
        const std::string new_text =
            "The capital of France is Paris, famous for the Eiffel Tower.";
        check(custom::supersede(db, embed_model, u_paris, new_text,
                                "test: supersede with a more specific fact", error),
              "supersede paris unit");
        custom::RecallRequest req = base_request("What is the capital of France?");
        custom::RecallResult result = custom::recall(db, embed_model, rerank_model, req);
        bool saw_new = false;
        for (const custom::RecallHit& hit : result.hits) {
            if (hit.unit_id == u_paris) saw_new = (hit.text == new_text);
        }
        check(saw_new, "superseded unit serves the new version");
    }
    {
        check(custom::set_state(db, u_dog, "invalidated", "test: stale fact", error),
              "invalidate dog unit");
        custom::RecallRequest req = base_request("loyal animals that bark");
        custom::RecallResult result = custom::recall(db, embed_model, rerank_model, req);
        check(result.ok && !hit_present(result, u_dog), "invalidated unit is hidden");
    }
    {
        const long long fts_before = db.fts_row_count(error);
        const long long emb_before = db.embedding_count(error);
        check(custom::erase(db, u_lex, error), "erase lexical unit");
        check(db.fts_row_count(error) < fts_before, "erase purged the FTS projection");
        check(db.embedding_count(error) < emb_before, "erase purged the embedding projection");
        custom::RecallRequest req = base_request("zephyrprotocol handshake");
        custom::RecallResult result = custom::recall(db, embed_model, rerank_model, req);
        check(result.ok && !hit_present(result, u_lex), "erased unit no longer recalls");
    }

    // ── Consolidation sweep ─────────────────────────────────────────────
    std::cout << "consolidation:\n";
    std::cout.flush();
    long long dep_a = 0, dep_b = 0;
    {
        custom::RetainResult r = custom::retain(
            db, embed_model,
            direct_spec("The deployment target is the homelab server at <deployment-lan>.",
                        "run-dep-a"),
            nullptr, error);
        dep_a = r.unit_ids.empty() ? 0 : r.unit_ids[0];
        r = custom::retain(
            db, embed_model,
            direct_spec("The deployment target is the homelab server at <deployment-lan> "
                        "today.",
                        "run-dep-b"),
            nullptr, error);
        dep_b = r.unit_ids.empty() ? 0 : r.unit_ids[0];
        check(dep_a > 0 && dep_b > 0, "retain near-duplicate pair");
    }
    {
        custom::SweepResult sweep = custom::consolidate_sweep(db, embed_model, "test", error);
        check(error.empty(), "sweep completed");
        check(sweep.proposed >= 1,
              "sweep proposed near-duplicates (proposed=" + std::to_string(sweep.proposed) +
                  ")");
        auto state_a = db.units_by_ids({dep_a}, error);
        auto state_b = db.units_by_ids({dep_b}, error);
        check(state_a.size() == 1 && state_b.size() == 1 &&
                  state_a[0].state == "active" && state_b[0].state == "active",
              "nothing is superseded until the operator approves");
    }
    {
        // The approval gate: find the pending candidate for the pair and
        // approve it; exactly one unit becomes superseded.
        long long candidate = 0;
        for (const auto& row : db.list_consolidations("pending")) {
            if ((row.older_unit_id == dep_a && row.newer_unit_id == dep_b) ||
                (row.older_unit_id == dep_b && row.newer_unit_id == dep_a)) {
                candidate = row.id;
                break;
            }
        }
        check(candidate > 0, "pending consolidation candidate exists for the pair");
        // Pin the older unit: approving must transfer the pin to the
        // survivor, so consolidation never silently drops a core unit.
        check(db.set_core(dep_a, true, error) && db.core_count(error) == 1,
              "pin the older unit before approval");
        check(candidate > 0 && custom::decide_consolidation(db, candidate, true, error),
              "approving the candidate succeeds");
        auto state_a = db.units_by_ids({dep_a}, error);
        auto state_b = db.units_by_ids({dep_b}, error);
        check(state_a.size() == 1 && state_b.size() == 1 &&
                  (state_a[0].state != "active") != (state_b[0].state != "active"),
              "exactly one of the pair is superseded after approval");
        check(db.core_count(error) == 1, "the core pin survives the consolidation");
        check(state_b.size() == 1 && state_b[0].state == "active" && state_b[0].core,
              "the survivor inherits the core pin");
        check(db.set_core(dep_b, false, error), "unpin the survivor (leave the bank clean)");
    }
    {
        custom::SweepResult sweep = custom::consolidate_sweep(db, embed_model, "test", error);
        check(sweep.fixpoint, "third sweep reaches a fixpoint (cooldown + no re-proposal)");
    }

    // ── Conflict detection (same topic, disjoint values) ───────────────
    std::cout << "conflict detection:\n";
    std::cout.flush();
    long long conf_a = 0, conf_b = 0;
    {
        // Same fact skeleton, different values. The phrasing differs enough
        // to land in the mid cosine band [0.55, sweep threshold) - a pair
        // above the threshold is a near-duplicate, and the near-duplicate
        // path legitimately claims it first.
        custom::RetainResult r = custom::retain(
            db, embed_model,
            direct_spec("The zephyr protocol listener binds port 4242 on the gateway.",
                        "run-conf-a"),
            nullptr, error);
        conf_a = r.unit_ids.empty() ? 0 : r.unit_ids[0];
        r = custom::retain(
            db, embed_model,
            direct_spec("Connections to the zephyr listener on the gateway are "
                        "accepted on port 6666.",
                        "run-conf-b"),
            nullptr, error);
        conf_b = r.unit_ids.empty() ? 0 : r.unit_ids[0];
        check(conf_a > 0 && conf_b > 0, "retain conflicting pair");
    }
    {
        custom::SweepResult sweep = custom::consolidate_sweep(db, embed_model, "test", error);
        check(error.empty(), "conflict sweep completed");
        long long candidate = 0;
        double candidate_cosine = 0.0;
        for (const auto& row : db.list_consolidations("pending")) {
            if ((row.older_unit_id == conf_a && row.newer_unit_id == conf_b) ||
                (row.older_unit_id == conf_b && row.newer_unit_id == conf_a)) {
                candidate = row.id;
                candidate_cosine = row.cosine;
                check(row.kind == "conflict",
                      "candidate is a conflict (kind=" + row.kind + ", cosine " +
                          std::to_string(candidate_cosine) + ")");
                break;
            }
        }
        check(candidate > 0,
              "conflict pair proposed (cosine " + std::to_string(candidate_cosine) +
                  ", conflicts proposed=" + std::to_string(sweep.conflicts_proposed) + ")");
        // Proposal only: both units stay active until the operator decides.
        auto state_a = db.units_by_ids({conf_a}, error);
        auto state_b = db.units_by_ids({conf_b}, error);
        check(state_a.size() == 1 && state_b.size() == 1 &&
                  state_a[0].state == "active" && state_b[0].state == "active",
              "conflict pair stays active until decided");
        // Approval is a truth decision: the older unit is superseded with a
        // conflict reason (not a near-duplicate one).
        check(candidate > 0 && custom::decide_consolidation(db, candidate, true, error),
              "approving the conflict candidate succeeds");
        auto after_a = db.units_by_ids({conf_a}, error);
        auto after_b = db.units_by_ids({conf_b}, error);
        const MemoryDb::UnitRow* superseded = nullptr;
        for (const auto& row : after_a) if (row.state != "active") superseded = &row;
        if (!superseded) for (const auto& row : after_b) if (row.state != "active") superseded = &row;
        check(superseded && superseded->state == "superseded" &&
                  superseded->state_reason.find("conflicts with") != std::string::npos,
              "the losing unit is superseded with a conflict reason ("
                  + (superseded ? superseded->state_reason : "n/a") + ")");
    }

    // ── Worker payload path (daemon retain-job processing) ──────────────
    std::cout << "worker payload:\n";
    std::cout.flush();
    {
        const std::string turn =
            "User: What port does the nyaxus daemon listen on?\n\n"
            "Assistant: The nyaxus daemon listens on port 8787 by default.";
        auto fake_extract = [](const std::string& content) -> std::string {
            (void)content;
            return "{\"candidates\": [{\"type\": \"world\", "
                   "\"text\": \"The nyaxus daemon listens on port 8787 by default.\", "
                   "\"confidence\": 0.9}]}";
        };
        json::Object payload;
        payload["content"] = json::Value(turn);
        payload["context"] = json::Value("completed turn");
        payload["document_id"] = json::Value("nyaxus-turn:run-worker-1");
        payload["session_id"] = json::Value("sess-worker");
        payload["run_id"] = json::Value("run-worker-1");
        payload["model_id"] = json::Value("aux:test-model");
        std::string operation_id;
        bool permanent = false;
        const bool ok = custom::process_retain_payload(
            db, embed_model, json::stringify(payload), fake_extract,
            operation_id, error, permanent);
        check(ok && !permanent && error.empty(), "extract path stores a grounded candidate");
        check(operation_id.find("stored=1") != std::string::npos,
              "operation id reports stored=1 (got: " + operation_id + ")");
    }
    {
        // Same payload again: content-hash duplicate, still terminal success.
        auto fake_extract = [](const std::string&) -> std::string {
            return "{\"candidates\": [{\"type\": \"world\", "
                   "\"text\": \"The nyaxus daemon listens on port 8787 by default.\", "
                   "\"confidence\": 0.9}]}";
        };
        json::Object payload;
        payload["content"] = json::Value("The nyaxus daemon listens on port 8787 by default.");
        payload["document_id"] = json::Value("nyaxus-turn:run-worker-1");
        std::string operation_id;
        bool permanent = false;
        const bool ok = custom::process_retain_payload(
            db, embed_model, json::stringify(payload), fake_extract,
            operation_id, error, permanent);
        check(ok && operation_id.find("duplicate=1") != std::string::npos,
              "re-run of the same candidate is a duplicate (got: " + operation_id + ")");
    }
    {
        // Empty candidate list: legitimate terminal success.
        auto empty_extract = [](const std::string&) -> std::string {
            return "{\"candidates\": []}";
        };
        json::Object payload;
        payload["content"] = json::Value("User: hi\n\nAssistant: hello there");
        payload["document_id"] = json::Value("nyaxus-turn:run-worker-2");
        std::string operation_id;
        bool permanent = false;
        const bool ok = custom::process_retain_payload(
            db, embed_model, json::stringify(payload), empty_extract,
            operation_id, error, permanent);
        check(ok && operation_id == "no candidates extracted",
              "empty candidate list completes the job");
    }
    {
        // Extractor failure: retryable (not permanent).
        auto throwing_extract = [](const std::string&) -> std::string {
            throw std::runtime_error("aux endpoint down");
        };
        json::Object payload;
        payload["content"] = json::Value("User: hi\n\nAssistant: hello there");
        payload["document_id"] = json::Value("nyaxus-turn:run-worker-3");
        std::string operation_id;
        bool permanent = false;
        const bool ok = custom::process_retain_payload(
            db, embed_model, json::stringify(payload), throwing_extract,
            operation_id, error, permanent);
        check(!ok && !permanent && error.find("aux endpoint down") != std::string::npos,
              "extractor failure is retryable");
    }
    {
        // Malformed payload: permanent failure.
        std::string operation_id;
        bool permanent = false;
        const bool ok = custom::process_retain_payload(
            db, embed_model, "{not json", nullptr, operation_id, error, permanent);
        check(!ok && permanent, "malformed payload fails permanently");
    }
    {
        // Direct (trusted) text path: no extractor needed.
        json::Object payload;
        payload["direct_text"] = json::Value("The worker payload path stores direct text.");
        payload["direct_type"] = json::Value("world");
        payload["document_id"] = json::Value("nyaxus-turn:run-worker-4");
        std::string operation_id;
        bool permanent = false;
        const bool ok = custom::process_retain_payload(
            db, embed_model, json::stringify(payload), nullptr,
            operation_id, error, permanent);
        check(ok && operation_id.find("stored=1") != std::string::npos,
              "direct text path stores without an extractor (got: " + operation_id + ")");
    }
    {
        // Approved-record payload (approval queue promotion): the trusted
        // text stores verbatim with the operator's explicit store_reason and
        // the record's model id.
        const std::string approved_text = "An approved record stores with its reason.";
        json::Object payload;
        payload["direct_text"] = json::Value(approved_text);
        payload["direct_type"] = json::Value("world");
        payload["document_id"] = json::Value("nyaxus-record:42");
        payload["model_id"] = json::Value("approved:record:42");
        payload["store_reason"] = json::Value("approved by operator (record 42)");
        std::string operation_id;
        bool permanent = false;
        const bool ok = custom::process_retain_payload(
            db, embed_model, json::stringify(payload), nullptr,
            operation_id, error, permanent);
        check(ok && operation_id.find("stored=1") != std::string::npos,
              "approved-record payload stores (got: " + operation_id + ")");
        std::string verr;
        bool found = false;
        for (const MemoryDb::UnitRow& unit : db.units_by_state("active", "", verr)) {
            const auto version = db.version(unit.current_version_id, verr);
            if (version && version->text == approved_text) {
                found = version->store_reason == "approved by operator (record 42)" &&
                        version->model_id == "approved:record:42";
                break;
            }
        }
        check(found, "stored version carries the operator's store_reason and model_id");
    }

    // ── Core memory (pinned, always-injected) ────────────────────────────
    std::cout << "core memory:\n";
    std::cout.flush();
    {
        custom::RetainResult r = custom::retain(
            db, embed_model,
            direct_spec("The operator's standing preference is terse answers.",
                        "run-core"),
            nullptr, error);
        check(r.stored == 1 && error.empty(), "retain core candidate (stored=1)");
        const long long u_core = r.unit_ids.empty() ? 0 : r.unit_ids[0];

        std::string cerr;
        const bool pinned = db.set_core(u_core, true, cerr);
        check(pinned && cerr.empty(), "pin the unit to core");
        check(db.core_count(cerr) == 1, "core count is 1");
        const std::vector<MemoryDb::UnitRow> cores = db.core_units(cerr);
        check(cores.size() == 1 && cores.front().id == u_core && cores.front().core,
              "core_units lists the pinned unit");
        bool flagged = false;
        for (const MemoryDb::UnitRow& unit : db.units_by_state("active", "", cerr)) {
            if (unit.id == u_core) flagged = unit.core;
        }
        check(flagged, "units_by_state surfaces the core flag");

        // Invalidation removes it from the core in effect (active only).
        check(db.set_state(u_core, "invalidated", "test", cerr),
              "invalidate the pinned unit");
        check(db.core_count(cerr) == 0 && db.core_units(cerr).empty(),
              "invalidated unit drops out of the core");

        // Round trip: unpin, reactivate, re-pin.
        check(db.set_core(u_core, false, cerr), "unpin the unit");
        check(db.set_state(u_core, "active", "test", cerr), "reactivate the unit");
        check(db.core_count(cerr) == 0, "unpinned unit is not in the core");
        check(db.set_core(u_core, true, cerr) && db.core_count(cerr) == 1,
              "re-pinning works after the round trip");
        check(db.set_core(u_core, false, cerr), "unpin again (leave the bank clean)");
    }

    // ── v1 -> v2 migration (pre-existing bank) ──────────────────────────
    std::cout << "v1 -> v2 migration:\n";
    std::cout.flush();
    {
        // A bank created before the approval gate: stamped with the old
        // schema version and missing the consolidation table.
        const std::filesystem::path legacy_path =
            std::filesystem::temp_directory_path() /
            ("nyaxus-memtest-legacy-" + std::to_string(stamp) + ".db");
        sqlite3* legacy = nullptr;
        check(sqlite3_open(legacy_path.string().c_str(), &legacy) == SQLITE_OK,
              "open the legacy bank");
        if (legacy) {
            char* serr = nullptr;
            sqlite3_exec(legacy, "PRAGMA user_version = 1;", nullptr, nullptr, &serr);
            sqlite3_free(serr);
            sqlite3_close(legacy);
        }
        MemoryDb legacy_db(legacy_path);
        std::string merr;
        const long long candidate = legacy_db.add_consolidation(1, 2, 0.95, "global", merr);
        check(candidate > 0 && merr.empty(),
              "v1 bank migrates and accepts consolidation candidates (id=" +
                  std::to_string(candidate) + (merr.empty() ? "" : " err=" + merr) + ")");
        const std::vector<MemoryDb::ConsolidationRow> rows =
            legacy_db.list_consolidations("pending");
        check(rows.size() == 1 && rows.front().status == "pending",
              "migrated bank lists the pending candidate");
        std::error_code lec;
        std::filesystem::remove(legacy_path, lec);
        std::filesystem::remove(legacy_path.string() + "-wal", lec);
        std::filesystem::remove(legacy_path.string() + "-shm", lec);
    }

    // ── v2 -> v3 migration (pre-existing bank) ──────────────────────────
    std::cout << "v2 -> v3 migration:\n";
    std::cout.flush();
    {
        // A bank from the consolidation-gate era: full v2 schema, stamped
        // v2, no core column. Build it with the current engine, then strip
        // the column and the stamp to simulate the older build.
        const std::filesystem::path legacy_path =
            std::filesystem::temp_directory_path() /
            ("nyaxus-memtest-legacy-v2-" + std::to_string(stamp) + ".db");
        {
            MemoryDb seed(legacy_path);
            std::string serr;
            custom::RetainResult r = custom::retain(
                seed, embed_model,
                direct_spec("A v2 bank gains the core flag on migration.", "run-v3"),
                nullptr, serr);
            check(r.stored == 1 && serr.empty(), "seed the legacy v2 bank");
        }
        sqlite3* legacy = nullptr;
        check(sqlite3_open(legacy_path.string().c_str(), &legacy) == SQLITE_OK,
              "reopen the legacy v2 bank raw");
        if (legacy) {
            char* serr = nullptr;
            sqlite3_exec(legacy,
                         "ALTER TABLE memory_units DROP COLUMN core;"
                         " PRAGMA user_version = 2;",
                         nullptr, nullptr, &serr);
            if (serr) {
                check(false, std::string("strip the core column: ") + serr);
                sqlite3_free(serr);
            }
            sqlite3_close(legacy);
        }
        MemoryDb legacy_db(legacy_path);
        std::string merr;
        check(legacy_db.core_units(merr).empty() && merr.empty(),
              "migrated bank answers core_units cleanly (err=" + merr + ")");
        const std::vector<MemoryDb::UnitRow> all =
            legacy_db.units_by_state("active", "", merr);
        check(all.size() == 1 && merr.empty(), "seeded unit survives the migration");
        if (all.size() == 1) {
            check(legacy_db.set_core(all.front().id, true, merr) && merr.empty(),
                  "pin works on the migrated bank");
            check(legacy_db.core_count(merr) == 1, "core count on the migrated bank");
            const std::vector<MemoryDb::UnitRow> after = legacy_db.core_units(merr);
            check(after.size() == 1 && after.front().id == all.front().id,
                  "core_units lists the pinned unit on the migrated bank");
        }
        std::error_code lec;
        std::filesystem::remove(legacy_path, lec);
        std::filesystem::remove(legacy_path.string() + "-wal", lec);
        std::filesystem::remove(legacy_path.string() + "-shm", lec);
    }

    // ── v6 -> v7 migration (pre-existing bank) ──────────────────────────
    std::cout << "v6 -> v7 migration:\n";
    std::cout.flush();
    {
        // A bank from the generation-log era: full v6 schema (with
        // memory_generations), stamped v6, missing the scene/persona tables.
        // Build it with the current engine, then drop the v7 tables and the
        // stamp to simulate the older build.
        const std::filesystem::path legacy_path =
            std::filesystem::temp_directory_path() /
            ("nyaxus-memtest-legacy-v6-" + std::to_string(stamp) + ".db");
        {
            MemoryDb seed(legacy_path);
            std::string serr;
            custom::RetainResult r = custom::retain(
                seed, embed_model,
                direct_spec("A v6 bank gains the scene tables on migration.", "run-v7"),
                nullptr, serr);
            check(r.stored == 1 && serr.empty(), "seed the legacy v6 bank");
        }
        sqlite3* legacy = nullptr;
        check(sqlite3_open(legacy_path.string().c_str(), &legacy) == SQLITE_OK,
              "reopen the legacy v6 bank raw");
        if (legacy) {
            char* serr = nullptr;
            sqlite3_exec(legacy,
                         "DROP TABLE IF EXISTS memory_scenes;"
                         " DROP TABLE IF EXISTS memory_persona;"
                         " PRAGMA user_version = 6;",
                         nullptr, nullptr, &serr);
            if (serr) {
                check(false, std::string("drop the v7 tables: ") + serr);
                sqlite3_free(serr);
            }
            sqlite3_close(legacy);
        }
        MemoryDb legacy_db(legacy_path);
        std::string merr;
        check(legacy_db.upsert_scene("migrated-scene", "s", 1, "content",
                                     "gen:test", merr) > 0 && merr.empty(),
              "v6 bank migrates and accepts scene upserts");
        check(legacy_db.upsert_persona("persona", "gen:test", merr) == 1 && merr.empty(),
              "v6 bank migrates and accepts persona upserts");
        check(legacy_db.scene_count(merr) == 1, "scene count on the migrated bank");
        std::error_code lec;
        std::filesystem::remove(legacy_path, lec);
        std::filesystem::remove(legacy_path.string() + "-wal", lec);
        std::filesystem::remove(legacy_path.string() + "-shm", lec);
    }

    // ── L2 scene refresh + L3 persona ───────────────────────────────────
    std::cout << "scene + persona:\n";
    std::cout.flush();
    {
        // parse_scene_decisions: valid kept, bad slug / unknown action / non-JSON dropped.
        const std::vector<custom::SceneDecision> parsed = custom::parse_scene_decisions(
            "[{\"action\":\"create\",\"name\":\"good-slug\",\"summary\":\"s\",\"heat\":1,"
            "\"content\":\"c\"},"
            "{\"action\":\"create\",\"name\":\"Bad Slug\",\"summary\":\"s\",\"heat\":1,"
            "\"content\":\"c\"},"
            "{\"action\":\"nope\",\"name\":\"other\",\"summary\":\"s\",\"heat\":1,"
            "\"content\":\"c\"}]");
        check(parsed.size() == 1 && parsed.front().name == "good-slug",
              "parse_scene_decisions keeps only valid decisions");
        check(custom::parse_scene_decisions("not json").empty(),
              "parse_scene_decisions: non-JSON -> no change");

        // First refresh: whole bank (no watermark yet), two creates.
        std::string prompt_seen;
        custom::SceneRefreshResult sr = custom::refresh_scenes(
            db,
            [&prompt_seen](const std::string& prompt) {
                prompt_seen = prompt;
                return "[{\"action\":\"create\",\"name\":\"city-facts\",\"summary\":\"city\","
                       "\"heat\":1,\"content\":\"# City\n\nCity facts.\"},"
                       "{\"action\":\"create\",\"name\":\"pet-facts\",\"summary\":\"pets\","
                       "\"heat\":1,\"content\":\"# Pets\n\nPet facts.\"}]";
            },
            "gen:scene:test-create", error);
        check(sr.ran && sr.created == 2 && sr.changed && sr.unit_ids.size() >= 5 &&
                  prompt_seen.find("## Existing scenes") != std::string::npos &&
                  prompt_seen.find("## New memories") != std::string::npos,
              "first scene refresh: whole-bank watermark, 2 creates, prompt sections");
        check(db.scene_count(error) == 2, "scene count is 2 after creates");
        check(!db.max_scene_updated_at(error).empty(), "scene watermark is set");

        // Update: heat bump + content replacement (new units first: the
        // watermark only advances past units the refresh has seen).
        for (int i = 0; i < 5; i++) {
            custom::retain(
                db, embed_model,
                direct_spec("A scene-refresh update filler unit number " + std::to_string(i) + ".",
                            "run-scene-update-" + std::to_string(i)),
                nullptr, error);
        }
        sr = custom::refresh_scenes(
            db,
            [](const std::string&) {
                return "[{\"action\":\"update\",\"name\":\"city-facts\",\"summary\":\"city\","
                       "\"heat\":7,\"content\":\"# City v2\n\nCity facts, updated.\"}]";
            },
            "gen:scene:test-update", error);
        check(sr.ran && sr.updated == 1 && sr.changed, "scene update applied");
        {
            const std::vector<MemoryDb::SceneRow> scenes = db.scenes(error);
            bool found = false;
            for (const MemoryDb::SceneRow& s : scenes) {
                if (s.name == "city-facts") {
                    found = true;
                    check(s.heat == 7 && s.content.find("City v2") != std::string::npos,
                          "updated scene carries the new heat and content");
                }
            }
            check(found, "updated scene present");
        }

        // Merge: retire both scenes into one (heat = sum of absorbed + 1).
        for (int i = 0; i < 5; i++) {
            custom::retain(
                db, embed_model,
                direct_spec("A scene-refresh merge filler unit number " + std::to_string(i) + ".",
                            "run-scene-merge-" + std::to_string(i)),
                nullptr, error);
        }
        sr = custom::refresh_scenes(
            db,
            [](const std::string&) {
                return "[{\"action\":\"merge\",\"name\":\"city-and-pets\",\"summary\":\"merged\","
                       "\"heat\":0,\"content\":\"# Merged\n\nCity and pets.\","
                       "\"retire\":[\"city-facts\",\"pet-facts\"]}]";
            },
            "gen:scene:test-merge", error);
        check(sr.ran && sr.merged == 1 && sr.changed, "scene merge applied");
        check(db.scene_count(error) == 1, "scene count is 1 after merge");
        {
            const std::vector<MemoryDb::SceneRow> scenes = db.scenes(error);
            check(scenes.size() == 1 && scenes.front().name == "city-and-pets" &&
                      scenes.front().heat == 9,  // 7 + 1 absorbed, + 1 for the merge
                  "merged scene absorbed both heats (7+1+1=9)");
        }

        // No new units since the watermark -> not ran.
        sr = custom::refresh_scenes(
            db, [](const std::string&) { return "[]"; }, "gen:scene:test-idle", error);
        check(!sr.ran && !sr.changed, "no new units -> refresh not ran");

        // LLM failure: the endpoint throws, state is preserved.
        for (int i = 0; i < 5; i++) {
            custom::retain(
                db, embed_model,
                direct_spec("A scene-refresh filler unit number " + std::to_string(i) + ".",
                            "run-scene-filler-" + std::to_string(i)),
                nullptr, error);
        }
        sr = custom::refresh_scenes(
            db,
            [](const std::string&) -> std::string {
                throw std::runtime_error("endpoint down");
            },
            "gen:scene:test-fail", error);
        check(sr.ran && !sr.changed && !sr.error.empty(),
              "LLM failure: error set, no change");
        check(db.scene_count(error) == 1, "scene count unchanged after LLM failure");

        // Cap: 25 creates against a 1-scene bank -> capped at 20, rest dropped.
        std::string cap_json = "[";
        for (int i = 0; i < 25; i++) {
            if (i) cap_json += ",";
            cap_json += "{\"action\":\"create\",\"name\":\"cap-" + std::to_string(i) +
                        "\",\"summary\":\"s\",\"heat\":1,\"content\":\"c\"}";
        }
        cap_json += "]";
        sr = custom::refresh_scenes(
            db, [cap_json](const std::string&) { return cap_json; },
            "gen:scene:test-cap", error);
        check(sr.ran && sr.dropped > 0 && db.scene_count(error) == 20,
              "scene cap enforced (20, extras dropped)");

        // Persona: created from the scenes.
        custom::PersonaRefreshResult pr = custom::refresh_persona(
            db,
            [](const std::string& prompt) {
                check(prompt.find("## Scene blocks") != std::string::npos &&
                          prompt.find("## Current persona") != std::string::npos,
                      "persona prompt carries scenes and current persona");
                return "{\"persona\": \"# Persona\\n\\nUser facts.\"}";
            },
            "gen:persona:test-create", error);
        check(pr.ran && pr.changed && !pr.scene_names.empty(), "persona created");
        {
            const std::optional<MemoryDb::PersonaRow> p = db.persona(error);
            check(p.has_value() && p->content.find("User facts") != std::string::npos,
                  "persona stored");
        }

        // Persona: identical text -> changed=false (no churn).
        pr = custom::refresh_persona(
            db,
            [](const std::string&) {
                return "{\"persona\": \"# Persona\n\nUser facts.\"}";
            },
            "gen:persona:test-same", error);
        check(pr.ran && !pr.changed, "persona unchanged on identical text");

        // Persona: unparseable output -> the current one is preserved.
        pr = custom::refresh_persona(
            db,
            [](const std::string&) { return "not json at all"; },
            "gen:persona:test-bad", error);
        check(pr.ran && !pr.changed && !pr.error.empty(),
              "persona: unparseable output keeps the current one");
        {
            const std::optional<MemoryDb::PersonaRow> p = db.persona(error);
            check(p.has_value() && p->content.find("User facts") != std::string::npos,
                  "persona preserved after unparseable output");
        }

        // parse_persona: valid / non-JSON / over-cap.
        check(custom::parse_persona("{\"persona\": \"x\"}") == "x",
              "parse_persona: valid");
        check(custom::parse_persona("nope").empty(), "parse_persona: non-JSON");
        const std::string big =
            "{\"persona\": \"" + std::string(5000, 'a') + "\"}";
        check(custom::parse_persona(big).empty(), "parse_persona: over-cap rejected");
    }

    // ── Status ──────────────────────────────────────────────────────────
    std::cout << "status:\n";
    std::cout.flush();
    {
        json::Object body = custom::status(db, embed_model, rerank_model);
        check(body.count("units") > 0 && body.count("versions") > 0 &&
                  body.count("engine") > 0,
              "status reports units, versions, and engine state");
    }

    // Cleanup (best effort).
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + "-wal", ec);
    std::filesystem::remove(db_path.string() + "-shm", ec);

    std::cout << (g_fail ? "\nFAILURES: " + std::to_string(g_fail) : "\nALL PASS") << "\n";
    std::cout.flush();
    return g_fail ? 1 : 0;
}
