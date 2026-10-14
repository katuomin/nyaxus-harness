#include "nyaxus/memorydb.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

#include "sqlite3.h"

namespace nyaxus {
namespace {

std::string now_utc() {
    const auto current = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(current.time_since_epoch()) %
        1000;
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(current);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &timestamp);
#else
    gmtime_r(&timestamp, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << "." << std::setw(3)
        << std::setfill('0') << milliseconds.count() << "Z";
    return out.str();
}

const char* column_text(sqlite3_stmt* stmt, int column) {
    const unsigned char* value = sqlite3_column_text(stmt, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
}

void check_sqlite(int code, const std::string& operation) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW) {
        throw std::runtime_error(operation + ": sqlite error " + std::to_string(code));
    }
}

// sqlite3_prepare_v2 returns a status code and passes the statement out via
// a pointer; this wrapper matches the more common return-the-statement form
// (nullptr + `error` on failure).
sqlite3_stmt* prepare_stmt(sqlite3* db, const char* sql, std::string& error,
                           const char* operation) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = std::string("prepare failed (") + operation + "): " +
                (db ? sqlite3_errmsg(db) : "unknown");
        return nullptr;
    }
    return stmt;
}

long long step_row(sqlite3_stmt* stmt, const std::string& operation) {
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        throw std::runtime_error(operation + ": sqlite step " + std::to_string(rc));
    }
    return rc == SQLITE_ROW ? 1 : 0;
}

// SHA-256 (FIPS 180-4), hex-encoded - content addressing for version dedupe.
// Same implementation as the ledger store (no external crypto dependency).
constexpr std::uint32_t kSha256Constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x2748774c, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline std::uint32_t sha256_rotr(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::string sha256_hex_impl(const std::string& input) {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const std::size_t bit_length = input.size() * 8;
    std::string message = input;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) message.push_back(0);
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<char>((bit_length >> (i * 8)) & 0xFF));
    }
    std::uint32_t w[64];
    for (std::size_t chunk = 0; chunk < message.size(); chunk += 64) {
        for (int i = 0; i < 16; ++i) {
            const std::size_t off = chunk + i * 4;
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(message[off])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(message[off + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(message[off + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(message[off + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + kSha256Constants[i] + w[i];
            const std::uint32_t S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; ++i) {
        for (int j = 3; j >= 0; --j) {
            const unsigned byte = (h[i] >> (j * 8)) & 0xFF;
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0F]);
        }
    }
    return out;
}

}  // namespace

std::string memory_content_hash(const std::string& text) { return sha256_hex_impl(text); }

MemoryDb::MemoryDb(std::filesystem::path path) : path_(std::move(path)) {
    if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path());
    const int rc = sqlite3_open(path_.string().c_str(), &db_);
    if (rc != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "unknown";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("could not open memory db: " + message);
    }
    sqlite3_busy_timeout(db_, 5000);  // single writer; readers never block long
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    std::string error;
    migrate(error);
    if (!error.empty()) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("memory db migration failed: " + error);
    }
}

MemoryDb::~MemoryDb() {
    if (db_) sqlite3_close(db_);
}

void MemoryDb::exec(const std::string& sql, std::string& error) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        error = message ? message : sqlite3_errmsg(db_);
        sqlite3_free(message);
    }
}

void MemoryDb::migrate(std::string& error) {
    int version = 0;
    sqlite3_stmt* stmt = prepare_stmt(db_, "PRAGMA user_version;", error, "prepare");
    if (stmt) {
        if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (version >= 5) return;
    if (version != 0 && version != 1 && version != 2 && version != 3 && version != 4) {
        error = "unknown schema version " + std::to_string(version);
        return;
    }
    const char* schema =
        "CREATE TABLE memory_units ("
        "  id INTEGER PRIMARY KEY,"
        "  type TEXT NOT NULL,"
        "  state TEXT NOT NULL DEFAULT 'active',"
        "  state_reason TEXT NOT NULL DEFAULT '',"
        "  scope TEXT NOT NULL DEFAULT 'global',"
        "  session_id TEXT NOT NULL DEFAULT '',"
        "  profile TEXT NOT NULL DEFAULT '',"
        "  run_id TEXT NOT NULL DEFAULT '',"
        "  current_version_id INTEGER NOT NULL DEFAULT 0,"
        "  confidence REAL NOT NULL DEFAULT 0.5,"
        "  importance REAL NOT NULL DEFAULT 0.5,"
        "  mentioned_at TEXT NOT NULL DEFAULT '',"
        "  occurred_start TEXT NOT NULL DEFAULT '',"
        "  occurred_end TEXT NOT NULL DEFAULT '',"
        "  last_consolidated_at TEXT NOT NULL DEFAULT '',"
        "  core INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");"
        "CREATE INDEX idx_units_state ON memory_units (state, updated_at);"
        "CREATE INDEX idx_units_scope ON memory_units (scope);"
        "CREATE TABLE memory_versions ("
        "  id INTEGER PRIMARY KEY,"
        "  memory_id INTEGER NOT NULL,"
        "  text TEXT NOT NULL,"
        "  content_hash TEXT NOT NULL,"
        "  supersedes_version_id INTEGER NOT NULL DEFAULT 0,"
        "  model_id TEXT NOT NULL DEFAULT '',"
        "  prompt_revision TEXT NOT NULL DEFAULT '',"
        "  store_reason TEXT NOT NULL,"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE INDEX idx_versions_hash ON memory_versions (content_hash);"
        "CREATE INDEX idx_versions_memory ON memory_versions (memory_id);"
        "CREATE TABLE memory_sources ("
        "  memory_version_id INTEGER NOT NULL,"
        "  session_id TEXT NOT NULL DEFAULT '',"
        "  branch_id TEXT NOT NULL DEFAULT '',"
        "  message_id TEXT NOT NULL DEFAULT '',"
        "  event_id TEXT NOT NULL DEFAULT '',"
        "  source_role TEXT NOT NULL DEFAULT '',"
        "  quote_or_span TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE INDEX idx_sources_version ON memory_sources (memory_version_id);"
        "CREATE TABLE memory_links ("
        "  id INTEGER PRIMARY KEY,"
        "  source_id INTEGER NOT NULL,"
        "  target_id INTEGER NOT NULL,"
        "  link_type TEXT NOT NULL,"
        "  status TEXT NOT NULL DEFAULT 'active',"
        "  reliability_scope TEXT NOT NULL DEFAULT '',"
        "  reliability_outcome TEXT NOT NULL DEFAULT '',"
        "  reliability_validation_revision TEXT NOT NULL DEFAULT '',"
        "  condition TEXT NOT NULL DEFAULT '',"
        "  guidance TEXT NOT NULL DEFAULT '',"
        "  pitfalls TEXT NOT NULL DEFAULT '',"
        "  evidence_version_id INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE INDEX idx_links_source ON memory_links (source_id, status);"
        "CREATE INDEX idx_links_target ON memory_links (target_id);"
        "CREATE TABLE memory_feedback ("
        "  id INTEGER PRIMARY KEY,"
        "  turn_id TEXT NOT NULL,"
        "  memory_id INTEGER NOT NULL,"
        "  offered_at TEXT NOT NULL,"
        "  cited_at TEXT NOT NULL DEFAULT '',"
        "  used_at TEXT NOT NULL DEFAULT '',"
        "  acted_on_at TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE INDEX idx_feedback_memory ON memory_feedback (memory_id);"
        "CREATE TABLE memory_anchors ("
        "  version_id INTEGER NOT NULL,"
        "  anchor_kind TEXT NOT NULL,"
        "  anchor TEXT NOT NULL,"
        "  PRIMARY KEY (version_id, anchor_kind, anchor)"
        ");"
        "CREATE INDEX idx_anchors_lookup ON memory_anchors (anchor_kind, anchor);"
        "CREATE TABLE memory_entities ("
        "  version_id INTEGER NOT NULL,"
        "  entity TEXT NOT NULL,"
        "  PRIMARY KEY (version_id, entity)"
        ");"
        "CREATE INDEX idx_entities_lookup ON memory_entities (entity);"
        "CREATE TABLE memory_embeddings ("
        "  version_id INTEGER PRIMARY KEY,"
        "  model_id TEXT NOT NULL,"
        "  dim INTEGER NOT NULL,"
        "  vector BLOB NOT NULL,"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE TABLE memory_sweeps ("
        "  id INTEGER PRIMARY KEY,"
        "  trigger_name TEXT NOT NULL,"
        "  watermark TEXT NOT NULL,"
        "  selected INTEGER NOT NULL,"
        "  changes INTEGER NOT NULL,"
        "  fixpoint INTEGER NOT NULL,"
        "  finished_at TEXT NOT NULL"
        ");"
        "CREATE TABLE memory_consolidations ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  older_unit_id INTEGER NOT NULL,"
        "  newer_unit_id INTEGER NOT NULL,"
        "  cosine REAL NOT NULL,"
        "  scope TEXT NOT NULL,"
        "  kind TEXT NOT NULL DEFAULT 'near_dup',"
        "  status TEXT NOT NULL DEFAULT 'pending',"
        "  created_at TEXT NOT NULL,"
        "  decided_at TEXT"
        ");"
        "CREATE VIRTUAL TABLE memory_fts USING fts5 ("
        "  memory_id UNINDEXED,"
        "  version_id UNINDEXED,"
        "  text"
        ");"
        "CREATE TABLE journeys ("
        "  session_id TEXT PRIMARY KEY,"
        "  profile TEXT NOT NULL DEFAULT '',"
        "  text TEXT NOT NULL,"
        "  watermark INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");";
    if (version == 0) {
        exec(schema, error);
        if (!error.empty()) return;
    }
    // v1 -> v2: the consolidation approval-gate table (already part of the
    // initial schema above; IF NOT EXISTS keeps both paths idempotent).
    exec("CREATE TABLE IF NOT EXISTS memory_consolidations ("
         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  older_unit_id INTEGER NOT NULL,"
         "  newer_unit_id INTEGER NOT NULL,"
         "  cosine REAL NOT NULL,"
         "  scope TEXT NOT NULL,"
         "  status TEXT NOT NULL DEFAULT 'pending',"
         "  created_at TEXT NOT NULL,"
         "  decided_at TEXT"
         ");", error);
    if (!error.empty()) return;
    exec("PRAGMA user_version = 2;", error);
    if (!error.empty()) return;
    // v2 -> v3: core-memory pin flag. Fresh v0 banks already carry the column
    // from the initial schema above; pre-v3 banks (v1 or v2) get it added.
    // Probe first: a stamped bank without the units table (or already
    // carrying the column) needs no ALTER, and the probe keeps a re-run
    // idempotent.
    if (version != 0) {
        bool table_exists = false;
        bool has_core = false;
        sqlite3_stmt* stmt = prepare_stmt(db_, "PRAGMA table_info(memory_units);",
                                          error, "prepare");
        if (stmt) {
            while (step_row(stmt, "core migration probe") == 1) {
                table_exists = true;
                const unsigned char* name = sqlite3_column_text(stmt, 1);
                if (name && std::string(reinterpret_cast<const char*>(name)) == "core") {
                    has_core = true;
                }
            }
            sqlite3_finalize(stmt);
        }
        if (table_exists && !has_core) {
            exec("ALTER TABLE memory_units ADD COLUMN core INTEGER NOT NULL DEFAULT 0;",
                 error);
            if (!error.empty()) return;
        }
    }
    exec("PRAGMA user_version = 3;", error);
    if (!error.empty()) return;
    // v3 -> v4: the per-session journey (narrative) document. A derived
    // artifact (rewritten in place by the auxiliary model), so it lives here
    // alongside the bank rather than as versioned units.
    exec("CREATE TABLE IF NOT EXISTS journeys ("
         "  session_id TEXT PRIMARY KEY,"
         "  profile TEXT NOT NULL DEFAULT '',"
         "  text TEXT NOT NULL,"
         "  watermark INTEGER NOT NULL DEFAULT 0,"
         "  created_at TEXT NOT NULL,"
         "  updated_at TEXT NOT NULL"
         ");", error);
    if (!error.empty()) return;
    exec("PRAGMA user_version = 4;", error);
    if (!error.empty()) return;
    // v4 -> v5: the candidate kind (near_dup | conflict). Conflict candidates
    // are the sweep's value-mismatch proposals (same-topic texts asserting
    // different values); they ride the same approval queue as near-duplicates.
    // Probe first so a re-run on an already-migrated bank is a no-op.
    if (version != 0) {
        bool has_kind = false;
        sqlite3_stmt* stmt = prepare_stmt(
            db_, "PRAGMA table_info(memory_consolidations);", error, "prepare");
        if (stmt) {
            while (step_row(stmt, "kind migration probe") == 1) {
                const unsigned char* name = sqlite3_column_text(stmt, 1);
                if (name && std::string(reinterpret_cast<const char*>(name)) == "kind") {
                    has_kind = true;
                }
            }
            sqlite3_finalize(stmt);
        }
        if (!has_kind) {
            exec("ALTER TABLE memory_consolidations "
                 "ADD COLUMN kind TEXT NOT NULL DEFAULT 'near_dup';",
                 error);
            if (!error.empty()) return;
        }
    }
    exec("PRAGMA user_version = 5;", error);
}

