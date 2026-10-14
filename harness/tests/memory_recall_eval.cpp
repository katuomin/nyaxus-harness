// Recall-quality eval fixture (memory-compare-2026-10 backlog item 1).
//
// A seeded bank (12 units, 4 topic clusters) + 12 in-domain paraphrased
// queries with expected unit ids, asserted recall@5 >= 0.90 (engram's
// gate). Catches silent degradation of the semantic/lexical/RRF/rerank
// chain under refactors: a query that used to hit a unit in the top 5 and
// no longer does fails the build.
//
// The 8 out-of-domain probes are report-only (no gate): their top-1
// rerank scores are the empirical null baseline used to calibrate
// memory.recall_floor.
//
// Uses the real Qwen3 engine (embedding + reranker) against a throwaway
// memory.db, mirroring memory_custom.cpp. The recall request mirrors the
// production daemon parameters (rerank_top=8, budget 2048).
//
// Build (zig recipe, same object set as the daemon minus daemon.cpp; see
// harness/README.md) and run with the model env vars:
//   NYAXUS_EMBED_MODEL=.../qwen3-embedding-0.6b-int8.nyx \
//   NYAXUS_RERANK_MODEL=.../qwen3-reranker-0.6b-int8.nyx \
//   harness/build/nyaxus_memory_recall_eval.exe
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "nyaxus/json.hpp"
#include "nyaxus/memory_custom.hpp"
#include "nyaxus/memorydb.hpp"
#include "nyaxus/qwen3.hpp"

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

struct SeedUnit {
    const char* text;
    const char* type;
};

struct QueryCase {
    const char* query;
    int expected_index;  // index into the seeded unit list
};

