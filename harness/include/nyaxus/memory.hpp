#pragma once

#include <set>
#include <string>
#include <vector>

#include "nyaxus/store.hpp"

namespace nyaxus {
namespace memory {

// Connection parameters for a Hindsight memory provider instance.
struct HindsightConfig {
    std::string endpoint;  // e.g. http://<hindsight-host>:8888 (optional; the custom provider runs locally)
    std::string bank_id;
    std::string api_key;  // empty = no Authorization header
    // Retain runs LLM extraction on the provider side; a degraded server has
    // been observed taking 50s-400s+ per attempt (including slow failures),
    // so allow a generous window. The worker is async, so this never blocks
    // a turn.
    long long timeout_ms = 600000;
    int max_retries = 5;  // total attempts before a job fails permanently
};

// One retained turn, in Hindsight's document terms. Document identity is
// per-turn (nyaxus-turn:<run-id>) so upserts never clobber other turns.
struct RetainTurn {
    std::string content;
    std::string context;
    std::string document_id;
    std::vector<std::string> tags;
};

// Ensure the bank exists. Hindsight's PUT is create-or-update, so this is
// safe to repeat on every pass.
bool ensure_bank(const HindsightConfig& config, std::string& error);

// Retain one turn. On success returns true; `operation_id` is filled when
// the server reports one (may stay empty).
bool retain(const HindsightConfig& config, const RetainTurn& turn,
            std::string& operation_id, std::string& error);

// Provider health: GET /version.
bool healthy(const HindsightConfig& config, std::string& error);

// One recalled item, in Hindsight's result terms.
struct RecallHit {
    std::string id;
    std::string text;
    std::string type;
    std::string document_id;
    std::vector<std::string> tags;
};

// One memory unit as Hindsight lists it (the fields needed to migrate it into
// the local memory.db).
struct MemoryUnit {
    std::string id;
    std::string text;
    std::string context;
    std::string fact_type;
    std::string document_id;
    std::string session_id;
    std::vector<std::string> tags;
    std::string state;  // "valid" | "invalidated"
};

// List every memory unit in the bank (paginates the provider internally).
// Returns true on success with `units` filled (possibly empty for an empty
// bank). Used by the one-time Hindsight -> local migration.
bool list_memories(const HindsightConfig& config, std::vector<MemoryUnit>& units,
                   std::string& error);

// A bounded recall query. Empty `tags` searches the whole bank; a non-empty
// set is matched with Hindsight's strict all-tags rule.
struct RecallRequest {
    std::string query;
    std::string budget = "low";
    long long max_tokens = 0;  // 0 = the provider's default for the budget
    std::vector<std::string> tags;
};

// Returns true when a recalled item should be hidden because its source was
// deleted, rewound, edited, or regenerated. A suppression key is a bare id
// (run id, session id) or an exact provider document_id / tag string; the
// match recognizes Nyaxus's own document ids (nyaxus-turn:<run>) and tags
// (nyaxus.run:<run>, nyaxus.session:<sid>, ...) and compares both the raw
// value and the extracted bare id against the suppressed set.
bool recall_hit_suppressed(const std::string& document_id,
                           const std::vector<std::string>& tags,
                           const std::set<std::string>& suppressed);

// Recall against the provider. On success returns true with `hits` filled
// (possibly empty: a valid query with no matches).
bool recall(const HindsightConfig& config, const RecallRequest& request,
            std::vector<RecallHit>& hits, std::string& error);

// Drain due outbox jobs (provider "hindsight", operation "retain") against
// the provider. Returns the number of jobs completed in this pass.
int process_due_jobs(SqliteStore& store, const HindsightConfig& config,
                     int max_jobs = 8);

}  // namespace memory
}  // namespace nyaxus