// ── Retain ───────────────────────────────────────────────────────────────

long long MemoryDb::insert_unit(const UnitSpec& spec, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (spec.text.empty()) {
        error = "unit text is required";
        return 0;
    }
    if (spec.store_reason.empty()) {
        error = "store_reason is required (blueprint: every record carries why + provenance)";
        return 0;
    }
    const std::string now = now_utc();
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        error = "could not begin transaction";
        return 0;
    }
    auto rollback = [&]() {
        char* message = nullptr;
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &message);
        if (message) sqlite3_free(message);
    };

    const long long unit_id = sqlite3_last_insert_rowid(db_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "INSERT INTO memory_units (type, state, scope, session_id, profile, run_id,"
        " confidence, mentioned_at, occurred_start, occurred_end, created_at, updated_at)"
        " VALUES (?, 'active', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
    error, "prepare");
    if (!stmt) {
        rollback();
        error = "prepare unit insert failed";
        return 0;
    }
    bind_text(stmt, 1, spec.type.empty() ? "experience" : spec.type);
    bind_text(stmt, 2, spec.scope.empty() ? "global" : spec.scope);
    bind_text(stmt, 3, spec.session_id);
    bind_text(stmt, 4, spec.profile);
    bind_text(stmt, 5, spec.run_id);
    sqlite3_bind_double(stmt, 6, spec.confidence);
    bind_text(stmt, 7, spec.mentioned_at);
    bind_text(stmt, 8, spec.occurred_start);
    bind_text(stmt, 9, spec.occurred_end);
    bind_text(stmt, 10, now);
    bind_text(stmt, 11, now);
    step_row(stmt, "unit insert");  // throws on error; DONE = success
    const long long new_unit_id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);

    const std::string content_hash = sha256_hex_impl(spec.text);
    stmt = prepare_stmt(
        db_,
        "INSERT INTO memory_versions (memory_id, text, content_hash, model_id,"
        " prompt_revision, store_reason, created_at) VALUES (?, ?, ?, ?, ?, ?, ?);",
    error, "prepare");
    if (!stmt) {
        rollback();
        error = "prepare version insert failed";
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, new_unit_id);
    bind_text(stmt, 2, spec.text);
    bind_text(stmt, 3, content_hash);
    bind_text(stmt, 4, spec.model_id);
    bind_text(stmt, 5, spec.prompt_revision);
    bind_text(stmt, 6, spec.store_reason);
    bind_text(stmt, 7, now);
    step_row(stmt, "version insert");  // throws on error; DONE = success
    const long long version_id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);

    stmt = prepare_stmt(
        db_, "UPDATE memory_units SET current_version_id = ? WHERE id = ?;", error, "prepare");
    if (!stmt || sqlite3_bind_int64(stmt, 1, version_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, new_unit_id) != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        rollback();
        error = "unit version link failed";
        return 0;
    }
    step_row(stmt, "unit update");  // throws on error; DONE = success
    sqlite3_finalize(stmt);

    bool ok = true;
    if (!spec.sources.empty()) {
        stmt = prepare_stmt(
            db_,
            "INSERT INTO memory_sources (memory_version_id, session_id, event_id,"
            " quote_or_span) VALUES (?, ?, ?, ?);",
    error, "prepare");
        if (!stmt) ok = false;
        for (const auto& [event_id, quote] : spec.sources) {
            if (!stmt) break;
            sqlite3_reset(stmt);
            sqlite3_bind_int64(stmt, 1, version_id);
            bind_text(stmt, 2, spec.session_id);
            bind_text(stmt, 3, event_id);
            bind_text(stmt, 4, quote);
            step_row(stmt, "source insert");
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    if (ok && !spec.anchors.empty()) {
        stmt = prepare_stmt(
            db_,
            "INSERT OR IGNORE INTO memory_anchors (version_id, anchor_kind, anchor)"
            " VALUES (?, ?, ?);",
    error, "prepare");
        if (!stmt) ok = false;
        for (const auto& [kind, anchor] : spec.anchors) {
            if (!stmt) break;
            sqlite3_reset(stmt);
            sqlite3_bind_int64(stmt, 1, version_id);
            bind_text(stmt, 2, kind);
            bind_text(stmt, 3, anchor);
            step_row(stmt, "anchor insert");
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    if (ok && !spec.entities.empty()) {
        stmt = prepare_stmt(
            db_, "INSERT OR IGNORE INTO memory_entities (version_id, entity) VALUES (?, ?);",
    error, "prepare");
        if (!stmt) ok = false;
        for (const std::string& entity : spec.entities) {
            if (!stmt) break;
            sqlite3_reset(stmt);
            sqlite3_bind_int64(stmt, 1, version_id);
            bind_text(stmt, 2, entity);
            step_row(stmt, "entity insert");
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    if (ok && !spec.embedding.empty()) {
        const std::size_t bytes = spec.embedding.size() * sizeof(float);
        stmt = prepare_stmt(
            db_,
            "INSERT OR REPLACE INTO memory_embeddings (version_id, model_id, dim, vector,"
            " created_at) VALUES (?, ?, ?, ?, ?);",
    error, "prepare");
        if (!stmt) ok = false;
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, version_id);
            bind_text(stmt, 2, spec.embedding_model);
            sqlite3_bind_int(stmt, 3, static_cast<int>(spec.embedding.size()));
            sqlite3_bind_blob(stmt, 4, spec.embedding.data(), bytes, SQLITE_TRANSIENT);
            bind_text(stmt, 5, now);
            step_row(stmt, "embedding insert");
            sqlite3_finalize(stmt);
        }
    }
    if (ok) {
        stmt = prepare_stmt(
            db_, "INSERT INTO memory_fts (memory_id, version_id, text) VALUES (?, ?, ?);",
    error, "prepare");
        if (!stmt) ok = false;
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, new_unit_id);
            sqlite3_bind_int64(stmt, 2, version_id);
            bind_text(stmt, 3, spec.text);
            step_row(stmt, "fts insert");
            sqlite3_finalize(stmt);
        }
    }
    if (!ok) {
        rollback();
        const std::string cause = error;
        // Keep the specific prepare/step cause: the generic label alone hid
        // which projection failed and why (e.g. "no such table: memory_fts").
        error = "projection insert failed" +
                (cause.empty() ? std::string() : std::string(" - ") + cause);
        return 0;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        error = "commit failed";
        return 0;
    }
    return new_unit_id;
}

bool MemoryDb::version_exists_by_hash(const std::string& content_hash, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_, "SELECT 1 FROM memory_versions WHERE content_hash = ? LIMIT 1;", error, "prepare");
    if (!stmt) {
        error = "prepare hash lookup failed";
        return false;
    }
    bind_text(stmt, 1, content_hash);
    const bool found = step_row(stmt, "hash lookup") == 1;
    sqlite3_finalize(stmt);
    return found;
}