int main() {
    const std::string embed_path =
        env_or("NYAXUS_EMBED_MODEL", "models/nyx/qwen3-embedding-0.6b-int8.nyx");
    const std::string rerank_path =
        env_or("NYAXUS_RERANK_MODEL", "models/nyx/qwen3-reranker-0.6b-int8.nyx");

    progress("loading embedding model: " + embed_path);
    std::string error;
    qwen3::Model embed_model = qwen3::load(embed_path, error);
    if (!embed_model.ok) {
        std::cerr << "could not load embedding model: " << error << "\n";
        return 2;
    }
    progress("loading reranker model: " + rerank_path);
    qwen3::Model rerank_model = qwen3::load(rerank_path, error);
    if (!rerank_model.ok) {
        std::cerr << "note: reranker not loaded (" << error << "); RRF-only mode\n";
        std::cerr.flush();
    }

    // Throwaway database in a temp directory.
    const auto stamp =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const std::filesystem::path db_path =
        std::filesystem::temp_directory_path() / ("nyaxus-memeval-" + std::to_string(stamp) +
                                                  ".db");
    progress("opening " + db_path.string());
    MemoryDb db(db_path);

    // ── Seed: 12 units, 4 clusters ────────────────────────────────────────
    // Deliberately concrete and distinctive (names, numbers, places): the
    // queries paraphrase, so a hit requires real semantic + lexical
    // agreement, not shared boilerplate.
    const std::vector<SeedUnit> seed_units = {
        // Cluster A: lighthouse.
        {"The lighthouse keeper password is seagull-42. The keeper log is the "
         "red notebook on the east shelf.",
         "world"},
        {"The lighthouse lens is a first-order Fresnel lens made by Barbier & "
         "Fenestre in 1911.",
         "observation"},
        {"The supply boat arrives at the lighthouse every second Tuesday at "
         "06:00. The pilot ladder must be raised before the boat docks.",
         "procedure"},
        // Cluster B: infrastructure.
        {"Database migration 47 (add_index_events_cursor) failed with "
         "'database is locked'. The fix was a WAL checkpoint before the index "
         "build.",
         "lesson"},
        {"The staging API runs at http://10.20.0.41:8443 and its TLS "
         "certificate is signed by the internal CA nyx-ca.",
         "world"},
        {"The deploy pipeline requires the FORGEJO_TOKEN environment "
         "variable. Tokens expire after 90 days and are kept in the vault "
         "under secret/deploy.",
         "world"},
        // Cluster C: people.
        {"Marta's coffee order is a flat white with oat milk, and she prefers "
         "meetings before 10:00.",
         "observation"},
        {"Jin works from the Osaka office on JST; the overlap window with the "
         "Berlin team is 08:00-10:00 JST.",
         "world"},
        {"Priya is on parental leave until 2026-12-01; her calendar "
         "auto-responds and forwards to the platform team.",
         "observation"},
        // Cluster D: ML.
        {"Ranking model v12 scored nDCG@10 0.71 on the November eval set, up "
         "from 0.68 for v11.",
         "observation"},
        {"The embedding service uses Qwen3-Embedding-0.6B quantized to int8, "
         "with 512-dimensional output and batch size 64.",
         "world"},
        {"The A/B test for the new retrieval page (experiment 88) showed a "
         "2.3% CTR lift, significant at p<0.01.",
         "observation"},
    };

    progress("seeding " + std::to_string(seed_units.size()) + " units (direct retain path)");
    std::vector<long long> unit_ids;
    for (std::size_t i = 0; i < seed_units.size(); ++i) {
        custom::RetainSpec spec;
        spec.direct_text = seed_units[i].text;
        spec.direct_type = seed_units[i].type;
        spec.run_id = "eval-seed-" + std::to_string(i + 1);
        spec.store_reason = "test: recall eval fixture seed";
        spec.model_id = "direct";
        custom::RetainResult r = retain(db, embed_model, spec, nullptr, error);
        if (!error.empty() || r.stored != 1 || r.unit_ids.empty()) {
            std::cout << "  FAIL  seed " << i + 1 << " (stored=" << r.stored
                      << " err=" << error << ")\n";
            ++g_fail;
            unit_ids.push_back(0);
            continue;
        }
        unit_ids.push_back(r.unit_ids.front());
    }
    check(unit_ids.size() == seed_units.size() &&
              std::all_of(unit_ids.begin(), unit_ids.end(),
                          [](long long id) { return id > 0; }),
          "all " + std::to_string(seed_units.size()) + " seed units stored");

    const custom::BrowseResult browse_result =
        custom::browse(db, custom::BrowseRequest());
    check(browse_result.ok && browse_result.total == static_cast<long long>(seed_units.size()),
          "bank holds exactly the seeded units (active total=" +
              std::to_string(browse_result.total) + ")");

    // ── In-domain queries (the gate) ───────────────────────────────────────
    // Paraphrased on purpose: shared surface words are limited, so the hit
    // must come from the embedding + rerank chain, not lexical luck.
    // Queries 5 and 11 also carry an exact anchor (URL / model name) to keep
    // the anchor stage in the loop.
    const std::vector<QueryCase> in_domain = {
        {"What is the keeper's password for the lighthouse?", 0},
        {"Who built the Fresnel lens in the lighthouse, and when?", 1},
        {"When does the supply boat come to the lighthouse?", 2},
        {"Why did migration 47 fail and what fixed it?", 3},
        {"Which CA signed the certificate for http://10.20.0.41:8443?", 4},
        {"What environment variable does the deploy pipeline need, and where is it kept?", 5},
        {"What does Marta drink, and when does she like to meet?", 6},
        {"What time window overlaps Jin's schedule with the Berlin team?", 7},
        {"Until when is Priya out on leave?", 8},
        {"How did ranking model v12 do on the November eval?", 9},
        {"What quantization does the embedding service use for Qwen3-Embedding-0.6B?", 10},
        {"What was the CTR outcome of experiment 88?", 11},
    };

    const int kTopK = 5;
    const double kGate = 0.90;  // engram's recall@5 bar

    std::cout << "in-domain recall@5 (gate >= " << kGate << "):\n";
    std::cout.flush();
    const long long t0 = now_ms();
    int hits5 = 0;
    int hits1 = 0;
    for (std::size_t i = 0; i < in_domain.size(); ++i) {
        const long long expected = unit_ids[in_domain[i].expected_index];
        custom::RecallRequest req;
        req.query = in_domain[i].query;
        req.budget_tokens = 2048;
        req.rerank_top = 8;  // production default
        const custom::RecallResult result =
            custom::recall(db, embed_model, rerank_model, req);
        if (!result.ok) {
            std::cout << "  FAIL  q" << i + 1 << " recall error: " << result.error
                      << "\n";
            ++g_fail;
            continue;
        }
        bool in_top5 = false;
        for (std::size_t k = 0; k < result.hits.size() && k < static_cast<std::size_t>(kTopK);
             ++k) {
            if (result.hits[k].unit_id == expected) {
                in_top5 = true;
                break;
            }
        }
        const bool at1 = !result.hits.empty() && result.hits.front().unit_id == expected;
        if (in_top5) ++hits5;
        if (at1) ++hits1;
        std::ostringstream detail;
        detail << (in_top5 ? "PASS  " : "MISS  ") << "q" << i + 1 << " expected=" << expected
               << " top5=[";
        for (std::size_t k = 0; k < result.hits.size() && k < static_cast<std::size_t>(kTopK);
             ++k) {
            if (k) detail << ",";
            detail << result.hits[k].unit_id;
        }
        detail << "] rerank_top5=[";
        for (std::size_t k = 0; k < result.hits.size() && k < static_cast<std::size_t>(kTopK);
             ++k) {
            if (k) detail << ",";
            std::ostringstream s;
            s << std::fixed << std::setprecision(3) << result.hits[k].rerank_score;
            detail << s.str();
        }
        detail << "]";
        std::cout << "  " << detail.str() << "\n";
        std::cout.flush();
    }
    const long long dt_ms = now_ms() - t0;

    const double recall_at_5 = static_cast<double>(hits5) /
                               static_cast<double>(in_domain.size());
    {
        std::ostringstream line;
        line << "recall@5 = " << hits5 << "/" << in_domain.size()
             << " (" << std::fixed << std::setprecision(3) << recall_at_5 << ")"
             << "  hit@1 = " << hits1 << "/" << in_domain.size()
             << "  (" << dt_ms << " ms for " << in_domain.size() << " recalls)";
        check(recall_at_5 >= kGate, line.str() + "  gate >= " +
                                        std::to_string(kGate));
    }

    // ── Out-of-domain probes (report-only: the floor's null baseline) ─────
    const std::vector<std::string> out_of_domain = {
        "What is the capital of Australia?",
        "Explain quantum chromodynamics in one paragraph.",
        "How do you bake sourdough with an open crumb?",
        "What is the boiling point of mercury?",
        "Summarize the plot of the movie Blade Runner.",
        "How does TCP congestion control work?",
        "In what year did the Berlin Wall fall?",
        "Recommend a jazz album from 1959.",
    };
    std::cout << "out-of-domain top-1 (null baseline for memory.recall_floor):\n";
    std::cout.flush();
    for (std::size_t i = 0; i < out_of_domain.size(); ++i) {
        custom::RecallRequest req;
        req.query = out_of_domain[i];
        req.budget_tokens = 2048;
        req.rerank_top = 8;
        const custom::RecallResult result =
            custom::recall(db, embed_model, rerank_model, req);
        if (!result.ok || result.hits.empty()) {
            std::cout << "  q" << i + 1 << " (no hits)\n";
            continue;
        }
        const custom::RecallHit& top = result.hits.front();
        std::ostringstream s;
        s << std::fixed << std::setprecision(3) << top.rerank_score;
        std::cout << "  q" << i + 1 << " top1=" << top.unit_id
                  << " rerank=" << s.str() << "\n";
        std::cout.flush();
    }

    // ── Recall floor (backlog item 2): suppression mechanics ──────────────
    // q1's true match sits near +2.1 and its near-misses near 0, so a floor
    // of 1.0 keeps the match and drops the rest; 4.0 suppresses everything.
    std::cout << "recall floor:\n";
    std::cout.flush();
    custom::RecallRequest floor_req;
    floor_req.query = in_domain[0].query;
    floor_req.budget_tokens = 2048;
    floor_req.rerank_top = 8;
    floor_req.recall_floor = 1.0;
    const custom::RecallResult floor_low =
        custom::recall(db, embed_model, rerank_model, floor_req);
    check(floor_low.ok && !floor_low.hits.empty() &&
              floor_low.hits.front().unit_id == unit_ids[in_domain[0].expected_index] &&
              floor_low.suppressed_below_floor >= 1,
          "floor 1.0 keeps the true match and suppresses near-misses "
          "(suppressed=" +
              std::to_string(floor_low.suppressed_below_floor) + ")");
    floor_req.recall_floor = 4.0;
    const custom::RecallResult floor_high =
        custom::recall(db, embed_model, rerank_model, floor_req);
    check(floor_high.ok && floor_high.hits.empty() &&
              floor_high.suppressed_below_floor >= 1,
          "floor 4.0 suppresses the whole result (suppressed=" +
              std::to_string(floor_high.suppressed_below_floor) + ")");

    if (g_fail == 0) {
        std::cout << "\nMEMORY_RECALL_EVAL_PASSED\n";
        return 0;
    }
    std::cout << "\nMEMORY_RECALL_EVAL_FAILED (" << g_fail << " failures)\n";
    return 1;
}