// ── Recall reads ─────────────────────────────────────────────────────────

std::vector<MemoryDb::UnitRow> MemoryDb::units_by_state(const std::string& state,
                                                         const std::string& scope_prefix,
                                                         std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string sql =
        "SELECT id, type, state, state_reason, scope, session_id, profile, run_id,"
        " current_version_id, confidence, mentioned_at, occurred_start, occurred_end,"
        " last_consolidated_at, core, created_at, updated_at FROM memory_units WHERE state = ?";
    if (!scope_prefix.empty()) sql += " AND scope = ?";
    sql += " ORDER BY id;";
    sqlite3_stmt* stmt = prepare_stmt(db_, sql.c_str(), error, "prepare");
    if (!stmt) {
        error = "prepare unit scan failed";
        return {};
    }
    int index = 1;
    bind_text(stmt, index++, state.empty() ? "active" : state);
    if (!scope_prefix.empty()) bind_text(stmt, index, scope_prefix);
    std::vector<UnitRow> rows;
    while (step_row(stmt, "unit scan") == 1) {
        UnitRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.type = column_text(stmt, 1);
        row.state = column_text(stmt, 2);
        row.state_reason = column_text(stmt, 3);
        row.scope = column_text(stmt, 4);
        row.session_id = column_text(stmt, 5);
        row.profile = column_text(stmt, 6);
        row.run_id = column_text(stmt, 7);
        row.current_version_id = sqlite3_column_int64(stmt, 8);
        row.confidence = sqlite3_column_double(stmt, 9);
        row.mentioned_at = column_text(stmt, 10);
        row.occurred_start = column_text(stmt, 11);
        row.occurred_end = column_text(stmt, 12);
        row.last_consolidated_at = column_text(stmt, 13);
        row.core = sqlite3_column_int(stmt, 14) != 0;
        row.created_at = column_text(stmt, 15);
        row.updated_at = column_text(stmt, 16);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<MemoryDb::UnitRow> MemoryDb::units_by_ids(const std::vector<long long>& ids,
                                                       std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ids.empty()) return {};
    std::string sql =
        "SELECT id, type, state, state_reason, scope, session_id, profile, run_id,"
        " current_version_id, confidence, mentioned_at, occurred_start, occurred_end,"
        " last_consolidated_at, core, created_at, updated_at FROM memory_units WHERE id IN (";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        sql += (i == 0 ? "?" : ",?");
    }
    sql += ");";
    sqlite3_stmt* stmt = prepare_stmt(db_, sql.c_str(), error, "prepare");
    if (!stmt) {
        error = "prepare unit fetch failed";
        return {};
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        sqlite3_bind_int64(stmt, static_cast<int>(i + 1), ids[i]);
    }
    std::vector<UnitRow> rows;
    while (step_row(stmt, "unit fetch") == 1) {
        UnitRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.type = column_text(stmt, 1);
        row.state = column_text(stmt, 2);
        row.state_reason = column_text(stmt, 3);
        row.scope = column_text(stmt, 4);
        row.session_id = column_text(stmt, 5);
        row.profile = column_text(stmt, 6);
        row.run_id = column_text(stmt, 7);
        row.current_version_id = sqlite3_column_int64(stmt, 8);
        row.confidence = sqlite3_column_double(stmt, 9);
        row.mentioned_at = column_text(stmt, 10);
        row.occurred_start = column_text(stmt, 11);
        row.occurred_end = column_text(stmt, 12);
        row.last_consolidated_at = column_text(stmt, 13);
        row.core = sqlite3_column_int(stmt, 14) != 0;
        row.created_at = column_text(stmt, 15);
        row.updated_at = column_text(stmt, 16);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::optional<MemoryDb::VersionRow> MemoryDb::version(long long version_id,
                                                       std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT id, memory_id, text, content_hash, supersedes_version_id, model_id,"
        " prompt_revision, store_reason, created_at FROM memory_versions WHERE id = ?;",
    error, "prepare");
    if (!stmt) {
        error = "prepare version fetch failed";
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, version_id);
    std::optional<VersionRow> row;
    if (step_row(stmt, "version fetch") == 1) {
        VersionRow r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.memory_id = sqlite3_column_int64(stmt, 1);
        r.text = column_text(stmt, 2);
        r.content_hash = column_text(stmt, 3);
        r.supersedes_version_id = sqlite3_column_int64(stmt, 4);
        r.model_id = column_text(stmt, 5);
        r.prompt_revision = column_text(stmt, 6);
        r.store_reason = column_text(stmt, 7);
        r.created_at = column_text(stmt, 8);
        row = std::move(r);
    }
    sqlite3_finalize(stmt);
    return row;
}

std::vector<MemoryDb::VersionRow> MemoryDb::versions_by_ids(
    const std::vector<long long>& ids, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ids.empty()) return {};
    std::string sql =
        "SELECT id, memory_id, text, content_hash, supersedes_version_id, model_id,"
        " prompt_revision, store_reason, created_at FROM memory_versions WHERE id IN (";
    for (std::size_t i = 0; i < ids.size(); ++i) sql += (i == 0 ? "?" : ",?");
    sql += ");";
    sqlite3_stmt* stmt = prepare_stmt(db_, sql.c_str(), error, "prepare");
    if (!stmt) {
        error = "prepare version fetch failed";
        return {};
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        sqlite3_bind_int64(stmt, static_cast<int>(i + 1), ids[i]);
    }
    std::vector<VersionRow> rows;
    while (step_row(stmt, "version fetch") == 1) {
        VersionRow r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.memory_id = sqlite3_column_int64(stmt, 1);
        r.text = column_text(stmt, 2);
        r.content_hash = column_text(stmt, 3);
        r.supersedes_version_id = sqlite3_column_int64(stmt, 4);
        r.model_id = column_text(stmt, 5);
        r.prompt_revision = column_text(stmt, 6);
        r.store_reason = column_text(stmt, 7);
        r.created_at = column_text(stmt, 8);
        rows.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<MemoryDb::VersionRow> MemoryDb::versions_for_unit(long long unit_id,
                                                               std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT id, memory_id, text, content_hash, supersedes_version_id, model_id,"
        " prompt_revision, store_reason, created_at FROM memory_versions"
        " WHERE memory_id = ? ORDER BY id;",
        error, "prepare");
    if (!stmt) {
        error = "prepare version history fetch failed";
        return {};
    }
    sqlite3_bind_int64(stmt, 1, unit_id);
    std::vector<VersionRow> rows;
    while (step_row(stmt, "version history fetch") == 1) {
        VersionRow r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.memory_id = sqlite3_column_int64(stmt, 1);
        r.text = column_text(stmt, 2);
        r.content_hash = column_text(stmt, 3);
        r.supersedes_version_id = sqlite3_column_int64(stmt, 4);
        r.model_id = column_text(stmt, 5);
        r.prompt_revision = column_text(stmt, 6);
        r.store_reason = column_text(stmt, 7);
        r.created_at = column_text(stmt, 8);
        rows.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<MemoryDb::AnchorHit> MemoryDb::anchors_matching(
    const std::vector<std::string>& anchors, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (anchors.empty()) return {};
    std::string sql =
        "SELECT version_id, anchor_kind, anchor FROM memory_anchors WHERE anchor IN (";
    for (std::size_t i = 0; i < anchors.size(); ++i) sql += (i == 0 ? "?" : ",?");
    sql += ");";
    sqlite3_stmt* stmt = prepare_stmt(db_, sql.c_str(), error, "prepare");
    if (!stmt) {
        error = "prepare anchor lookup failed";
        return {};
    }
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        bind_text(stmt, static_cast<int>(i + 1), anchors[i]);
    }
    std::vector<AnchorHit> hits;
    while (step_row(stmt, "anchor lookup") == 1) {
        AnchorHit hit;
        hit.version_id = sqlite3_column_int64(stmt, 0);
        hit.kind = column_text(stmt, 1);
        hit.anchor = column_text(stmt, 2);
        hits.push_back(std::move(hit));
    }
    sqlite3_finalize(stmt);
    return hits;
}

std::vector<MemoryDb::FtsHit> MemoryDb::fts_search(const std::string& match, int limit,
                                                    std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (match.empty()) return {};
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT memory_id, version_id, bm25(memory_fts) AS score FROM memory_fts"
        " JOIN memory_units ON memory_units.id = memory_fts.memory_id"
        " WHERE memory_fts MATCH ? AND memory_units.state = 'active'"
        " ORDER BY score ASC LIMIT ?;",
    error, "prepare");
    if (!stmt) {
        error = "prepare fts search failed";
        return {};
    }
    bind_text(stmt, 1, match);
    sqlite3_bind_int(stmt, 2, limit < 1 ? 50 : limit);
    std::vector<FtsHit> hits;
    while (step_row(stmt, "fts search") == 1) {
        FtsHit hit;
        hit.memory_id = sqlite3_column_int64(stmt, 0);
        hit.version_id = sqlite3_column_int64(stmt, 1);
        hit.bm25 = sqlite3_column_double(stmt, 2);
        hits.push_back(std::move(hit));
    }
    sqlite3_finalize(stmt);
    return hits;
}

MemoryDb::BrowsePage MemoryDb::browse_units(const std::string& state,
                                            const std::string& type,
                                            const std::string& scope,
                                            const std::string& fts, int limit, int offset,
                                            std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    BrowsePage page;
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    if (offset < 0) offset = 0;

    // Shared equality filters; bind order is state, type, scope (non-empty
    // only), after any FTS match expression.
    std::string filters;
    std::vector<std::string> binds;
    auto add_filter = [&](const char* column, const std::string& value) {
        if (value.empty()) return;
        filters += std::string(" AND u.") + column + " = ?";
        binds.push_back(value);
    };
    add_filter("state", state);
    add_filter("type", type);
    add_filter("scope", scope);

    const char* columns =
        "u.id, u.type, u.state, u.state_reason, u.scope, u.session_id, u.profile,"
        " u.run_id, u.current_version_id, u.confidence, u.mentioned_at,"
        " u.occurred_start, u.occurred_end, u.last_consolidated_at, u.core,"
        " u.created_at, u.updated_at";
    auto read_unit = [](sqlite3_stmt* stmt) -> UnitRow {
        UnitRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.type = column_text(stmt, 1);
        row.state = column_text(stmt, 2);
        row.state_reason = column_text(stmt, 3);
        row.scope = column_text(stmt, 4);
        row.session_id = column_text(stmt, 5);
        row.profile = column_text(stmt, 6);
        row.run_id = column_text(stmt, 7);
        row.current_version_id = sqlite3_column_int64(stmt, 8);
        row.confidence = sqlite3_column_double(stmt, 9);
        row.mentioned_at = column_text(stmt, 10);
        row.occurred_start = column_text(stmt, 11);
        row.occurred_end = column_text(stmt, 12);
        row.last_consolidated_at = column_text(stmt, 13);
        row.core = sqlite3_column_int(stmt, 14) != 0;
        row.created_at = column_text(stmt, 15);
        row.updated_at = column_text(stmt, 16);
        return row;
    };
    auto bind_filters = [&](sqlite3_stmt* stmt, int index, const std::string& match) {
        if (!match.empty()) bind_text(stmt, index++, match);
        for (const std::string& value : binds) bind_text(stmt, index++, value);
    };

    if (!fts.empty()) {
        // Text filter. A unit's older versions may match too, so the ranked
        // rows are deduplicated per unit (best version first). FTS5's MATCH
        // and bm25() take the table name, not an alias, and bm25() may only
        // appear in the top-level query that selects from the FTS table -
        // not in aggregates, windows, or subqueries.
        std::string count_sql =
            "SELECT COUNT(DISTINCT u.id) FROM memory_fts"
            " JOIN memory_units u ON u.id = memory_fts.memory_id"
            " WHERE memory_fts MATCH ?" + filters + ";";
        sqlite3_stmt* stmt = prepare_stmt(db_, count_sql.c_str(), error, "browse count");
        if (!stmt) {
            error = "prepare browse count failed";
            return page;
        }
        bind_filters(stmt, 1, fts);
        page.total = step_row(stmt, "browse count") == 1 ? sqlite3_column_int64(stmt, 0) : 0;
        sqlite3_finalize(stmt);

        // Fetch a ranked pool (a unit can occupy several rows) and dedupe.
        int pool = (offset + limit) * 4;
        if (pool < 200) pool = 200;
        if (pool > 2000) pool = 2000;
        std::string sql = "SELECT " + std::string(columns) +
                          " FROM memory_fts JOIN memory_units u ON u.id = memory_fts.memory_id"
                          " WHERE memory_fts MATCH ?" + filters +
                          " ORDER BY bm25(memory_fts) ASC, u.id ASC, memory_fts.version_id ASC"
                          " LIMIT ?;";
        stmt = prepare_stmt(db_, sql.c_str(), error, "browse page");
        if (!stmt) {
            error = "prepare browse page failed";
            return page;
        }
        bind_filters(stmt, 1, fts);
        sqlite3_bind_int(stmt, static_cast<int>(binds.size()) + 2, pool);
        std::vector<UnitRow> fetched;
        while (step_row(stmt, "browse page") == 1) {
            fetched.push_back(read_unit(stmt));
        }
        sqlite3_finalize(stmt);
        std::set<long long> seen;
        std::size_t distinct = 0;
        for (const UnitRow& row : fetched) {
            if (!seen.insert(row.id).second) continue;  // a better row already kept
            if (distinct < static_cast<std::size_t>(offset)) {
                ++distinct;
                continue;
            }
            ++distinct;
            page.units.push_back(row);
            if (page.units.size() >= static_cast<std::size_t>(limit)) break;
        }
        return page;
    }

    // No text filter: newest-first listing.
    std::string count_sql =
        "SELECT COUNT(*) FROM memory_units u WHERE 1=1" + filters + ";";
    sqlite3_stmt* stmt = prepare_stmt(db_, count_sql.c_str(), error, "browse count");
    if (!stmt) {
        error = "prepare browse count failed";
        return page;
    }
    bind_filters(stmt, 1, "");
    page.total = step_row(stmt, "browse count") == 1 ? sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);

    std::string sql = "SELECT " + std::string(columns) +
                      " FROM memory_units u WHERE 1=1" + filters +
                      " ORDER BY u.created_at DESC, u.id DESC LIMIT ? OFFSET ?;";
    stmt = prepare_stmt(db_, sql.c_str(), error, "browse page");
    if (!stmt) {
        error = "prepare browse page failed";
        return page;
    }
    bind_filters(stmt, 1, "");
    sqlite3_bind_int(stmt, static_cast<int>(binds.size()) + 1, limit);
    sqlite3_bind_int(stmt, static_cast<int>(binds.size()) + 2, offset);
    while (step_row(stmt, "browse page") == 1) {
        page.units.push_back(read_unit(stmt));
    }
    sqlite3_finalize(stmt);
    return page;
}

std::vector<MemoryDb::EmbedRow> MemoryDb::active_embeddings(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT e.version_id, e.model_id, e.vector FROM memory_embeddings e"
        " JOIN memory_units u ON u.current_version_id = e.version_id"
        " WHERE u.state = 'active';",
    error, "prepare");
    if (!stmt) {
        error = "prepare embedding scan failed";
        return {};
    }
    std::vector<EmbedRow> rows;
    while (step_row(stmt, "embedding scan") == 1) {
        EmbedRow row;
        row.version_id = sqlite3_column_int64(stmt, 0);
        row.model_id = column_text(stmt, 1);
        const void* blob = sqlite3_column_blob(stmt, 2);
        const std::size_t bytes = blob ? static_cast<std::size_t>(sqlite3_column_bytes(stmt, 2))
                                      : 0;
        if (blob && bytes % sizeof(float) == 0) {
            const float* data = static_cast<const float*>(blob);
            row.vector.assign(data, data + bytes / sizeof(float));
        }
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<long long> MemoryDb::versions_with_entities(
    const std::vector<std::string>& entities, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entities.empty()) return {};
    std::string sql = "SELECT DISTINCT version_id FROM memory_entities WHERE entity IN (";
    for (std::size_t i = 0; i < entities.size(); ++i) sql += (i == 0 ? "?" : ",?");
    sql += ");";
    sqlite3_stmt* stmt = prepare_stmt(db_, sql.c_str(), error, "prepare");
    if (!stmt) {
        error = "prepare entity lookup failed";
        return {};
    }
    for (std::size_t i = 0; i < entities.size(); ++i) {
        bind_text(stmt, static_cast<int>(i + 1), entities[i]);
    }
    std::vector<long long> ids;
    while (step_row(stmt, "entity lookup") == 1) {
        ids.push_back(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return ids;
}

// ── Curate ───────────────────────────────────────────────────────────────

bool MemoryDb::supersede(long long unit_id, const std::string& new_text,
                         const std::string& reason, const std::string& model_id,
                         std::vector<float> embedding, const std::string& embedding_model,
                         std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (new_text.empty()) {
        error = "supersede text is required";
        return false;
    }
    if (reason.empty()) {
        error = "supersede reason is required";
        return false;
    }
    const std::string now = now_utc();
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        error = "could not begin transaction";
        return false;
    }
    auto rollback = [&]() {
        char* message = nullptr;
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &message);
        if (message) sqlite3_free(message);
    };

    sqlite3_stmt* stmt = prepare_stmt(
        db_, "SELECT current_version_id FROM memory_units WHERE id = ?;", error, "prepare");
    if (!stmt) {
        rollback();
        error = "prepare unit fetch failed";
        return false;
    }
    sqlite3_bind_int64(stmt, 1, unit_id);
    long long previous_version = 0;
    if (step_row(stmt, "unit fetch") == 1) {
        previous_version = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (previous_version == 0) {
        rollback();
        error = "unit not found";
        return false;
    }

    const std::string content_hash = sha256_hex_impl(new_text);
    stmt = prepare_stmt(
        db_,
        "INSERT INTO memory_versions (memory_id, text, content_hash, supersedes_version_id,"
        " model_id, store_reason, created_at) VALUES (?, ?, ?, ?, ?, ?, ?);",
    error, "prepare");
    if (!stmt) {
        rollback();
        error = "prepare version insert failed";
        return false;
    }
    sqlite3_bind_int64(stmt, 1, unit_id);
    bind_text(stmt, 2, new_text);
    bind_text(stmt, 3, content_hash);
    sqlite3_bind_int64(stmt, 4, previous_version);
    bind_text(stmt, 5, model_id);
    bind_text(stmt, 6, reason);
    bind_text(stmt, 7, now);
    step_row(stmt, "version insert");  // throws on error; DONE = success
    const long long version_id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);

    // The unit stays active with a new current version (append-only);
    // supersession is recorded on the version, not the unit.
    bool ok = true;
    stmt = prepare_stmt(
        db_, "UPDATE memory_units SET current_version_id = ?, updated_at = ? WHERE id = ?;",
    error, "prepare");
    if (!stmt) ok = false;
    if (stmt) {
        sqlite3_bind_int64(stmt, 1, version_id);
        bind_text(stmt, 2, now);
        sqlite3_bind_int64(stmt, 3, unit_id);
        step_row(stmt, "unit update");
        sqlite3_finalize(stmt);
    }
    if (ok && !embedding.empty()) {
        const std::size_t bytes = embedding.size() * sizeof(float);
        stmt = prepare_stmt(
            db_,
            "INSERT OR REPLACE INTO memory_embeddings (version_id, model_id, dim, vector,"
            " created_at) VALUES (?, ?, ?, ?, ?);",
    error, "prepare");
        if (!stmt) ok = false;
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, version_id);
            bind_text(stmt, 2, embedding_model);
            sqlite3_bind_int(stmt, 3, static_cast<int>(embedding.size()));
            sqlite3_bind_blob(stmt, 4, embedding.data(), bytes, SQLITE_TRANSIENT);
            bind_text(stmt, 5, now);
            step_row(stmt, "embedding insert");
            sqlite3_finalize(stmt);
        }
    }
    if (ok) {
        stmt = prepare_stmt(
            db_, "INSERT INTO memory_fts (memory_id, version_id, text) VALUES (?, ?, ?);",
    error, "prepare");
        if (!stmt) ok = false;
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, unit_id);
            sqlite3_bind_int64(stmt, 2, version_id);
            bind_text(stmt, 3, new_text);
            step_row(stmt, "fts insert");
            sqlite3_finalize(stmt);
        }
    }
    if (!ok) {
        rollback();
        error = "supersede projection update failed";
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        error = "commit failed";
        return false;
    }
    return true;
}

bool MemoryDb::set_state(long long unit_id, const std::string& state,
                         const std::string& reason, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state != "active" && state != "superseded" && state != "invalidated" &&
        state != "rejected") {
        error = "unknown state: " + state;
        return false;
    }
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "UPDATE memory_units SET state = ?, state_reason = ?, updated_at = ? WHERE id = ?;",
    error, "prepare");
    if (!stmt) {
        error = "prepare state update failed";
        return false;
    }
    bind_text(stmt, 1, state);
    bind_text(stmt, 2, reason);
    bind_text(stmt, 3, now_utc());
    sqlite3_bind_int64(stmt, 4, unit_id);
    const bool ok = step_row(stmt, "state update") == 0 &&
                    sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);
    if (!ok) {
        error = "unit not found";
        return ok;
    }
    // Rejection is strong negative evidence: lower the unit's confidence.
    if (state == "invalidated" || state == "rejected") {
        set_confidence_value(unit_id, 0.1, error);
    }
    return ok;
}

void MemoryDb::apply_confidence_delta(long long unit_id, double delta,
                                      std::string& error) {
    // Caller holds mutex_. Bounded to [0,1] via SQL MIN/MAX.
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "UPDATE memory_units SET confidence = MIN(1.0, MAX(0.0, confidence + ?))"
        " WHERE id = ?;",
        error, "prepare");
    if (!stmt) {
        error = "prepare confidence update failed";
        return;
    }
    sqlite3_bind_double(stmt, 1, delta);
    sqlite3_bind_int64(stmt, 2, unit_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void MemoryDb::set_confidence_value(long long unit_id, double value,
                                    std::string& error) {
    // Caller holds mutex_.
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "UPDATE memory_units SET confidence = ? WHERE id = ?;",
        error, "prepare");
    if (!stmt) {
        error = "prepare confidence set failed";
        return;
    }
    sqlite3_bind_double(stmt, 1, value);
    sqlite3_bind_int64(stmt, 2, unit_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool MemoryDb::erase(long long unit_id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        error = "could not begin transaction";
        return false;
    }
    auto rollback = [&]() {
        char* message = nullptr;
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &message);
        if (message) sqlite3_free(message);
    };
    const char* deletes[] = {
        "DELETE FROM memory_fts WHERE memory_id = ?;",
        "DELETE FROM memory_anchors WHERE version_id IN (SELECT id FROM memory_versions"
        " WHERE memory_id = ?);",
        "DELETE FROM memory_entities WHERE version_id IN (SELECT id FROM memory_versions"
        " WHERE memory_id = ?);",
        "DELETE FROM memory_embeddings WHERE version_id IN (SELECT id FROM memory_versions"
        " WHERE memory_id = ?);",
        "DELETE FROM memory_links WHERE source_id = ? OR target_id = ?;",
        "DELETE FROM memory_feedback WHERE memory_id = ?;",
        "DELETE FROM memory_sources WHERE memory_version_id IN (SELECT id FROM"
        " memory_versions WHERE memory_id = ?);",
        "DELETE FROM memory_versions WHERE memory_id = ?;",
        "DELETE FROM memory_units WHERE id = ?;",
    };
    for (const char* sql : deletes) {
        sqlite3_stmt* stmt = prepare_stmt(db_, sql, error, "prepare");
        if (!stmt) {
            rollback();
            error = "prepare erase failed";
            return false;
        }
        const bool double_bind =
            std::string(sql).find("source_id = ? OR target_id = ?") != std::string::npos;
        sqlite3_bind_int64(stmt, 1, unit_id);
        if (double_bind) sqlite3_bind_int64(stmt, 2, unit_id);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            rollback();
            error = "erase delete failed";
            return false;
        }
        sqlite3_finalize(stmt);
    }
    // The final DELETE targets memory_units; zero rows means the id never
    // existed, so the caller can 404 instead of silently succeeding.
    if (sqlite3_changes(db_) == 0) {
        rollback();
        error = "unit not found";
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        error = "commit failed";
        return false;
    }
    return true;
}

// ── Links ────────────────────────────────────────────────────────────────

bool MemoryDb::add_link(long long source_id, long long target_id, const std::string& link_type,
                        const std::string& status, long long evidence_version_id,
                        std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "INSERT INTO memory_links (source_id, target_id, link_type, status,"
        " evidence_version_id, created_at) VALUES (?, ?, ?, ?, ?, ?);",
    error, "prepare");
    if (!stmt) {
        error = "prepare link insert failed";
        return false;
    }
    sqlite3_bind_int64(stmt, 1, source_id);
    sqlite3_bind_int64(stmt, 2, target_id);
    bind_text(stmt, 3, link_type);
    bind_text(stmt, 4, status.empty() ? "active" : status);
    sqlite3_bind_int64(stmt, 5, evidence_version_id);
    bind_text(stmt, 6, now_utc());
    const bool ok = step_row(stmt, "link insert") == 0;
    sqlite3_finalize(stmt);
    if (!ok) error = "link insert failed";
    return ok;
}

std::vector<MemoryDb::LinkRow> MemoryDb::links_from(long long unit_id,
                                                     const std::string& status,
                                                     std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT id, source_id, target_id, link_type, status, reliability_scope,"
        " reliability_outcome, reliability_validation_revision, condition, guidance,"
        " pitfalls, evidence_version_id, created_at FROM memory_links"
        " WHERE source_id = ? AND status = ? ORDER BY id;",
    error, "prepare");
    if (!stmt) {
        error = "prepare link fetch failed";
        return {};
    }
    sqlite3_bind_int64(stmt, 1, unit_id);
    bind_text(stmt, 2, status.empty() ? "active" : status);
    std::vector<LinkRow> rows;
    while (step_row(stmt, "link fetch") == 1) {
        LinkRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.source_id = sqlite3_column_int64(stmt, 1);
        row.target_id = sqlite3_column_int64(stmt, 2);
        row.link_type = column_text(stmt, 3);
        row.status = column_text(stmt, 4);
        row.reliability_scope = column_text(stmt, 5);
        row.reliability_outcome = column_text(stmt, 6);
        row.reliability_revision = column_text(stmt, 7);
        row.condition = column_text(stmt, 8);
        row.guidance = column_text(stmt, 9);
        row.pitfalls = column_text(stmt, 10);
        row.evidence_version_id = sqlite3_column_int64(stmt, 11);
        row.created_at = column_text(stmt, 12);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

// ── Feedback ─────────────────────────────────────────────────────────────

void MemoryDb::record_offered(const std::vector<long long>& unit_ids,
                              const std::string& turn_id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_ids.empty()) return;
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "INSERT INTO memory_feedback (turn_id, memory_id, offered_at) VALUES (?, ?, ?);",
    error, "prepare");
    if (!stmt) {
        error = "prepare feedback insert failed";
        return;
    }
    bind_text(stmt, 1, turn_id);
    bind_text(stmt, 3, now_utc());
    for (const long long unit_id : unit_ids) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 2, unit_id);
        if (step_row(stmt, "feedback insert") != 0) {
            error = "feedback insert failed";
            break;
        }
    }
    sqlite3_finalize(stmt);
}

int MemoryDb::record_used(const std::vector<long long>& unit_ids,
                          const std::string& turn_id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_ids.empty()) return 0;
    // Validation lives in the WHERE clause: only rows that were offered on
    // this turn can be marked used, and a second call on the same turn is a
    // no-op (used_at already set).
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "UPDATE memory_feedback SET used_at = ? WHERE turn_id = ? AND memory_id = ?"
        " AND offered_at != '' AND used_at = '';",
        error, "prepare");
    if (!stmt) {
        error = "prepare feedback update failed";
        return 0;
    }
    bind_text(stmt, 1, now_utc());
    bind_text(stmt, 2, turn_id);
    int marked = 0;
    for (const long long unit_id : unit_ids) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 3, unit_id);
        if (step_row(stmt, "feedback update") != 0) {
            error = "feedback update failed";
            break;
        }
        const int changes = static_cast<int>(sqlite3_changes(db_));
        marked += changes;
        if (changes > 0) {
            // A low-trust use claim: a smaller confidence nudge than a citation.
            std::string bump_error;
            apply_confidence_delta(unit_id, 0.05, bump_error);
            if (!bump_error.empty() && error.empty()) error = bump_error;
        }
    }
    sqlite3_finalize(stmt);
    return marked;
}

int MemoryDb::record_cited(const std::vector<long long>& unit_ids,
                           const std::string& turn_id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_ids.empty()) return 0;
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "UPDATE memory_feedback SET cited_at = ? WHERE turn_id = ? AND memory_id = ?"
        " AND offered_at != '' AND cited_at = '';",
        error, "prepare");
    if (!stmt) {
        error = "prepare feedback update failed";
        return 0;
    }
    bind_text(stmt, 1, now_utc());
    bind_text(stmt, 2, turn_id);
    int marked = 0;
    for (const long long unit_id : unit_ids) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 3, unit_id);
        if (step_row(stmt, "feedback update") != 0) {
            error = "feedback update failed";
            break;
        }
        const int changes = static_cast<int>(sqlite3_changes(db_));
        marked += changes;
        if (changes > 0) {
            // A citation is verifiable evidence the memory was relied on: the
            // strongest positive signal, so it raises confidence the most.
            std::string bump_error;
            apply_confidence_delta(unit_id, 0.10, bump_error);
            if (!bump_error.empty() && error.empty()) error = bump_error;
        }
    }
    sqlite3_finalize(stmt);
    return marked;
}

int MemoryDb::record_acted(const std::vector<long long>& unit_ids,
                           const std::string& turn_id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_ids.empty()) return 0;
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "UPDATE memory_feedback SET acted_on_at = ? WHERE turn_id = ? AND memory_id = ?"
        " AND offered_at != '' AND acted_on_at = '';",
        error, "prepare");
    if (!stmt) {
        error = "prepare feedback update failed";
        return 0;
    }
    bind_text(stmt, 1, now_utc());
    bind_text(stmt, 2, turn_id);
    int marked = 0;
    for (const long long unit_id : unit_ids) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 3, unit_id);
        if (step_row(stmt, "feedback update") != 0) {
            error = "feedback update failed";
            break;
        }
        const int changes = static_cast<int>(sqlite3_changes(db_));
        marked += changes;
        if (changes > 0) {
            // A low-trust acted-on claim: a smaller confidence nudge.
            std::string bump_error;
            apply_confidence_delta(unit_id, 0.05, bump_error);
            if (!bump_error.empty() && error.empty()) error = bump_error;
        }
    }
    sqlite3_finalize(stmt);
    return marked;
}

MemoryDb::FeedbackSummary MemoryDb::feedback_summary(long long unit_id,
                                                     std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    FeedbackSummary summary;
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT COUNT(*), MAX(offered_at),"
        " SUM(CASE WHEN cited_at != '' THEN 1 ELSE 0 END), MAX(cited_at),"
        " SUM(CASE WHEN used_at != '' THEN 1 ELSE 0 END), MAX(used_at),"
        " SUM(CASE WHEN acted_on_at != '' THEN 1 ELSE 0 END), MAX(acted_on_at)"
        " FROM memory_feedback WHERE memory_id = ?;",
        error, "prepare");
    if (!stmt) {
        error = "prepare feedback summary failed";
        return summary;
    }
    sqlite3_bind_int64(stmt, 1, unit_id);
    if (step_row(stmt, "feedback summary") == 1) {
        summary.offered = sqlite3_column_int64(stmt, 0);
        if (summary.offered > 0) summary.last_offered_at = column_text(stmt, 1);
        summary.cited = sqlite3_column_int64(stmt, 2);
        if (summary.cited > 0) summary.last_cited_at = column_text(stmt, 3);
        summary.used = sqlite3_column_int64(stmt, 4);
        if (summary.used > 0) summary.last_used_at = column_text(stmt, 5);
        summary.acted_on = sqlite3_column_int64(stmt, 6);
        if (summary.acted_on > 0) summary.last_acted_on_at = column_text(stmt, 7);
    }
    sqlite3_finalize(stmt);
    return summary;
}

// ── Sweeps ───────────────────────────────────────────────────────────────

std::optional<MemoryDb::SweepRow> MemoryDb::last_sweep(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT id, trigger_name, watermark, selected, changes, fixpoint, finished_at"
        " FROM memory_sweeps ORDER BY id DESC LIMIT 1;",
    error, "prepare");
    if (!stmt) {
        error = "prepare sweep fetch failed";
        return std::nullopt;
    }
    std::optional<SweepRow> row;
    if (step_row(stmt, "sweep fetch") == 1) {
        SweepRow r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.trigger = column_text(stmt, 1);
        r.watermark = column_text(stmt, 2);
        r.selected = sqlite3_column_int64(stmt, 3);
        r.changes = sqlite3_column_int64(stmt, 4);
        r.fixpoint = sqlite3_column_int(stmt, 5) != 0;
        r.finished_at = column_text(stmt, 6);
        row = std::move(r);
    }
    sqlite3_finalize(stmt);
    return row;
}

void MemoryDb::record_sweep(const std::string& trigger, const std::string& watermark,
                            long long selected, long long changes, bool fixpoint,
                            std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "INSERT INTO memory_sweeps (trigger_name, watermark, selected, changes, fixpoint,"
        " finished_at) VALUES (?, ?, ?, ?, ?, ?);",
    error, "prepare");
    if (!stmt) {
        error = "prepare sweep insert failed";
        return;
    }
    bind_text(stmt, 1, trigger);
    bind_text(stmt, 2, watermark);
    sqlite3_bind_int64(stmt, 3, selected);
    sqlite3_bind_int64(stmt, 4, changes);
    sqlite3_bind_int(stmt, 5, fixpoint ? 1 : 0);
    bind_text(stmt, 6, now_utc());
    if (step_row(stmt, "sweep insert") != 0) error = "sweep insert failed";
    sqlite3_finalize(stmt);
}

std::vector<MemoryDb::UnitRow> MemoryDb::active_units_updated_since(
    const std::string& watermark, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT id, type, state, state_reason, scope, session_id, profile, run_id,"
        " current_version_id, confidence, mentioned_at, occurred_start, occurred_end,"
        " last_consolidated_at, core, created_at, updated_at FROM memory_units"
        " WHERE state = 'active' AND updated_at > ? ORDER BY updated_at;",
    error, "prepare");
    if (!stmt) {
        error = "prepare sweep selection failed";
        return {};
    }
    bind_text(stmt, 1, watermark);
    std::vector<UnitRow> rows;
    while (step_row(stmt, "sweep selection") == 1) {
        UnitRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.type = column_text(stmt, 1);
        row.state = column_text(stmt, 2);
        row.state_reason = column_text(stmt, 3);
        row.scope = column_text(stmt, 4);
        row.session_id = column_text(stmt, 5);
        row.profile = column_text(stmt, 6);
        row.run_id = column_text(stmt, 7);
        row.current_version_id = sqlite3_column_int64(stmt, 8);
        row.confidence = sqlite3_column_double(stmt, 9);
        row.mentioned_at = column_text(stmt, 10);
        row.occurred_start = column_text(stmt, 11);
        row.occurred_end = column_text(stmt, 12);
        row.last_consolidated_at = column_text(stmt, 13);
        row.core = sqlite3_column_int(stmt, 14) != 0;
        row.created_at = column_text(stmt, 15);
        row.updated_at = column_text(stmt, 16);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

void MemoryDb::mark_consolidated(long long unit_id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "UPDATE memory_units SET last_consolidated_at = ?, updated_at = ? WHERE id = ?;",
    error, "prepare");
    if (!stmt) {
        error = "prepare consolidation mark failed";
        return;
    }
    bind_text(stmt, 1, now_utc());
    bind_text(stmt, 2, now_utc());
    sqlite3_bind_int64(stmt, 3, unit_id);
    if (step_row(stmt, "consolidation mark") != 0) error = "consolidation mark failed";
    sqlite3_finalize(stmt);
}

bool MemoryDb::set_core(long long unit_id, bool on, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_, "UPDATE memory_units SET core = ?, updated_at = ? WHERE id = ?;", error, "prepare");
    if (!stmt) {
        error = "prepare core update failed";
        return false;
    }
    sqlite3_bind_int(stmt, 1, on ? 1 : 0);
    bind_text(stmt, 2, now_utc());
    sqlite3_bind_int64(stmt, 3, unit_id);
    if (step_row(stmt, "core update") != 0) {
        error = "core update failed";
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return sqlite3_changes(db_) > 0;
}

std::vector<MemoryDb::UnitRow> MemoryDb::core_units(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT id, type, state, state_reason, scope, session_id, profile, run_id,"
        " current_version_id, confidence, mentioned_at, occurred_start, occurred_end,"
        " last_consolidated_at, core, created_at, updated_at FROM memory_units"
        " WHERE state = 'active' AND core = 1 ORDER BY id;",
    error, "prepare");
    if (!stmt) {
        error = "prepare core scan failed";
        return {};
    }
    std::vector<UnitRow> rows;
    while (step_row(stmt, "core scan") == 1) {
        UnitRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.type = column_text(stmt, 1);
        row.state = column_text(stmt, 2);
        row.state_reason = column_text(stmt, 3);
        row.scope = column_text(stmt, 4);
        row.session_id = column_text(stmt, 5);
        row.profile = column_text(stmt, 6);
        row.run_id = column_text(stmt, 7);
        row.current_version_id = sqlite3_column_int64(stmt, 8);
        row.confidence = sqlite3_column_double(stmt, 9);
        row.mentioned_at = column_text(stmt, 10);
        row.occurred_start = column_text(stmt, 11);
        row.occurred_end = column_text(stmt, 12);
        row.last_consolidated_at = column_text(stmt, 13);
        row.core = true;
        row.created_at = column_text(stmt, 14);
        row.updated_at = column_text(stmt, 15);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

long long MemoryDb::core_count(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_, "SELECT COUNT(*) FROM memory_units WHERE state = 'active' AND core = 1;",
        error, "prepare");
    if (!stmt) {
        error = "prepare core count failed";
        return 0;
    }
    long long count = 0;
    if (step_row(stmt, "core count") == 1) count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// ── Consolidation candidates (approval gate) ──────────────────────────────

long long MemoryDb::add_consolidation(long long older_unit_id, long long newer_unit_id,
                                      double cosine, const std::string& scope,
                                      std::string& error, const std::string& kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "INSERT INTO memory_consolidations (older_unit_id, newer_unit_id, cosine, scope,"
        " kind, created_at) VALUES (?, ?, ?, ?, ?, ?);",
    error, "prepare");
    if (!stmt) {
        error = "prepare consolidation insert failed";
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, older_unit_id);
    sqlite3_bind_int64(stmt, 2, newer_unit_id);
    sqlite3_bind_double(stmt, 3, cosine);
    bind_text(stmt, 4, scope);
    bind_text(stmt, 5, kind);
    bind_text(stmt, 6, now_utc());
    if (step_row(stmt, "consolidation insert") != 0) {
        error = "consolidation insert failed";
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    sqlite3_stmt* id_stmt =
        prepare_stmt(db_, "SELECT last_insert_rowid();", error, "prepare");
    if (!id_stmt) {
        error = "prepare consolidation id fetch failed";
        return 0;
    }
    long long id = 0;
    if (step_row(id_stmt, "consolidation id fetch") == 1) {
        id = sqlite3_column_int64(id_stmt, 0);
    }
    sqlite3_finalize(id_stmt);
    return id;
}

std::vector<MemoryDb::ConsolidationRow> MemoryDb::list_consolidations(
    const std::string& status) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConsolidationRow> rows;
    std::string perr;
    const std::string sql = status.empty()
        ? "SELECT id, older_unit_id, newer_unit_id, cosine, scope, kind, status,"
          " created_at, decided_at FROM memory_consolidations ORDER BY id DESC;"
        : "SELECT id, older_unit_id, newer_unit_id, cosine, scope, kind, status,"
          " created_at, decided_at FROM memory_consolidations WHERE status = ?"
          " ORDER BY id DESC;";
    sqlite3_stmt* stmt = prepare_stmt(db_, sql.c_str(), perr, "prepare");
    if (!stmt) return rows;
    if (!status.empty()) bind_text(stmt, 1, status);
    while (step_row(stmt, "consolidation list") == 1) {
        ConsolidationRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.older_unit_id = sqlite3_column_int64(stmt, 1);
        row.newer_unit_id = sqlite3_column_int64(stmt, 2);
        row.cosine = sqlite3_column_double(stmt, 3);
        row.scope = column_text(stmt, 4);
        row.kind = column_text(stmt, 5);
        row.status = column_text(stmt, 6);
        row.created_at = column_text(stmt, 7);
        row.decided_at = column_text(stmt, 8);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::optional<MemoryDb::ConsolidationRow> MemoryDb::get_consolidation(long long id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string perr;
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT id, older_unit_id, newer_unit_id, cosine, scope, kind, status,"
        " created_at, decided_at FROM memory_consolidations WHERE id = ?;",
        perr, "prepare");
    if (!stmt) return std::nullopt;
    sqlite3_bind_int64(stmt, 1, id);
    std::optional<ConsolidationRow> row;
    if (step_row(stmt, "consolidation fetch") == 1) {
        ConsolidationRow r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.older_unit_id = sqlite3_column_int64(stmt, 1);
        r.newer_unit_id = sqlite3_column_int64(stmt, 2);
        r.cosine = sqlite3_column_double(stmt, 3);
        r.scope = column_text(stmt, 4);
        r.kind = column_text(stmt, 5);
        r.status = column_text(stmt, 6);
        r.created_at = column_text(stmt, 7);
        r.decided_at = column_text(stmt, 8);
        row = std::move(r);
    }
    sqlite3_finalize(stmt);
    return row;
}

bool MemoryDb::set_consolidation_status(long long id, const std::string& status,
                                        std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "UPDATE memory_consolidations SET status = ?, decided_at = ? WHERE id = ?;",
    error, "prepare");
    if (!stmt) {
        error = "prepare consolidation update failed";
        return false;
    }
    bind_text(stmt, 1, status);
    bind_text(stmt, 2, now_utc());
    sqlite3_bind_int64(stmt, 3, id);
    if (step_row(stmt, "consolidation update") != 0) {
        error = "consolidation update failed";
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    sqlite3_stmt* chk = prepare_stmt(
        db_, "SELECT COUNT(*) FROM memory_consolidations WHERE id = ?;", error, "prepare");
    if (!chk) return false;
    sqlite3_bind_int64(chk, 1, id);
    bool exists = false;
    if (step_row(chk, "consolidation check") == 1) {
        exists = sqlite3_column_int64(chk, 0) > 0;
    }
    sqlite3_finalize(chk);
    return exists;
}

bool MemoryDb::has_consolidation_for_pair(long long older_unit_id,
                                          long long newer_unit_id,
                                          std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT COUNT(*) FROM memory_consolidations"
        " WHERE older_unit_id = ? AND newer_unit_id = ?;",
        error, "prepare");
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, older_unit_id);
    sqlite3_bind_int64(stmt, 2, newer_unit_id);
    bool found = false;
    if (step_row(stmt, "consolidation pair check") == 1) {
        found = sqlite3_column_int64(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    return found;
}

std::map<std::string, long long> MemoryDb::consolidation_status_counts(
    std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, long long> counts;
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT status, COUNT(*) FROM memory_consolidations GROUP BY status;",
        error, "prepare");
    if (!stmt) return counts;
    while (step_row(stmt, "consolidation counts") == 1) {
        counts[column_text(stmt, 0)] = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return counts;
}

// ── Journey (per-session narrative) ─────────────────────────────────────

std::optional<MemoryDb::JourneyRow> MemoryDb::journey_for(
    const std::string& session_id, std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT session_id, profile, text, watermark, created_at, updated_at"
        " FROM journeys WHERE session_id = ?;",
        error, "prepare journey fetch");
    if (!stmt) return std::nullopt;
    bind_text(stmt, 1, session_id);
    std::optional<JourneyRow> row;
    if (step_row(stmt, "journey fetch") == 1) {
        JourneyRow j;
        j.session_id = session_id;
        j.profile = column_text(stmt, 1);
        j.text = column_text(stmt, 2);
        j.watermark = sqlite3_column_int64(stmt, 3);
        j.created_at = column_text(stmt, 4);
        j.updated_at = column_text(stmt, 5);
        row = std::move(j);
    }
    sqlite3_finalize(stmt);
    return row;
}

bool MemoryDb::upsert_journey(const std::string& session_id,
                              const std::string& profile, const std::string& text,
                              long long watermark, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string now = now_utc();
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "INSERT INTO journeys (session_id, profile, text, watermark, created_at,"
        " updated_at) VALUES (?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(session_id) DO UPDATE SET"
        " profile = excluded.profile, text = excluded.text,"
        " watermark = excluded.watermark, updated_at = excluded.updated_at;",
        error, "prepare journey upsert");
    if (!stmt) return false;
    bind_text(stmt, 1, session_id);
    bind_text(stmt, 2, profile);
    bind_text(stmt, 3, text);
    sqlite3_bind_int64(stmt, 4, watermark);
    bind_text(stmt, 5, now);
    bind_text(stmt, 6, now);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<MemoryDb::JourneyRow> MemoryDb::latest_journey_for_profile(
    const std::string& profile, const std::string& exclude_sid,
    std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (profile.empty()) return std::nullopt;
    sqlite3_stmt* stmt = prepare_stmt(
        db_,
        "SELECT session_id, profile, text, watermark, created_at, updated_at"
        " FROM journeys WHERE profile = ? AND session_id != ?"
        " ORDER BY updated_at DESC, session_id DESC LIMIT 1;",
        error, "prepare journey profile fetch");
    if (!stmt) return std::nullopt;
    bind_text(stmt, 1, profile);
    bind_text(stmt, 2, exclude_sid);
    std::optional<JourneyRow> row;
    if (step_row(stmt, "journey profile fetch") == 1) {
        JourneyRow j;
        j.session_id = column_text(stmt, 0);
        j.profile = column_text(stmt, 1);
        j.text = column_text(stmt, 2);
        j.watermark = sqlite3_column_int64(stmt, 3);
        j.created_at = column_text(stmt, 4);
        j.updated_at = column_text(stmt, 5);
        row = std::move(j);
    }
    sqlite3_finalize(stmt);
    return row;
}

bool MemoryDb::delete_journey(const std::string& session_id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(db_, "DELETE FROM journeys WHERE session_id = ?;",
                                     error, "prepare journey delete");
    if (!stmt) return false;
    bind_text(stmt, 1, session_id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

long long MemoryDb::journey_count(std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = prepare_stmt(db_, "SELECT COUNT(*) FROM journeys;", error,
                                     "prepare journey count");
    if (!stmt) return 0;
    long long count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// ── Status ───────────────────────────────────────────────────────────────────────

namespace {
long long scalar_count(sqlite3* db, const char* sql, std::string& error) {
    sqlite3_stmt* stmt = prepare_stmt(db, sql, error, "prepare");
    if (!stmt) {
        error = "prepare count failed";
        return 0;
    }
    long long count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}
}  // namespace

std::map<std::string, long long> MemoryDb::unit_state_counts(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, long long> counts;
    sqlite3_stmt* stmt = prepare_stmt(
        db_, "SELECT state, COUNT(*) FROM memory_units GROUP BY state;", error, "prepare");
    if (!stmt) {
        error = "prepare state counts failed";
        return counts;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        counts[column_text(stmt, 0)] = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return counts;
}

long long MemoryDb::version_count(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return scalar_count(db_, "SELECT COUNT(*) FROM memory_versions;", error);
}

long long MemoryDb::embedding_count(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return scalar_count(db_, "SELECT COUNT(*) FROM memory_embeddings;", error);
}

long long MemoryDb::fts_row_count(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return scalar_count(db_, "SELECT COUNT(*) FROM memory_fts;", error);
}

long long MemoryDb::link_count(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return scalar_count(db_, "SELECT COUNT(*) FROM memory_links;", error);
}

long long MemoryDb::feedback_count(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return scalar_count(db_, "SELECT COUNT(*) FROM memory_feedback;", error);
}

long long MemoryDb::sweep_count(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return scalar_count(db_, "SELECT COUNT(*) FROM memory_sweeps;", error);
}

}  // namespace nyaxus
