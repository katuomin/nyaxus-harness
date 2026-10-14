#include "nyaxus/store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
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

// SHA-256 (FIPS 180-4), hex-encoded. Content addressing for attachment
// dedup; the core has no other crypto dependency.
constexpr std::uint32_t kSha256Constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline std::uint32_t sha256_rotr(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (character < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(character) << std::dec;
                } else {
                    out << static_cast<char>(character);
                }
                break;
        }
    }
    return out.str();
}

// Encode a string-map payload as a compact JSON object.
std::string payload_json(const std::map<std::string, std::string>& payload) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto& [key, value] : payload) {
        if (!first) out << ",";
        first = false;
        out << "\"" << json_escape(key) << "\":\"" << json_escape(value) << "\"";
    }
    out << "}";
    return out.str();
}

std::map<std::string, std::string> parse_payload(const std::string& text) {
    std::map<std::string, std::string> payload;
    // Very small subset parser for {"k":"v",...}; values are unescaped strings.
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t key_start = text.find('"', cursor);
        if (key_start == std::string::npos) break;
        const std::size_t key_end = text.find('"', key_start + 1);
        if (key_end == std::string::npos) break;
        const std::string key = text.substr(key_start + 1, key_end - key_start - 1);
        const std::size_t value_marker = text.find(":\"", key_end);
        if (value_marker == std::string::npos) break;
        const std::size_t value_start = value_marker + 2;
        std::size_t value_end = value_start;
        bool escaped = false;
        while (value_end < text.size()) {
            if (!escaped && text[value_end] == '"') break;
            if (!escaped && text[value_end] == '\\') {
                escaped = true;
            } else {
                escaped = false;
            }
            ++value_end;
        }
        std::string value = text.substr(value_start, value_end - value_start);
        std::string unescaped;
        bool value_escaped = false;
        for (const char character : value) {
            if (value_escaped) {
                unescaped.push_back(character == 'n'   ? '\n'
                                    : character == 't' ? '\t'
                                    : character == 'r' ? '\r'
                                                       : character);
                value_escaped = false;
            } else if (character == '\\') {
                value_escaped = true;
            } else {
                unescaped.push_back(character);
            }
        }
        payload[key] = unescaped;
        cursor = value_end + 1;
    }
    return payload;
}

}  // namespace

// SHA-256 (FIPS 180-4), hex-encoded. Content addressing for attachment
// dedup; the core has no other crypto dependency.
std::string sha256_hex(const std::string& input) {
    std::uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    std::string message = input;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) message.push_back(0);
    const unsigned long long bit_length =
        static_cast<unsigned long long>(input.size()) * 8ull;
    for (int index = 7; index >= 0; --index) {
        message.push_back(static_cast<char>((bit_length >> (index * 8)) & 0xff));
    }
    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t words[64];
        for (int t = 0; t < 16; ++t) {
            const std::size_t base = offset + static_cast<std::size_t>(t) * 4;
            words[t] = (static_cast<std::uint32_t>(static_cast<unsigned char>(message[base])) << 24) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(message[base + 1])) << 16) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(message[base + 2])) << 8) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(message[base + 3])));
        }
        for (int t = 16; t < 64; ++t) {
            const std::uint32_t s0 = sha256_rotr(words[t - 15], 7) ^
                                     sha256_rotr(words[t - 15], 18) ^ (words[t - 15] >> 3);
            const std::uint32_t s1 = sha256_rotr(words[t - 2], 17) ^
                                     sha256_rotr(words[t - 2], 19) ^ (words[t - 2] >> 10);
            words[t] = words[t - 16] + s0 + words[t - 7] + s1;
        }
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int t = 0; t < 64; ++t) {
            const std::uint32_t big_s1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + big_s1 + choice + kSha256Constants[t] + words[t];
            const std::uint32_t big_s0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = big_s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
    static const char hex_digits[] = "0123456789abcdef";
    std::string digest;
    digest.reserve(64);
    for (int index = 0; index < 8; ++index) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            digest.push_back(hex_digits[(state[index] >> shift) & 0x0f]);
        }
    }
    return digest;
}

SqliteStore::SqliteStore(std::filesystem::path path) : path_(std::move(path)) {
    if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path());
    const int rc = sqlite3_open(path_.string().c_str(), &db_);
    if (rc != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "unknown";
        throw std::runtime_error("could not open sqlite store: " + message);
    }
    const char* schema =
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id TEXT PRIMARY KEY,"
        "  profile TEXT NOT NULL DEFAULT 'orchestrator',"
        "  model TEXT NOT NULL DEFAULT '',"
        "  title TEXT NOT NULL DEFAULT 'new conversation',"
        "  updated_at TEXT NOT NULL,"
        "  context_mode TEXT NOT NULL DEFAULT 'selected',"
        "  context_status TEXT NOT NULL DEFAULT 'ready',"
        "  context_summary TEXT NOT NULL DEFAULT '',"
        "  context_error TEXT NOT NULL DEFAULT '',"
        "  source_session_id TEXT NOT NULL DEFAULT '',"
        "  context_watermark INTEGER NOT NULL DEFAULT 0,"
        "  profile_revision INTEGER NOT NULL DEFAULT 0,"
        "  profile_snapshot TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  parent_id INTEGER NOT NULL DEFAULT 0,"
        "  session_id TEXT NOT NULL,"
        "  role TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  run_id TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL,"
        "  reasoning TEXT NOT NULL DEFAULT '',"
        "  is_active INTEGER NOT NULL DEFAULT 1"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id);"
        "CREATE TABLE IF NOT EXISTS attachments ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  message_id INTEGER NOT NULL,"
        "  name TEXT NOT NULL,"
        "  kind TEXT NOT NULL DEFAULT 'text',"
        "  mime_type TEXT NOT NULL DEFAULT '',"
        "  size INTEGER NOT NULL DEFAULT 0,"
        "  width INTEGER,"
        "  height INTEGER,"
        "  content TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_attachments_message ON attachments(message_id);"
        "CREATE TABLE IF NOT EXISTS events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  event_id TEXT NOT NULL,"
        "  session_id TEXT NOT NULL,"
        "  run_id TEXT NOT NULL,"
        "  parent_run_id TEXT NOT NULL DEFAULT '',"
        "  seq INTEGER NOT NULL,"
        "  event_type TEXT NOT NULL,"
        "  emitted_at TEXT NOT NULL,"
        "  payload TEXT NOT NULL DEFAULT '{}'"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_events_run ON events(run_id);"
        "CREATE INDEX IF NOT EXISTS idx_events_session ON events(session_id);"
        "CREATE TABLE IF NOT EXISTS profiles ("
        "  id TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  description TEXT NOT NULL DEFAULT '',"
        "  identity_prompt TEXT NOT NULL DEFAULT '',"
        "  base_model TEXT NOT NULL DEFAULT '',"
        "  vision INTEGER NOT NULL DEFAULT 0,"
        "  temperature REAL,"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL,"
        "  revision INTEGER NOT NULL DEFAULT 1"
        ");"
        "CREATE TABLE IF NOT EXISTS settings ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS providers ("
        "  name TEXT PRIMARY KEY,"
        "  endpoint TEXT NOT NULL DEFAULT '',"
        "  model TEXT NOT NULL DEFAULT '',"
        "  api_key TEXT NOT NULL DEFAULT '',"
        "  params TEXT NOT NULL DEFAULT '{}',"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS memory_outbox ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  idempotency_key TEXT NOT NULL UNIQUE,"
        "  provider TEXT NOT NULL DEFAULT 'hindsight',"
        "  operation TEXT NOT NULL,"
        "  payload TEXT NOT NULL,"
        "  status TEXT NOT NULL DEFAULT 'pending',"
        "  retry_count INTEGER NOT NULL DEFAULT 0,"
        "  next_attempt_at TEXT NOT NULL DEFAULT '',"
        "  last_error TEXT NOT NULL DEFAULT '',"
        "  provider_operation_id TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_memory_outbox_due ON memory_outbox(status, next_attempt_at, id);"
        "CREATE TABLE IF NOT EXISTS memory_suppressions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  key TEXT NOT NULL UNIQUE,"
        "  reason TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS memory_records ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  status TEXT NOT NULL DEFAULT 'candidate',"
        "  type TEXT NOT NULL DEFAULT 'fact',"
        "  text TEXT NOT NULL,"
        "  scope TEXT NOT NULL DEFAULT 'session',"
        "  session_id TEXT NOT NULL DEFAULT '',"
        "  profile TEXT NOT NULL DEFAULT '',"
        "  run_id TEXT NOT NULL DEFAULT '',"
        "  source_message_ids TEXT NOT NULL DEFAULT '',"
        "  provenance TEXT NOT NULL DEFAULT '',"
        "  contradicts_id INTEGER NOT NULL DEFAULT 0,"
        "  superseded_by_id INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL,"
        "  decided_at TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_memory_records_status ON memory_records(status, id);"
        "CREATE TABLE IF NOT EXISTS prompt_packs ("
        "  id TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  description TEXT NOT NULL DEFAULT '',"
        "  revision INTEGER NOT NULL DEFAULT 1,"
        "  validation_status TEXT NOT NULL DEFAULT 'unchecked',"
        "  fingerprint TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS prompt_pack_files ("
        "  pack_id TEXT NOT NULL,"
        "  logical_path TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  content_hash TEXT NOT NULL DEFAULT '',"
        "  revision INTEGER NOT NULL DEFAULT 1,"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL,"
        "  PRIMARY KEY (pack_id, logical_path)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_prompt_pack_files_pack ON prompt_pack_files(pack_id);"
        "CREATE TABLE IF NOT EXISTS pack_snapshots ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  pack_id TEXT NOT NULL,"
        "  revision INTEGER NOT NULL,"
        "  document TEXT NOT NULL,"
        "  fingerprint TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL,"
        "  UNIQUE(pack_id, revision, fingerprint)"
        ");"
        "CREATE TABLE IF NOT EXISTS status_events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id TEXT NOT NULL,"
        "  profile TEXT NOT NULL DEFAULT '',"
        "  state TEXT NOT NULL,"
        "  note TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_status_events_session ON status_events(session_id, id);"
        "CREATE TABLE IF NOT EXISTS sources ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id TEXT NOT NULL,"
        "  run_id TEXT NOT NULL DEFAULT '',"
        "  url TEXT NOT NULL,"
        "  title TEXT NOT NULL DEFAULT '',"
        "  content_hash TEXT NOT NULL DEFAULT '',"
        "  bytes_fetched INTEGER NOT NULL DEFAULT 0,"
        "  status TEXT NOT NULL DEFAULT 'ok',"
        "  error TEXT NOT NULL DEFAULT '',"
        "  used INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_sources_session ON sources(session_id, id);"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_sources_session_url ON sources(session_id, url);"
        "CREATE TABLE IF NOT EXISTS always_allowed ("
        "  tool TEXT PRIMARY KEY,"
        "  pattern TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS skills_enabled ("
        "  name TEXT PRIMARY KEY,"
        "  enabled INTEGER NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");";
    char* error = nullptr;
    const int exec_rc = sqlite3_exec(db_, schema, nullptr, nullptr, &error);
    if (exec_rc != SQLITE_OK) {
        const std::string message = error ? error : "unknown";
        sqlite3_free(error);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("could not initialize sqlite store: " + message);
    }
    // Migration: add the reasoning and is_active columns to pre-existing databases.
    sqlite3_stmt* probe = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT reasoning FROM messages LIMIT 1", -1, &probe,
                           nullptr) != SQLITE_OK) {
        if (probe) sqlite3_finalize(probe);
        sqlite3_exec(db_, "ALTER TABLE messages ADD COLUMN reasoning TEXT NOT NULL "
                          "DEFAULT ''",
                     nullptr, nullptr, nullptr);
    } else {
        sqlite3_finalize(probe);
    }
    probe = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT is_active FROM messages LIMIT 1", -1, &probe,
                           nullptr) != SQLITE_OK) {
        if (probe) sqlite3_finalize(probe);
        sqlite3_exec(db_, "ALTER TABLE messages ADD COLUMN is_active INTEGER NOT NULL "
                          "DEFAULT 1",
                     nullptr, nullptr, nullptr);
    } else {
        sqlite3_finalize(probe);
    }
    probe = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT parent_id FROM messages LIMIT 1", -1, &probe,
                           nullptr) != SQLITE_OK) {
        if (probe) sqlite3_finalize(probe);
        sqlite3_exec(db_, "ALTER TABLE messages ADD COLUMN parent_id INTEGER NOT NULL "
                          "DEFAULT 0",
                     nullptr, nullptr, nullptr);
        // Backfill: treat the existing linear order as a single chain.
        sqlite3_exec(db_,
                     "UPDATE messages SET parent_id = (SELECT COALESCE(MAX(m2.id), 0) "
                     "FROM messages m2 WHERE m2.session_id = messages.session_id AND "
                     "m2.id < messages.id)",
                     nullptr, nullptr, nullptr);
    } else {
        sqlite3_finalize(probe);
    }

    // Migrate columns introduced after the initial sessions/messages schema.
    // Probing each column keeps this compatible with databases created by any
    // earlier backend slice, including databases where the table already exists.
    const auto ensure_column = [&](const char* table, const char* column,
                                   const char* definition) {
        sqlite3_stmt* column_probe = nullptr;
        const std::string query = "SELECT " + std::string(column) + " FROM " +
                                  table + " LIMIT 1";
        if (sqlite3_prepare_v2(db_, query.c_str(), -1, &column_probe, nullptr) != SQLITE_OK) {
            if (column_probe) sqlite3_finalize(column_probe);
            const std::string alter = "ALTER TABLE " + std::string(table) +
                                      " ADD COLUMN " + column + " " + definition;
            sqlite3_exec(db_, alter.c_str(), nullptr, nullptr, nullptr);
        } else {
            sqlite3_finalize(column_probe);
        }
    };
    ensure_column("sessions", "context_mode", "TEXT NOT NULL DEFAULT 'selected'");
    ensure_column("sessions", "context_status", "TEXT NOT NULL DEFAULT 'ready'");
    ensure_column("sessions", "context_summary", "TEXT NOT NULL DEFAULT ''");
    ensure_column("sessions", "context_error", "TEXT NOT NULL DEFAULT ''");
    ensure_column("sessions", "source_session_id", "TEXT NOT NULL DEFAULT ''");
    ensure_column("sessions", "context_watermark", "INTEGER NOT NULL DEFAULT 0");
    ensure_column("sessions", "md_log_path", "TEXT NOT NULL DEFAULT ''");
    ensure_column("profiles", "vision", "INTEGER NOT NULL DEFAULT 0");
    ensure_column("attachments", "mime_type", "TEXT NOT NULL DEFAULT ''");
    ensure_column("attachments", "width", "INTEGER");
    ensure_column("attachments", "height", "INTEGER");
    ensure_column("attachments", "content_hash", "TEXT NOT NULL DEFAULT ''");
    ensure_column("always_allowed", "pattern", "TEXT NOT NULL DEFAULT ''");
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_attachments_content_hash "
                      "ON attachments(content_hash)",
                 nullptr, nullptr, nullptr);
    ensure_column("sessions", "profile_revision", "INTEGER NOT NULL DEFAULT 0");
    ensure_column("sessions", "profile_snapshot", "TEXT NOT NULL DEFAULT ''");
    ensure_column("sessions", "pack_snapshot_id", "INTEGER NOT NULL DEFAULT 0");
    ensure_column("sessions", "pack_revision", "INTEGER NOT NULL DEFAULT 0");
    ensure_column("sessions", "tags", "TEXT NOT NULL DEFAULT ''");
    ensure_column("sessions", "parent_session_id", "TEXT NOT NULL DEFAULT ''");
    // Group chat (docs/group-chat.md): ordered speaker profile ids for a
    // group session (comma-separated; empty = ordinary session), and the
    // speaker attribution on a group turn's assistant message.
    ensure_column("sessions", "speakers", "TEXT NOT NULL DEFAULT ''");
    // Per-session round-budget override for groups (0 = global default).
    ensure_column("sessions", "group_rounds_override", "INTEGER NOT NULL DEFAULT 0");
    ensure_column("messages", "speaker", "TEXT NOT NULL DEFAULT ''");
    // A-7: the client's optimistic row id (dedupe anchor for snapshots and
    // SSE replay). Partial unique: 0 is the unanchored default.
    ensure_column("messages", "client_id", "INTEGER NOT NULL DEFAULT 0");
    sqlite3_exec(db_, "CREATE UNIQUE INDEX IF NOT EXISTS idx_messages_client_id "
                      "ON messages(client_id) WHERE client_id != 0",
                 nullptr, nullptr, nullptr);
    ensure_column("profiles", "child_profile_allowlist", "TEXT NOT NULL DEFAULT ''");
    ensure_column("profiles", "revision", "INTEGER NOT NULL DEFAULT 1");
    // Profile -> provider (connection) reference. Empty = the default provider.
    ensure_column("profiles", "provider", "TEXT NOT NULL DEFAULT ''");
    // Profile vision tristate: 0 = unset (inherit the provider's capability).
    ensure_column("profiles", "vision_set", "INTEGER NOT NULL DEFAULT 0");
    // Prompt-pack association: empty = legacy identity_prompt behavior.
    ensure_column("profiles", "prompt_pack_id", "TEXT NOT NULL DEFAULT ''");
    ensure_column("profiles", "prompt_plan_json", "TEXT NOT NULL DEFAULT ''");

    // Migrate pack_snapshots from UNIQUE(pack_id, revision) to
    // UNIQUE(pack_id, revision, fingerprint): the resolved document also
    // depends on the profile (the pack template bakes in the profile name),
    // so two profiles bound to the same pack at the same revision need
    // separate snapshots. Existing rows are unique on the old key, so the
    // copy is 1:1 and the rebuild is idempotent.
    {
        sqlite3_stmt* sql_probe = nullptr;
        bool needs_rebuild = false;
        if (sqlite3_prepare_v2(db_, "SELECT sql FROM sqlite_master WHERE type = 'table' "
                                   "AND name = 'pack_snapshots'",
                               -1, &sql_probe, nullptr) == SQLITE_OK &&
            sqlite3_step(sql_probe) == SQLITE_ROW) {
            const char* table_sql = reinterpret_cast<const char*>(sqlite3_column_text(sql_probe, 0));
            if (table_sql &&
                std::string(table_sql).find("UNIQUE(pack_id, revision)") != std::string::npos &&
                std::string(table_sql).find("revision, fingerprint)") == std::string::npos) {
                needs_rebuild = true;
            }
        }
        if (sql_probe) sqlite3_finalize(sql_probe);
        if (needs_rebuild) {
            sqlite3_exec(db_,
                "CREATE TABLE pack_snapshots_new ("
                "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "  pack_id TEXT NOT NULL,"
                "  revision INTEGER NOT NULL,"
                "  document TEXT NOT NULL,"
                "  fingerprint TEXT NOT NULL DEFAULT '',"
                "  created_at TEXT NOT NULL,"
                "  UNIQUE(pack_id, revision, fingerprint)"
                ");"
                "INSERT INTO pack_snapshots_new (id, pack_id, revision, document, fingerprint, created_at) "
                "SELECT id, pack_id, revision, document, fingerprint, created_at FROM pack_snapshots;"
                "DROP TABLE pack_snapshots;"
                "ALTER TABLE pack_snapshots_new RENAME TO pack_snapshots;",
                nullptr, nullptr, nullptr);
            // One-shot data fix: sessions pinned to a snapshot resolved for a
            // different profile keep the old soul for their life. The document
            // header records the profile name it was resolved under; clear any
            // pin whose header no longer matches the session's profile so the
            // next run re-resolves.
            sqlite3_exec(db_,
                "UPDATE sessions SET pack_snapshot_id = 0, pack_revision = 0 "
                "WHERE pack_snapshot_id > 0 AND "
                "(SELECT substr(s.document, "
                "         instr(s.document, 'profile: ') + 9, "
                "         instr(substr(s.document, instr(s.document, 'profile: ') + 9), char(10)) - 1) "
                " FROM pack_snapshots s WHERE s.id = sessions.pack_snapshot_id) "
                "<> (SELECT p.name FROM profiles p WHERE p.id = sessions.profile)",
                nullptr, nullptr, nullptr);
        }
    }

    // Seed the default profile once; INSERT OR IGNORE keeps it idempotent.
    {
        sqlite3_stmt* stmt = nullptr;
        if (prepare("INSERT OR IGNORE INTO profiles "
                    "(id, name, description, identity_prompt, base_model, vision, temperature, "
                    "created_at, updated_at) "
                    "VALUES (?1, ?2, ?3, ?4, ?5, 0, NULL, ?6, ?7)", &stmt)) {
            const std::string now = now_utc();
            bind_text(stmt, 1, "orchestrator");
            bind_text(stmt, 2, "orchestrator");
            bind_text(stmt, 3, "default session profile");
            bind_text(stmt, 4, "");
            bind_text(stmt, 5, "");
            bind_text(stmt, 6, now);
            bind_text(stmt, 7, now);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    // Fixup: an assistant is superseded (inactive) when the next message in the same
    // session is also an assistant, i.e. it is not the latest iteration of its turn.
    sqlite3_exec(db_,
                 "UPDATE messages SET is_active = 0 WHERE role = 'assistant' AND id IN ("
                 "  SELECT m1.id FROM messages m1 WHERE m1.role = 'assistant' AND EXISTS ("
                 "    SELECT 1 FROM messages m2 WHERE m2.session_id = m1.session_id AND "
                 "    m2.id = (SELECT MIN(id) FROM messages m3 WHERE m3.session_id = "
                 "    m1.session_id AND m3.id > m1.id) AND m2.role = 'assistant'))",
                 nullptr, nullptr, nullptr);
}

SqliteStore::~SqliteStore() {
    if (db_) sqlite3_close(db_);
}

const std::filesystem::path& SqliteStore::path() const {
    return path_;
}

bool SqliteStore::prepare(const char* sql, sqlite3_stmt** stmt) const {
    return sqlite3_prepare_v2(db_, sql, -1, stmt, nullptr) == SQLITE_OK;
}

std::uint64_t SqliteStore::next_sequence(const std::string& run_id) const {
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT COALESCE(MAX(seq), 0) FROM events WHERE run_id = ?1", &stmt)) {
        return 1;
    }
    bind_text(stmt, 1, run_id);
    std::uint64_t current = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) current = static_cast<std::uint64_t>(
        sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
    return current + 1;
}

Event SqliteStore::append_event(std::string session_id,
                                std::string run_id,
                                std::string parent_run_id,
                                std::string event_type,
                                std::map<std::string, std::string> payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t sequence = next_sequence(run_id);
    Event event{1,
                run_id + ":" + std::to_string(sequence),
                std::move(session_id),
                std::move(run_id),
                std::move(parent_run_id),
                sequence,
                std::move(event_type),
                now_utc(),
                std::move(payload)};
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO events (event_id, session_id, run_id, parent_run_id, seq, "
                 "event_type, emitted_at, payload) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                 &stmt)) {
        throw std::runtime_error("prepare event insert failed");
    }
    bind_text(stmt, 1, event.event_id);
    bind_text(stmt, 2, event.session_id);
    bind_text(stmt, 3, event.run_id);
    bind_text(stmt, 4, event.parent_run_id);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(event.sequence));
    bind_text(stmt, 6, event.event_type);
    bind_text(stmt, 7, event.emitted_at);
    bind_text(stmt, 8, payload_json(event.payload));
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    check_sqlite(rc, "event insert");
    event.row_id = static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db_));
    return event;
}

std::vector<Event> SqliteStore::replay_events(const std::string& run_id,
                                              std::uint64_t after_sequence) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT event_id, session_id, run_id, parent_run_id, seq, event_type, "
                 "emitted_at, payload, id FROM events WHERE run_id = ?1 AND seq > ?2 "
                 "ORDER BY seq ASC",
                 &stmt)) {
        return {};
    }
    bind_text(stmt, 1, run_id);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(after_sequence));
    std::vector<Event> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Event event;
        event.event_id = column_text(stmt, 0);
        event.session_id = column_text(stmt, 1);
        event.run_id = column_text(stmt, 2);
        event.parent_run_id = column_text(stmt, 3);
        event.sequence = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
        event.event_type = column_text(stmt, 5);
        event.emitted_at = column_text(stmt, 6);
        event.payload = parse_payload(column_text(stmt, 7));
        event.row_id = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 8));
        result.push_back(std::move(event));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<Event> SqliteStore::replay_session_events(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT event_id, session_id, run_id, parent_run_id, seq, event_type, "
                 "emitted_at, payload, id FROM events WHERE session_id = ?1 "
                 "ORDER BY id ASC",
                 &stmt)) {
        return {};
    }
    bind_text(stmt, 1, session_id);
    std::vector<Event> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Event event;
        event.event_id = column_text(stmt, 0);
        event.session_id = column_text(stmt, 1);
        event.run_id = column_text(stmt, 2);
        event.parent_run_id = column_text(stmt, 3);
        event.sequence = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
        event.event_type = column_text(stmt, 5);
        event.emitted_at = column_text(stmt, 6);
        event.payload = parse_payload(column_text(stmt, 7));
        event.row_id = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 8));
        result.push_back(std::move(event));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<Event> SqliteStore::replay_session_events_after(const std::string& session_id,
                                                            std::uint64_t after_row_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT event_id, session_id, run_id, parent_run_id, seq, event_type, "
                 "emitted_at, payload, id FROM events WHERE session_id = ?1 AND id > ?2 "
                 "ORDER BY id ASC",
                 &stmt)) {
        return {};
    }
    bind_text(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(after_row_id));
    std::vector<Event> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Event event;
        event.event_id = column_text(stmt, 0);
        event.session_id = column_text(stmt, 1);
        event.run_id = column_text(stmt, 2);
        event.parent_run_id = column_text(stmt, 3);
        event.sequence = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
        event.event_type = column_text(stmt, 5);
        event.emitted_at = column_text(stmt, 6);
        event.payload = parse_payload(column_text(stmt, 7));
        event.row_id = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 8));
        result.push_back(std::move(event));
    }
    sqlite3_finalize(stmt);
    return result;
}

void SqliteStore::delete_run_delta_events(const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (prepare("DELETE FROM events WHERE run_id = ?1 AND event_type IN "
                "('reasoning.delta', 'text.delta')",
                &stmt)) {
        bind_text(stmt, 1, run_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<std::string> SqliteStore::inflight_run_ids(
        const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    std::vector<std::string> result;
    if (prepare("SELECT run_id FROM ("
                "  SELECT run_id, event_type,"
                "         ROW_NUMBER() OVER (PARTITION BY run_id ORDER BY id DESC) AS rn"
                "  FROM events"
                "  WHERE session_id = ?1 AND event_type IN "
                "        ('run.accepted','run.queued','run.started',"
                "         'run.completed','run.cancelled','run.failed')"
                ") WHERE rn = 1 AND event_type IN "
                "        ('run.accepted','run.queued','run.started')",
                &stmt)) {
        bind_text(stmt, 1, session_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            result.push_back(column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

std::string SqliteStore::latest_run_event_type(
        const std::string& session_id, const std::string& run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    std::string result;
    if (prepare("SELECT event_type FROM events"
                "  WHERE session_id = ?1 AND run_id = ?2"
                "  AND event_type IN ('run.accepted','run.queued','run.started',"
                "                     'run.completed','run.cancelled','run.failed')"
                "  ORDER BY id DESC LIMIT 1",
                &stmt)) {
        bind_text(stmt, 1, session_id);
        bind_text(stmt, 2, run_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = column_text(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

std::string SqliteStore::continued_payload(
        const std::string& session_id, const std::string& run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    std::string result;
    if (prepare("SELECT payload FROM events"
                "  WHERE session_id = ?1 AND run_id = ?2 AND event_type = 'run.continued'"
                "  ORDER BY id DESC LIMIT 1",
                &stmt)) {
        bind_text(stmt, 1, session_id);
        bind_text(stmt, 2, run_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = column_text(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

std::vector<StoredSession> SqliteStore::list_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, profile, model, title, updated_at, context_mode, "
                 "context_status, context_summary, context_error, source_session_id, "
                 "context_watermark, md_log_path, tags, parent_session_id, speakers, "
                 "group_rounds_override "
                 "FROM sessions ORDER BY updated_at DESC",
                 &stmt)) {
        return {};
    }
    std::vector<StoredSession> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        StoredSession session;
        session.id = column_text(stmt, 0);
        session.profile = column_text(stmt, 1);
        session.model = column_text(stmt, 2);
        session.title = column_text(stmt, 3);
        session.updated_at = column_text(stmt, 4);
        session.context_mode = column_text(stmt, 5);
        session.context_status = column_text(stmt, 6);
        session.context_summary = column_text(stmt, 7);
        session.context_error = column_text(stmt, 8);
        session.source_session_id = column_text(stmt, 9);
        session.context_watermark = sqlite3_column_int64(stmt, 10);
        session.md_log_path = column_text(stmt, 11);
        session.tags = column_text(stmt, 12);
        session.parent_session_id = column_text(stmt, 13);
        session.speakers = column_text(stmt, 14);
        session.group_rounds_override = sqlite3_column_int(stmt, 15);
        result.push_back(std::move(session));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<StoredSession> SqliteStore::latest_session_for_profile(
    const std::string& profile, const std::string& exclude_id) const {
    if (profile.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    // Worker sessions (agent.spawn children) are excluded: they are the
    // profile's side-quests, not its conversation history - including them
    // would pollute the "previous session" pointer with a worker's last
    // activity.
    if (!prepare("SELECT id, profile, model, title, updated_at, context_mode, "
                 "context_status, context_summary, context_error, source_session_id, "
                 "context_watermark FROM sessions "
                 "WHERE profile = ?1 AND id != ?2 AND parent_session_id = '' "
                 "ORDER BY updated_at DESC LIMIT 1",
                 &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, profile);
    bind_text(stmt, 2, exclude_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    StoredSession session;
    session.id = column_text(stmt, 0);
    session.profile = column_text(stmt, 1);
    session.model = column_text(stmt, 2);
    session.title = column_text(stmt, 3);
    session.updated_at = column_text(stmt, 4);
    session.context_mode = column_text(stmt, 5);
    session.context_status = column_text(stmt, 6);
    session.context_summary = column_text(stmt, 7);
    session.context_error = column_text(stmt, 8);
    session.source_session_id = column_text(stmt, 9);
    session.context_watermark = sqlite3_column_int64(stmt, 10);
    sqlite3_finalize(stmt);
    return session;
}

std::optional<StoredSession> SqliteStore::get_session(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT profile, model, title, updated_at, context_mode, context_status, "
                 "context_summary, context_error, source_session_id, context_watermark, "
                 "profile_revision, profile_snapshot, pack_snapshot_id, pack_revision, "
                 "md_log_path, tags, parent_session_id, speakers, group_rounds_override "
                 "FROM sessions WHERE id = ?1",
                 &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, id);
    StoredSession session;
    session.id = id;
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    session.profile = column_text(stmt, 0);
    session.model = column_text(stmt, 1);
    session.title = column_text(stmt, 2);
    session.updated_at = column_text(stmt, 3);
    session.context_mode = column_text(stmt, 4);
    session.context_status = column_text(stmt, 5);
    session.context_summary = column_text(stmt, 6);
    session.context_error = column_text(stmt, 7);
    session.source_session_id = column_text(stmt, 8);
    session.context_watermark = sqlite3_column_int64(stmt, 9);
    session.profile_revision = sqlite3_column_int64(stmt, 10);
    session.profile_snapshot = column_text(stmt, 11);
    session.pack_snapshot_id = sqlite3_column_int64(stmt, 12);
    session.pack_revision = sqlite3_column_int64(stmt, 13);
    session.md_log_path = column_text(stmt, 14);
    session.tags = column_text(stmt, 15);
    session.parent_session_id = column_text(stmt, 16);
    session.speakers = column_text(stmt, 17);
    session.group_rounds_override = sqlite3_column_int(stmt, 18);
    sqlite3_finalize(stmt);
    return session;
}

void SqliteStore::upsert_session(const StoredSession& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO sessions (id, profile, model, title, updated_at, "
                 "context_mode, context_status, context_summary, context_error, source_session_id, "
                 "context_watermark, profile_revision, profile_snapshot, "
                 "pack_snapshot_id, pack_revision, md_log_path, tags, parent_session_id, speakers, "
                 "group_rounds_override) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20) "
                 "ON CONFLICT(id) DO UPDATE SET profile=excluded.profile, "
                 "model=excluded.model, title=excluded.title, updated_at=excluded.updated_at, "
                 "context_mode=excluded.context_mode, context_status=excluded.context_status, "
                 "context_summary=excluded.context_summary, context_error=excluded.context_error, "
                 "source_session_id=excluded.source_session_id, "
                 "context_watermark=excluded.context_watermark, "
                 "profile_revision=excluded.profile_revision, "
                 "profile_snapshot=excluded.profile_snapshot, "
                 "pack_snapshot_id=excluded.pack_snapshot_id, "
                 "pack_revision=excluded.pack_revision, "
                 "md_log_path=excluded.md_log_path, "
                 "tags=excluded.tags, "
                 "parent_session_id=excluded.parent_session_id, "
                 "speakers=excluded.speakers, "
                 "group_rounds_override=excluded.group_rounds_override",
                 &stmt)) {
        return;
    }
    bind_text(stmt, 1, session.id);
    bind_text(stmt, 2, session.profile);
    bind_text(stmt, 3, session.model);
    bind_text(stmt, 4, session.title);
    bind_text(stmt, 5, session.updated_at);
    bind_text(stmt, 6, session.context_mode);
    bind_text(stmt, 7, session.context_status);
    bind_text(stmt, 8, session.context_summary);
    bind_text(stmt, 9, session.context_error);
    bind_text(stmt, 10, session.source_session_id);
    sqlite3_bind_int64(stmt, 11, static_cast<sqlite3_int64>(session.context_watermark));
    sqlite3_bind_int64(stmt, 12, static_cast<sqlite3_int64>(session.profile_revision));
    bind_text(stmt, 13, session.profile_snapshot);
    sqlite3_bind_int64(stmt, 14, static_cast<sqlite3_int64>(session.pack_snapshot_id));
    sqlite3_bind_int64(stmt, 15, static_cast<sqlite3_int64>(session.pack_revision));
    bind_text(stmt, 16, session.md_log_path);
    bind_text(stmt, 17, session.tags);
    bind_text(stmt, 18, session.parent_session_id);
    bind_text(stmt, 19, session.speakers);
    sqlite3_bind_int64(stmt, 20,
                       static_cast<sqlite3_int64>(session.group_rounds_override));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SqliteStore::delete_session(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    // Attachments reference messages, so they must be deleted first while the
    // subquery can still resolve the message ids.
    if (prepare("DELETE FROM attachments WHERE message_id IN "
                "(SELECT id FROM messages WHERE session_id = ?1)", &stmt)) {
        bind_text(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (prepare("DELETE FROM messages WHERE session_id = ?1", &stmt)) {
        bind_text(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (prepare("DELETE FROM events WHERE session_id = ?1", &stmt)) {
        bind_text(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (prepare("DELETE FROM sessions WHERE id = ?1", &stmt)) {
        bind_text(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

bool SqliteStore::rename_session(const std::string& id, const std::string& title) {
    if (id.empty() || title.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("UPDATE sessions SET title = ?2 WHERE id = ?1", &stmt)) {
        return false;
    }
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, title);
    sqlite3_step(stmt);
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changed > 0;
}

bool SqliteStore::set_session_tags(const std::string& id, const std::string& tags) {
    if (id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("UPDATE sessions SET tags = ?2 WHERE id = ?1", &stmt)) {
        return false;
    }
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, tags);
    sqlite3_step(stmt);
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changed > 0;
}

std::vector<StoredProfile> SqliteStore::list_profiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, name, description, identity_prompt, base_model, vision, "
                 "temperature, created_at, updated_at, revision, provider, vision_set, "
                 "prompt_pack_id, prompt_plan_json, child_profile_allowlist "
                 "FROM profiles ORDER BY created_at, id",
                 &stmt)) {
        return {};
    }
    std::vector<StoredProfile> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        StoredProfile profile;
        profile.id = column_text(stmt, 0);
        profile.name = column_text(stmt, 1);
        profile.description = column_text(stmt, 2);
        profile.identity_prompt = column_text(stmt, 3);
        profile.base_model = column_text(stmt, 4);
        profile.vision = sqlite3_column_int(stmt, 5) != 0;
        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
            profile.has_temperature = true;
            profile.temperature = sqlite3_column_double(stmt, 6);
        }
        profile.created_at = column_text(stmt, 7);
        profile.updated_at = column_text(stmt, 8);
        profile.revision = sqlite3_column_int64(stmt, 9);
        profile.provider = column_text(stmt, 10);
        profile.has_vision = sqlite3_column_int(stmt, 11) != 0;
        profile.prompt_pack_id = column_text(stmt, 12);
        profile.prompt_plan_json = column_text(stmt, 13);
        profile.child_profile_allowlist = column_text(stmt, 14);
        result.push_back(std::move(profile));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<StoredProfile> SqliteStore::get_profile(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, name, description, identity_prompt, base_model, vision, "
                 "temperature, created_at, updated_at, revision, provider, vision_set, "
                 "prompt_pack_id, prompt_plan_json, child_profile_allowlist "
                 "FROM profiles WHERE id = ?1",
                 &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, id);
    std::optional<StoredProfile> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        StoredProfile profile;
        profile.id = column_text(stmt, 0);
        profile.name = column_text(stmt, 1);
        profile.description = column_text(stmt, 2);
        profile.identity_prompt = column_text(stmt, 3);
        profile.base_model = column_text(stmt, 4);
        profile.vision = sqlite3_column_int(stmt, 5) != 0;
        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
            profile.has_temperature = true;
            profile.temperature = sqlite3_column_double(stmt, 6);
        }
        profile.created_at = column_text(stmt, 7);
        profile.updated_at = column_text(stmt, 8);
        profile.revision = sqlite3_column_int64(stmt, 9);
        profile.provider = column_text(stmt, 10);
        profile.has_vision = sqlite3_column_int(stmt, 11) != 0;
        profile.prompt_pack_id = column_text(stmt, 12);
        profile.prompt_plan_json = column_text(stmt, 13);
        profile.child_profile_allowlist = column_text(stmt, 14);
        result = std::move(profile);
    }
    sqlite3_finalize(stmt);
    return result;
}

void SqliteStore::upsert_profile(const StoredProfile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO profiles "
                 "(id, name, description, identity_prompt, base_model, vision, temperature, "
                 "created_at, updated_at, revision, provider, vision_set, "
                 "prompt_pack_id, prompt_plan_json, child_profile_allowlist) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15) "
                 "ON CONFLICT(id) DO UPDATE SET name = excluded.name, "
                 "description = excluded.description, "
                 "identity_prompt = excluded.identity_prompt, "
                 "base_model = excluded.base_model, "
                 "vision = excluded.vision, "
                 "temperature = excluded.temperature, "
                 "updated_at = excluded.updated_at, "
                 "revision = excluded.revision, "
                 "provider = excluded.provider, "
                 "vision_set = excluded.vision_set, "
                 "prompt_pack_id = excluded.prompt_pack_id, "
                 "prompt_plan_json = excluded.prompt_plan_json, "
                 "child_profile_allowlist = excluded.child_profile_allowlist", &stmt)) {
        return;
    }
    bind_text(stmt, 1, profile.id);
    bind_text(stmt, 2, profile.name);
    bind_text(stmt, 3, profile.description);
    bind_text(stmt, 4, profile.identity_prompt);
    bind_text(stmt, 5, profile.base_model);
    sqlite3_bind_int(stmt, 6, profile.vision ? 1 : 0);
    if (profile.has_temperature) {
        sqlite3_bind_double(stmt, 7, profile.temperature);
    } else {
        sqlite3_bind_null(stmt, 7);
    }
    bind_text(stmt, 8, profile.created_at);
    bind_text(stmt, 9, profile.updated_at);
    sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(profile.revision));
    bind_text(stmt, 11, profile.provider);
    sqlite3_bind_int(stmt, 12, profile.has_vision ? 1 : 0);
    bind_text(stmt, 13, profile.prompt_pack_id);
    bind_text(stmt, 14, profile.prompt_plan_json);
    bind_text(stmt, 15, profile.child_profile_allowlist);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SqliteStore::delete_profile(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (prepare("DELETE FROM profiles WHERE id = ?1", &stmt)) {
        bind_text(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

bool SqliteStore::rename_profile(const std::string& old_id,
                                 const std::string& new_id) {
    if (old_id.empty() || new_id.empty() || old_id == new_id) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    char* error = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &error) != SQLITE_OK) {
        if (error) sqlite3_free(error);
        return false;
    }
    bool ok = false;
    sqlite3_stmt* stmt = nullptr;
    if (prepare("UPDATE profiles SET id = ?2 WHERE id = ?1", &stmt)) {
        bind_text(stmt, 1, old_id);
        bind_text(stmt, 2, new_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
        sqlite3_finalize(stmt);
    }
    if (ok) {
        stmt = nullptr;
        if (prepare("UPDATE sessions SET profile = ?2 WHERE profile = ?1", &stmt)) {
            bind_text(stmt, 1, old_id);
            bind_text(stmt, 2, new_id);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
        }
    }
    if (ok) {
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &error) != SQLITE_OK) {
            if (error) sqlite3_free(error);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
        return true;
    }
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    return false;
}

// ── Prompt packs ──────────────────────────────────────────────────────────

bool SqliteStore::create_pack(const std::string& id, const std::string& name,
                              const std::string& description) {
    if (id.empty() || name.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO prompt_packs "
                 "(id, name, description, revision, validation_status, fingerprint, "
                 "created_at, updated_at) "
                 "VALUES (?1, ?2, ?3, 1, 'unchecked', '', ?4, ?4)", &stmt)) {
        return false;
    }
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, name);
    bind_text(stmt, 3, description);
    bind_text(stmt, 4, now_utc());
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<PromptPack> SqliteStore::list_packs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, name, description, revision, validation_status, fingerprint, "
                 "created_at, updated_at FROM prompt_packs ORDER BY created_at, id",
                 &stmt)) {
        return {};
    }
    std::vector<PromptPack> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PromptPack pack;
        pack.id = column_text(stmt, 0);
        pack.name = column_text(stmt, 1);
        pack.description = column_text(stmt, 2);
        pack.revision = sqlite3_column_int64(stmt, 3);
        pack.validation_status = column_text(stmt, 4);
        pack.fingerprint = column_text(stmt, 5);
        pack.created_at = column_text(stmt, 6);
        pack.updated_at = column_text(stmt, 7);
        result.push_back(std::move(pack));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<PromptPack> SqliteStore::get_pack(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, name, description, revision, validation_status, fingerprint, "
                 "created_at, updated_at FROM prompt_packs WHERE id = ?1", &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, id);
    std::optional<PromptPack> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        PromptPack pack;
        pack.id = column_text(stmt, 0);
        pack.name = column_text(stmt, 1);
        pack.description = column_text(stmt, 2);
        pack.revision = sqlite3_column_int64(stmt, 3);
        pack.validation_status = column_text(stmt, 4);
        pack.fingerprint = column_text(stmt, 5);
        pack.created_at = column_text(stmt, 6);
        pack.updated_at = column_text(stmt, 7);
        result = std::move(pack);
    }
    sqlite3_finalize(stmt);
    return result;
}

void SqliteStore::update_pack(const std::string& id, const std::string& name,
                              const std::string& description) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("UPDATE prompt_packs SET name = ?2, description = ?3, "
                 "updated_at = ?4 WHERE id = ?1", &stmt)) {
        return;
    }
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, name);
    bind_text(stmt, 3, description);
    bind_text(stmt, 4, now_utc());
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SqliteStore::bump_pack_revision(const std::string& id) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("UPDATE prompt_packs SET revision = revision + 1, "
                 "validation_status = 'unchecked', updated_at = ?2 WHERE id = ?1", &stmt)) {
        return;
    }
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, now_utc());
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SqliteStore::set_pack_validation(const std::string& id, const std::string& status,
                                      const std::string& fingerprint) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("UPDATE prompt_packs SET validation_status = ?2, fingerprint = ?3 "
                 "WHERE id = ?1", &stmt)) {
        return;
    }
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, status);
    bind_text(stmt, 3, fingerprint);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool SqliteStore::delete_pack(const std::string& id) {
    if (id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("DELETE FROM prompt_packs WHERE id = ?1", &stmt)) return false;
    bind_text(stmt, 1, id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);
    if (ok) {
        stmt = nullptr;
        if (prepare("DELETE FROM prompt_pack_files WHERE pack_id = ?1", &stmt)) {
            bind_text(stmt, 1, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    return ok;
}

void SqliteStore::upsert_pack_file(const std::string& pack_id,
                                   const std::string& logical_path,
                                   const std::string& content,
                                   const std::string& content_hash) {
    if (pack_id.empty() || logical_path.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO prompt_pack_files "
                 "(pack_id, logical_path, content, content_hash, revision, created_at, updated_at) "
                 "VALUES (?1, ?2, ?3, ?4, 1, ?5, ?5) "
                 "ON CONFLICT(pack_id, logical_path) DO UPDATE SET "
                 "content = excluded.content, "
                 "content_hash = excluded.content_hash, "
                 "revision = prompt_pack_files.revision + 1, "
                 "updated_at = excluded.updated_at", &stmt)) {
        return;
    }
    bind_text(stmt, 1, pack_id);
    bind_text(stmt, 2, logical_path);
    bind_text(stmt, 3, content);
    bind_text(stmt, 4, content_hash);
    bind_text(stmt, 5, now_utc());
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::optional<PromptPackFile> SqliteStore::get_pack_file(const std::string& pack_id,
                                                          const std::string& logical_path) const {
    if (pack_id.empty() || logical_path.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT pack_id, logical_path, content, content_hash, revision, "
                 "created_at, updated_at FROM prompt_pack_files "
                 "WHERE pack_id = ?1 AND logical_path = ?2", &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, pack_id);
    bind_text(stmt, 2, logical_path);
    std::optional<PromptPackFile> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        PromptPackFile file;
        file.pack_id = column_text(stmt, 0);
        file.logical_path = column_text(stmt, 1);
        file.content = column_text(stmt, 2);
        file.content_hash = column_text(stmt, 3);
        file.revision = sqlite3_column_int64(stmt, 4);
        file.created_at = column_text(stmt, 5);
        file.updated_at = column_text(stmt, 6);
        result = std::move(file);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<PromptPackFile> SqliteStore::list_pack_files(const std::string& pack_id) const {
    if (pack_id.empty()) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT pack_id, logical_path, content, content_hash, revision, "
                 "created_at, updated_at FROM prompt_pack_files "
                 "WHERE pack_id = ?1 ORDER BY logical_path", &stmt)) {
        return {};
    }
    bind_text(stmt, 1, pack_id);
    std::vector<PromptPackFile> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PromptPackFile file;
        file.pack_id = column_text(stmt, 0);
        file.logical_path = column_text(stmt, 1);
        file.content = column_text(stmt, 2);
        file.content_hash = column_text(stmt, 3);
        file.revision = sqlite3_column_int64(stmt, 4);
        file.created_at = column_text(stmt, 5);
        file.updated_at = column_text(stmt, 6);
        result.push_back(std::move(file));
    }
    sqlite3_finalize(stmt);
    return result;
}

long long SqliteStore::get_or_create_pack_snapshot(const std::string& pack_id,
                                                   long long revision,
                                                   const std::string& document,
                                                   const std::string& fingerprint) {
    if (pack_id.empty() || revision <= 0 || document.empty() || fingerprint.empty()) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    // Match on the document fingerprint, not just (pack_id, revision): the
    // same pack at the same revision resolves to a different document per
    // profile, and a content-blind lookup would pin a session to another
    // profile's soul.
    if (!prepare("SELECT id FROM pack_snapshots WHERE pack_id = ?1 AND revision = ?2 "
                 "AND fingerprint = ?3",
                 &stmt)) {
        return 0;
    }
    bind_text(stmt, 1, pack_id);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(revision));
    bind_text(stmt, 3, fingerprint);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const long long id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return id;
    }
    sqlite3_finalize(stmt);
    if (!prepare("INSERT INTO pack_snapshots (pack_id, revision, document, fingerprint, "
                 "created_at) VALUES (?1, ?2, ?3, ?4, ?5)", &stmt)) {
        return 0;
    }
    bind_text(stmt, 1, pack_id);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(revision));
    bind_text(stmt, 3, document);
    bind_text(stmt, 4, fingerprint);
    bind_text(stmt, 5, now_utc());
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return 0;
    }
    const long long id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);
    return id;
}

std::optional<PackSnapshot> SqliteStore::get_pack_snapshot(long long id) const {
    if (id <= 0) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, pack_id, revision, document, fingerprint, created_at "
                 "FROM pack_snapshots WHERE id = ?1", &stmt)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(id));
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    PackSnapshot snapshot;
    snapshot.id = sqlite3_column_int64(stmt, 0);
    snapshot.pack_id = column_text(stmt, 1);
    snapshot.revision = sqlite3_column_int64(stmt, 2);
    snapshot.document = column_text(stmt, 3);
    snapshot.fingerprint = column_text(stmt, 4);
    snapshot.created_at = column_text(stmt, 5);
    sqlite3_finalize(stmt);
    return snapshot;
}

std::vector<PackSnapshot> SqliteStore::list_pack_snapshots(const std::string& pack_id) const {
    if (pack_id.empty()) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, pack_id, revision, fingerprint, created_at "
                 "FROM pack_snapshots WHERE pack_id = ?1 ORDER BY revision", &stmt)) {
        return {};
    }
    bind_text(stmt, 1, pack_id);
    std::vector<PackSnapshot> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PackSnapshot snapshot;
        snapshot.id = sqlite3_column_int64(stmt, 0);
        snapshot.pack_id = column_text(stmt, 1);
        snapshot.revision = sqlite3_column_int64(stmt, 2);
        snapshot.fingerprint = column_text(stmt, 3);
        snapshot.created_at = column_text(stmt, 4);
        result.push_back(std::move(snapshot));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool SqliteStore::set_session_pack_snapshot(const std::string& session_id,
                                            long long snapshot_id,
                                            long long pack_revision) {
    // snapshot_id 0 = clear the pin (profile switch invalidates it).
    if (session_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("UPDATE sessions SET pack_snapshot_id = ?2, pack_revision = ?3 "
                 "WHERE id = ?1", &stmt)) {
        return false;
    }
    bind_text(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(snapshot_id));
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(pack_revision));
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool SqliteStore::delete_pack_file(const std::string& pack_id,
                                   const std::string& logical_path) {
    if (pack_id.empty() || logical_path.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("DELETE FROM prompt_pack_files WHERE pack_id = ?1 AND logical_path = ?2",
                 &stmt)) {
        return false;
    }
    bind_text(stmt, 1, pack_id);
    bind_text(stmt, 2, logical_path);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);
    return ok;
}

bool SqliteStore::profile_references_pack(const std::string& pack_id) const {
    if (pack_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT 1 FROM profiles WHERE prompt_pack_id = ?1 LIMIT 1", &stmt)) {
        return false;
    }
    bind_text(stmt, 1, pack_id);
    const bool referenced = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return referenced;
}

long long SqliteStore::add_status_event(const std::string& session_id,
                                        const std::string& profile,
                                        const std::string& state,
                                        const std::string& note) {
    if (session_id.empty() || state.empty()) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO status_events (session_id, profile, state, note, created_at) "
                 "VALUES (?1, ?2, ?3, ?4, ?5)", &stmt)) {
        return 0;
    }
    bind_text(stmt, 1, session_id);
    bind_text(stmt, 2, profile);
    bind_text(stmt, 3, state);
    bind_text(stmt, 4, note);
    bind_text(stmt, 5, now_utc());
    sqlite3_step(stmt);
    const long long id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);
    return id;
}

std::optional<StatusEvent> SqliteStore::latest_status_event(
    const std::string& session_id) const {
    if (session_id.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, session_id, profile, state, note, created_at FROM status_events "
                 "WHERE session_id = ?1 ORDER BY id DESC LIMIT 1", &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, session_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    StatusEvent event;
    event.id = sqlite3_column_int64(stmt, 0);
    event.session_id = column_text(stmt, 1);
    event.profile = column_text(stmt, 2);
    event.state = column_text(stmt, 3);
    event.note = column_text(stmt, 4);
    event.created_at = column_text(stmt, 5);
    sqlite3_finalize(stmt);
    return event;
}

std::optional<StatusEvent> SqliteStore::latest_status_event_for_profile(
    const std::string& profile) const {
    if (profile.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, session_id, profile, state, note, created_at FROM status_events "
                 "WHERE profile = ?1 ORDER BY id DESC LIMIT 1", &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, profile);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    StatusEvent event;
    event.id = sqlite3_column_int64(stmt, 0);
    event.session_id = column_text(stmt, 1);
    event.profile = column_text(stmt, 2);
    event.state = column_text(stmt, 3);
    event.note = column_text(stmt, 4);
    event.created_at = column_text(stmt, 5);
    sqlite3_finalize(stmt);
    return event;
}

std::vector<StatusEvent> SqliteStore::list_status_events(const std::string& session_id,
                                                          long long limit) const {
    std::vector<StatusEvent> out;
    if (session_id.empty()) return out;
    if (limit <= 0) limit = 20;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, session_id, profile, state, note, created_at FROM status_events "
                 "WHERE session_id = ?1 ORDER BY id DESC LIMIT ?2", &stmt)) {
        return out;
    }
    bind_text(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        StatusEvent event;
        event.id = sqlite3_column_int64(stmt, 0);
        event.session_id = column_text(stmt, 1);
        event.profile = column_text(stmt, 2);
        event.state = column_text(stmt, 3);
        event.note = column_text(stmt, 4);
        event.created_at = column_text(stmt, 5);
        out.push_back(std::move(event));
    }
    sqlite3_finalize(stmt);
    return out;
}

long long SqliteStore::add_source(const std::string& session_id,
                                  const std::string& run_id,
                                  const std::string& url,
                                  const std::string& title,
                                  const std::string& content_hash,
                                  long long bytes_fetched,
                                  const std::string& status,
                                  const std::string& error) {
    if (session_id.empty() || url.empty()) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    // Dedupe by (session_id, url): the same page keeps one row and one
    // [src:<id>] id for the session's whole life; a re-fetch refreshes it.
    sqlite3_stmt* find = nullptr;
    if (prepare("SELECT id FROM sources WHERE session_id = ?1 AND url = ?2 "
                "ORDER BY id DESC LIMIT 1", &find)) {
        bind_text(find, 1, session_id);
        bind_text(find, 2, url);
        if (sqlite3_step(find) == SQLITE_ROW) {
            const long long id = sqlite3_column_int64(find, 0);
            sqlite3_finalize(find);
            if (!prepare("UPDATE sources SET run_id = ?1, title = ?2, content_hash = ?3, "
                         "bytes_fetched = ?4, status = ?5, error = ?6 WHERE id = ?7",
                         &find)) {
                return 0;
            }
            bind_text(find, 1, run_id);
            bind_text(find, 2, title);
            bind_text(find, 3, content_hash);
            sqlite3_bind_int64(find, 4, bytes_fetched);
            bind_text(find, 5, status);
            bind_text(find, 6, error);
            sqlite3_bind_int64(find, 7, id);
            sqlite3_step(find);
            sqlite3_finalize(find);
            return id;
        }
        sqlite3_finalize(find);
    }
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO sources (session_id, run_id, url, title, content_hash, "
                 "bytes_fetched, status, error, created_at) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", &stmt)) {
        return 0;
    }
    bind_text(stmt, 1, session_id);
    bind_text(stmt, 2, run_id);
    bind_text(stmt, 3, url);
    bind_text(stmt, 4, title);
    bind_text(stmt, 5, content_hash);
    sqlite3_bind_int64(stmt, 6, bytes_fetched);
    bind_text(stmt, 7, status);
    bind_text(stmt, 8, error);
    bind_text(stmt, 9, now_utc());
    sqlite3_step(stmt);
    const long long id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);
    return id;
}

std::optional<Source> SqliteStore::find_source(const std::string& session_id,
                                               const std::string& url) const {
    if (session_id.empty() || url.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, session_id, run_id, url, title, content_hash, "
                 "bytes_fetched, status, error, used, created_at FROM sources "
                 "WHERE session_id = ?1 AND url = ?2 ORDER BY id DESC LIMIT 1",
                 &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, session_id);
    bind_text(stmt, 2, url);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    Source source;
    source.id = sqlite3_column_int64(stmt, 0);
    const auto text_of = [&stmt](int col) {
        const unsigned char* raw = sqlite3_column_text(stmt, col);
        return raw ? std::string(reinterpret_cast<const char*>(raw))
                   : std::string();
    };
    source.session_id = text_of(1);
    source.run_id = text_of(2);
    source.url = text_of(3);
    source.title = text_of(4);
    source.content_hash = text_of(5);
    source.bytes_fetched = sqlite3_column_int64(stmt, 6);
    source.status = text_of(7);
    source.error = text_of(8);
    source.used = sqlite3_column_int(stmt, 9) != 0;
    source.created_at = text_of(10);
    sqlite3_finalize(stmt);
    return source;
}

std::optional<Source> SqliteStore::get_source(long long id) const {
    if (id <= 0) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, session_id, run_id, url, title, content_hash, bytes_fetched, "
                 "status, error, used, created_at FROM sources WHERE id = ?1", &stmt)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    Source source;
    source.id = sqlite3_column_int64(stmt, 0);
    source.session_id = column_text(stmt, 1);
    source.run_id = column_text(stmt, 2);
    source.url = column_text(stmt, 3);
    source.title = column_text(stmt, 4);
    source.content_hash = column_text(stmt, 5);
    source.bytes_fetched = sqlite3_column_int64(stmt, 6);
    source.status = column_text(stmt, 7);
    source.error = column_text(stmt, 8);
    source.used = sqlite3_column_int(stmt, 9) != 0;
    source.created_at = column_text(stmt, 10);
    sqlite3_finalize(stmt);
    return source;
}

std::vector<Source> SqliteStore::list_sources(const std::string& session_id) const {
    std::vector<Source> out;
    if (session_id.empty()) return out;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, session_id, run_id, url, title, content_hash, bytes_fetched, "
                 "status, error, used, created_at FROM sources "
                 "WHERE session_id = ?1 ORDER BY id ASC", &stmt)) {
        return out;
    }
    bind_text(stmt, 1, session_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Source source;
        source.id = sqlite3_column_int64(stmt, 0);
        source.session_id = column_text(stmt, 1);
        source.run_id = column_text(stmt, 2);
        source.url = column_text(stmt, 3);
        source.title = column_text(stmt, 4);
        source.content_hash = column_text(stmt, 5);
        source.bytes_fetched = sqlite3_column_int64(stmt, 6);
        source.status = column_text(stmt, 7);
        source.error = column_text(stmt, 8);
        source.used = sqlite3_column_int(stmt, 9) != 0;
        source.created_at = column_text(stmt, 10);
        out.push_back(std::move(source));
    }
    sqlite3_finalize(stmt);
    return out;
}

int SqliteStore::mark_sources_used(const std::vector<long long>& ids,
                                   const std::string& session_id) {
    if (ids.empty() || session_id.empty()) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    int marked = 0;
    for (const long long id : ids) {
        sqlite3_stmt* stmt = nullptr;
        if (!prepare("UPDATE sources SET used = 1 WHERE id = ?1 AND session_id = ?2",
                     &stmt)) {
            break;
        }
        sqlite3_bind_int64(stmt, 1, id);
        bind_text(stmt, 2, session_id);
        sqlite3_step(stmt);
        marked += sqlite3_changes(db_);
        sqlite3_finalize(stmt);
    }
    return marked;
}

void SqliteStore::delete_sources_for_session(const std::string& session_id) {
    if (session_id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("DELETE FROM sources WHERE session_id = ?1", &stmt)) return;
    bind_text(stmt, 1, session_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SqliteStore::delete_sources_for_runs(
    const std::string& session_id, const std::vector<std::string>& run_ids) {
    if (session_id.empty() || run_ids.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("DELETE FROM sources WHERE session_id = ?1 AND run_id = ?2", &stmt)) return;
    bind_text(stmt, 1, session_id);
    for (const std::string& run_id : run_ids) {
        if (run_id.empty()) continue;
        bind_text(stmt, 2, run_id);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

std::vector<StoredProvider> SqliteStore::list_providers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT name, endpoint, model, api_key, params, created_at, updated_at "
                 "FROM providers ORDER BY created_at, name", &stmt)) {
        return {};
    }
    std::vector<StoredProvider> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        StoredProvider provider;
        provider.name = column_text(stmt, 0);
        provider.endpoint = column_text(stmt, 1);
        provider.model = column_text(stmt, 2);
        provider.api_key = column_text(stmt, 3);
        provider.params = column_text(stmt, 4);
        provider.created_at = column_text(stmt, 5);
        provider.updated_at = column_text(stmt, 6);
        result.push_back(std::move(provider));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<StoredProvider> SqliteStore::get_provider(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT name, endpoint, model, api_key, params, created_at, updated_at "
                 "FROM providers WHERE name = ?1", &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, name);
    std::optional<StoredProvider> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        StoredProvider provider;
        provider.name = column_text(stmt, 0);
        provider.endpoint = column_text(stmt, 1);
        provider.model = column_text(stmt, 2);
        provider.api_key = column_text(stmt, 3);
        provider.params = column_text(stmt, 4);
        provider.created_at = column_text(stmt, 5);
        provider.updated_at = column_text(stmt, 6);
        result = std::move(provider);
    }
    sqlite3_finalize(stmt);
    return result;
}

void SqliteStore::upsert_provider(const StoredProvider& provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO providers "
                 "(name, endpoint, model, api_key, params, created_at, updated_at) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
                 "ON CONFLICT(name) DO UPDATE SET endpoint = excluded.endpoint, "
                 "model = excluded.model, "
                 "api_key = excluded.api_key, "
                 "params = excluded.params, "
                 "updated_at = excluded.updated_at", &stmt)) {
        return;
    }
    bind_text(stmt, 1, provider.name);
    bind_text(stmt, 2, provider.endpoint);
    bind_text(stmt, 3, provider.model);
    bind_text(stmt, 4, provider.api_key);
    bind_text(stmt, 5, provider.params);
    bind_text(stmt, 6, provider.created_at);
    bind_text(stmt, 7, provider.updated_at);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SqliteStore::delete_provider(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (prepare("DELETE FROM providers WHERE name = ?1", &stmt)) {
        bind_text(stmt, 1, name);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::optional<std::string> SqliteStore::get_setting(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT value FROM settings WHERE key = ?1", &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, key);
    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = column_text(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return result;
}

void SqliteStore::set_setting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO settings (key, value, updated_at) VALUES (?1, ?2, ?3) "
                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value, "
                 "updated_at = excluded.updated_at",
                 &stmt)) {
        std::fprintf(stderr, "set_setting: prepare failed: %s\n", sqlite3_errmsg(db_));
        return;
    }
    bind_text(stmt, 1, key);
    bind_text(stmt, 2, value);
    bind_text(stmt, 3, now_utc());
    const int step_rc = sqlite3_step(stmt);
    if (step_rc != SQLITE_DONE) {
        std::fprintf(stderr, "set_setting: step failed (%d): %s\n",
                     step_rc, sqlite3_errmsg(db_));
    }
    sqlite3_finalize(stmt);
}

std::map<std::string, std::string> SqliteStore::all_settings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT key, value FROM settings ORDER BY key", &stmt)) {
        return {};
    }
    std::map<std::string, std::string> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result[column_text(stmt, 0)] = column_text(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return result;
}

long long SqliteStore::enqueue_memory_job(const std::string& idempotency_key,
                                          const std::string& provider,
                                          const std::string& operation,
                                          const std::string& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string now = now_utc();
    sqlite3_stmt* stmt = nullptr;
    if (prepare("INSERT OR IGNORE INTO memory_outbox "
                "(idempotency_key, provider, operation, payload, status, retry_count, "
                "next_attempt_at, last_error, provider_operation_id, created_at, updated_at) "
                "VALUES (?1, ?2, ?3, ?4, 'pending', 0, '', '', '', ?5, ?5)", &stmt)) {
        bind_text(stmt, 1, idempotency_key);
        bind_text(stmt, 2, provider);
        bind_text(stmt, 3, operation);
        bind_text(stmt, 4, payload);
        bind_text(stmt, 5, now);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    // INSERT OR IGNORE is a no-op for a duplicate key; return the row either way.
    long long id = 0;
    if (prepare("SELECT id FROM memory_outbox WHERE idempotency_key = ?1", &stmt)) {
        bind_text(stmt, 1, idempotency_key);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            id = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return id;
}

std::vector<MemoryOutboxJob> SqliteStore::due_memory_jobs(long long limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string now = now_utc();
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, idempotency_key, provider, operation, payload, status, "
                 "retry_count, next_attempt_at, last_error, provider_operation_id, "
                 "created_at, updated_at "
                 "FROM memory_outbox "
                 "WHERE status = 'pending' AND (next_attempt_at = '' OR next_attempt_at <= ?1) "
                 "ORDER BY id LIMIT ?2", &stmt)) {
        return {};
    }
    bind_text(stmt, 1, now);
    sqlite3_bind_int64(stmt, 2, limit);
    std::vector<MemoryOutboxJob> jobs;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MemoryOutboxJob job;
        job.id = sqlite3_column_int64(stmt, 0);
        job.idempotency_key = column_text(stmt, 1);
        job.provider = column_text(stmt, 2);
        job.operation = column_text(stmt, 3);
        job.payload = column_text(stmt, 4);
        job.status = column_text(stmt, 5);
        job.retry_count = static_cast<int>(sqlite3_column_int(stmt, 6));
        job.next_attempt_at = column_text(stmt, 7);
        job.last_error = column_text(stmt, 8);
        job.provider_operation_id = column_text(stmt, 9);
        job.created_at = column_text(stmt, 10);
        job.updated_at = column_text(stmt, 11);
        jobs.push_back(job);
    }
    sqlite3_finalize(stmt);
    return jobs;
}

std::optional<MemoryOutboxJob> SqliteStore::get_memory_job(long long id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, idempotency_key, provider, operation, payload, status, "
                 "retry_count, next_attempt_at, last_error, provider_operation_id, "
                 "created_at, updated_at FROM memory_outbox WHERE id = ?1", &stmt)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        MemoryOutboxJob job;
        job.id = sqlite3_column_int64(stmt, 0);
        job.idempotency_key = column_text(stmt, 1);
        job.provider = column_text(stmt, 2);
        job.operation = column_text(stmt, 3);
        job.payload = column_text(stmt, 4);
        job.status = column_text(stmt, 5);
        job.retry_count = static_cast<int>(sqlite3_column_int(stmt, 6));
        job.next_attempt_at = column_text(stmt, 7);
        job.last_error = column_text(stmt, 8);
        job.provider_operation_id = column_text(stmt, 9);
        job.created_at = column_text(stmt, 10);
        job.updated_at = column_text(stmt, 11);
        sqlite3_finalize(stmt);
        return job;
    }
    sqlite3_finalize(stmt);
    return std::nullopt;
}

void SqliteStore::mark_memory_job_sending(long long id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (prepare("UPDATE memory_outbox SET status = 'pending', updated_at = ?2 WHERE id = ?1",
                &stmt)) {
        sqlite3_bind_int64(stmt, 1, id);
        bind_text(stmt, 2, now_utc());
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void SqliteStore::mark_memory_job_completed(long long id,
                                            const std::string& provider_operation_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (prepare("UPDATE memory_outbox SET status = 'completed', "
                "provider_operation_id = ?3, updated_at = ?2 WHERE id = ?1", &stmt)) {
        sqlite3_bind_int64(stmt, 1, id);
        bind_text(stmt, 2, now_utc());
        bind_text(stmt, 3, provider_operation_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void SqliteStore::retry_memory_job(long long id, const std::string& error,
                                   const std::string& next_attempt_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (prepare("UPDATE memory_outbox SET status = 'pending', retry_count = retry_count + 1, "
                "next_attempt_at = ?3, last_error = ?4, updated_at = ?2 WHERE id = ?1", &stmt)) {
        sqlite3_bind_int64(stmt, 1, id);
        bind_text(stmt, 2, now_utc());
        bind_text(stmt, 3, next_attempt_at);
        bind_text(stmt, 4, error);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void SqliteStore::fail_memory_job(long long id, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (prepare("UPDATE memory_outbox SET status = 'failed', last_error = ?3, "
                "updated_at = ?2 WHERE id = ?1", &stmt)) {
        sqlite3_bind_int64(stmt, 1, id);
        bind_text(stmt, 2, now_utc());
        bind_text(stmt, 3, error);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::map<std::string, long long> SqliteStore::memory_job_counts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, long long> counts;
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT status, COUNT(*) FROM memory_outbox GROUP BY status", &stmt)) {
        return counts;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        counts[column_text(stmt, 0)] = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return counts;
}

long long SqliteStore::suppress_memory_key(const std::string& key,
                                           const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string now = now_utc();
    sqlite3_stmt* stmt = nullptr;
    if (prepare("INSERT OR IGNORE INTO memory_suppressions (key, reason, created_at) "
                "VALUES (?1, ?2, ?3)", &stmt)) {
        bind_text(stmt, 1, key);
        bind_text(stmt, 2, reason);
        bind_text(stmt, 3, now);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    // INSERT OR IGNORE is a no-op for a duplicate key; return the row either way.
    long long id = 0;
    if (prepare("SELECT id FROM memory_suppressions WHERE key = ?1", &stmt)) {
        bind_text(stmt, 1, key);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            id = static_cast<long long>(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return id;
}

std::vector<std::string> SqliteStore::memory_suppression_keys() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> keys;
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT key FROM memory_suppressions ORDER BY id", &stmt)) {
        return keys;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        keys.push_back(column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return keys;
}

std::map<std::string, long long> SqliteStore::memory_suppression_counts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, long long> counts;
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT reason, COUNT(*) FROM memory_suppressions GROUP BY reason",
                 &stmt)) {
        return counts;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        counts[column_text(stmt, 0)] = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return counts;
}

void SqliteStore::add_always_allowed(const std::string& tool,
                                     const std::string& pattern) {
    if (tool.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (prepare("INSERT OR IGNORE INTO always_allowed (tool, pattern, created_at) "
                "VALUES (?1, ?2, ?3)",
                &stmt)) {
        bind_text(stmt, 1, tool);
        bind_text(stmt, 2, pattern);
        bind_text(stmt, 3, now_utc());
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

SqliteStore::AlwaysAllowedRule SqliteStore::always_allowed_rule(
    const std::string& tool) const {
    AlwaysAllowedRule rule;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT pattern FROM always_allowed WHERE tool = ?1", &stmt)) {
        return rule;
    }
    bind_text(stmt, 1, tool);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        rule.present = true;
        rule.pattern = column_text(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return rule;
}

bool SqliteStore::is_always_allowed(const std::string& tool) const {
    const AlwaysAllowedRule rule = always_allowed_rule(tool);
    return rule.present && rule.pattern.empty();
}

void SqliteStore::remove_always_allowed(const std::string& tool) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (prepare("DELETE FROM always_allowed WHERE tool = ?1", &stmt)) {
        bind_text(stmt, 1, tool);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<std::pair<std::string, std::string>> SqliteStore::list_always_allowed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, std::string>> rows;
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT tool, pattern FROM always_allowed ORDER BY tool", &stmt)) {
        return rows;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.emplace_back(column_text(stmt, 0), column_text(stmt, 1));
    }
    sqlite3_finalize(stmt);
    return rows;
}

void SqliteStore::set_skill_enabled(const std::string& name, bool enabled) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (prepare("INSERT INTO skills_enabled (name, enabled, updated_at) "
                "VALUES (?1, ?2, ?3) "
                "ON CONFLICT(name) DO UPDATE SET enabled = ?2, updated_at = ?3",
                &stmt)) {
        bind_text(stmt, 1, name);
        sqlite3_bind_int(stmt, 2, enabled ? 1 : 0);
        bind_text(stmt, 3, now_utc());
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

bool SqliteStore::skill_enabled(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT enabled FROM skills_enabled WHERE name = ?1", &stmt)) {
        return true;  // no row = enabled by default
    }
    bind_text(stmt, 1, name);
    bool enabled = true;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        enabled = sqlite3_column_int(stmt, 0) != 0;
    }
    sqlite3_finalize(stmt);
    return enabled;
}

std::vector<std::pair<std::string, bool>> SqliteStore::list_skill_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, bool>> rows;
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT name, enabled FROM skills_enabled ORDER BY name", &stmt)) {
        return rows;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.emplace_back(column_text(stmt, 0), sqlite3_column_int(stmt, 1) != 0);
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<std::string> SqliteStore::run_ids_of_descendants(
    const std::string& session_id, long long ancestor_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> run_ids;
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("WITH RECURSIVE descendants(id, run_id) AS ("
                 "  SELECT id, run_id FROM messages WHERE session_id = ?1 AND parent_id = ?2"
                 "  UNION"
                 "  SELECT m.id, m.run_id FROM messages m JOIN descendants d ON m.parent_id = d.id"
                 "   WHERE m.session_id = ?1"
                 ")"
                 "SELECT DISTINCT run_id FROM descendants WHERE run_id != ''", &stmt)) {
        return run_ids;
    }
    bind_text(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(ancestor_id));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        run_ids.push_back(column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return run_ids;
}

std::vector<std::string> SqliteStore::run_ids_off_active_path(
    const std::string& session_id, long long active_leaf_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Every distinct non-empty run id in the session.
    std::set<std::string> all;
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT DISTINCT run_id FROM messages "
                 "WHERE session_id = ?1 AND run_id != ''", &stmt)) {
        return {};
    }
    bind_text(stmt, 1, session_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        all.insert(column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    // The run ids on the active path, walking the parent chain from the leaf
    // to the root (a simple, cycle-free walk - no recursive CTE needed).
    std::set<std::string> on_path;
    long long current = active_leaf_id;
    while (current > 0) {
        sqlite3_stmt* node = nullptr;
        if (!prepare("SELECT parent_id, run_id FROM messages WHERE id = ?1", &node)) {
            break;
        }
        sqlite3_bind_int64(node, 1, static_cast<sqlite3_int64>(current));
        if (sqlite3_step(node) == SQLITE_ROW) {
            current = static_cast<long long>(sqlite3_column_int64(node, 0));
            const std::string run_id = column_text(node, 1);
            if (!run_id.empty()) on_path.insert(run_id);
        } else {
            current = 0;
        }
        sqlite3_finalize(node);
    }
    std::vector<std::string> result;
    for (const std::string& run_id : all) {
        if (!on_path.count(run_id)) result.push_back(run_id);
    }
    return result;
}

long long SqliteStore::add_memory_record(const MemoryRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string now = now_utc();
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO memory_records "
                 "(status, type, text, scope, session_id, profile, run_id, "
                 " source_message_ids, provenance, contradicts_id, superseded_by_id, "
                 " created_at, updated_at, decided_at) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)",
                 &stmt)) {
        return 0;
    }
    bind_text(stmt, 1, record.status);
    bind_text(stmt, 2, record.type);
    bind_text(stmt, 3, record.text);
    bind_text(stmt, 4, record.scope);
    bind_text(stmt, 5, record.session_id);
    bind_text(stmt, 6, record.profile);
    bind_text(stmt, 7, record.run_id);
    bind_text(stmt, 8, record.source_message_ids);
    bind_text(stmt, 9, record.provenance);
    sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(record.contradicts_id));
    sqlite3_bind_int64(stmt, 11, static_cast<sqlite3_int64>(record.superseded_by_id));
    bind_text(stmt, 12, record.created_at.empty() ? now : record.created_at);
    bind_text(stmt, 13, now);
    bind_text(stmt, 14, record.decided_at);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    check_sqlite(rc, "memory record insert");
    return static_cast<long long>(sqlite3_last_insert_rowid(db_));
}

std::optional<MemoryRecord> SqliteStore::get_memory_record(long long id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, status, type, text, scope, session_id, profile, run_id, "
                 "source_message_ids, provenance, contradicts_id, superseded_by_id, "
                 "created_at, updated_at, decided_at "
                 "FROM memory_records WHERE id = ?1", &stmt)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(id));
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    MemoryRecord record;
    record.id = static_cast<long long>(sqlite3_column_int64(stmt, 0));
    record.status = column_text(stmt, 1);
    record.type = column_text(stmt, 2);
    record.text = column_text(stmt, 3);
    record.scope = column_text(stmt, 4);
    record.session_id = column_text(stmt, 5);
    record.profile = column_text(stmt, 6);
    record.run_id = column_text(stmt, 7);
    record.source_message_ids = column_text(stmt, 8);
    record.provenance = column_text(stmt, 9);
    record.contradicts_id = static_cast<long long>(sqlite3_column_int64(stmt, 10));
    record.superseded_by_id = static_cast<long long>(sqlite3_column_int64(stmt, 11));
    record.created_at = column_text(stmt, 12);
    record.updated_at = column_text(stmt, 13);
    record.decided_at = column_text(stmt, 14);
    sqlite3_finalize(stmt);
    return record;
}

std::vector<MemoryRecord> SqliteStore::list_memory_records(
    const std::string& status) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MemoryRecord> records;
    sqlite3_stmt* stmt = nullptr;
    const bool filter = !status.empty();
    const char* query =
        filter
            ? "SELECT id, status, type, text, scope, session_id, profile, run_id, "
              "source_message_ids, provenance, contradicts_id, superseded_by_id, "
              "created_at, updated_at, decided_at FROM memory_records "
              "WHERE status = ?1 ORDER BY id DESC"
            : "SELECT id, status, type, text, scope, session_id, profile, run_id, "
              "source_message_ids, provenance, contradicts_id, superseded_by_id, "
              "created_at, updated_at, decided_at FROM memory_records "
              "ORDER BY id DESC";
    if (!prepare(query, &stmt)) {
        return records;
    }
    if (filter) bind_text(stmt, 1, status);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MemoryRecord record;
        record.id = static_cast<long long>(sqlite3_column_int64(stmt, 0));
        record.status = column_text(stmt, 1);
        record.type = column_text(stmt, 2);
        record.text = column_text(stmt, 3);
        record.scope = column_text(stmt, 4);
        record.session_id = column_text(stmt, 5);
        record.profile = column_text(stmt, 6);
        record.run_id = column_text(stmt, 7);
        record.source_message_ids = column_text(stmt, 8);
        record.provenance = column_text(stmt, 9);
        record.contradicts_id = static_cast<long long>(sqlite3_column_int64(stmt, 10));
        record.superseded_by_id = static_cast<long long>(sqlite3_column_int64(stmt, 11));
        record.created_at = column_text(stmt, 12);
        record.updated_at = column_text(stmt, 13);
        record.decided_at = column_text(stmt, 14);
        records.push_back(std::move(record));
    }
    sqlite3_finalize(stmt);
    return records;
}

bool SqliteStore::set_memory_record_status(long long id, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("UPDATE memory_records SET status = ?2, updated_at = ?3, "
                 "decided_at = ?3 WHERE id = ?1", &stmt)) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(id));
    bind_text(stmt, 2, status);
    bind_text(stmt, 3, now_utc());
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    check_sqlite(rc, "memory record status update");
    return sqlite3_changes(db_) > 0;
}

std::map<std::string, long long> SqliteStore::memory_record_counts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, long long> counts;
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT status, COUNT(*) FROM memory_records GROUP BY status", &stmt)) {
        return counts;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        counts[column_text(stmt, 0)] = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return counts;
}

std::vector<StoredMessage> SqliteStore::session_messages(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, parent_id, role, content, run_id, created_at, reasoning, "
                 "is_active, speaker, client_id FROM messages "
                 "WHERE session_id = ?1 ORDER BY id ASC",
                 &stmt)) {
        return {};
    }
    bind_text(stmt, 1, session_id);
    std::vector<StoredMessage> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        StoredMessage message;
        message.id = sqlite3_column_int64(stmt, 0);
        message.parent_id = sqlite3_column_int64(stmt, 1);
        message.session_id = session_id;
        message.role = column_text(stmt, 2);
        message.content = column_text(stmt, 3);
        message.run_id = column_text(stmt, 4);
        message.created_at = column_text(stmt, 5);
        message.reasoning = column_text(stmt, 6);
        message.is_active = sqlite3_column_int(stmt, 7) != 0;
        message.speaker = column_text(stmt, 8);
        message.client_id = sqlite3_column_int64(stmt, 9);
        result.push_back(std::move(message));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<StoredMessage> SqliteStore::active_messages(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, parent_id, role, content, run_id, created_at, reasoning, "
                 "speaker, client_id FROM "
                 "messages WHERE session_id = ?1 AND is_active = 1 ORDER BY id ASC",
                 &stmt)) {
        return {};
    }
    bind_text(stmt, 1, session_id);
    std::vector<StoredMessage> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        StoredMessage message;
        message.id = sqlite3_column_int64(stmt, 0);
        message.parent_id = sqlite3_column_int64(stmt, 1);
        message.session_id = session_id;
        message.role = column_text(stmt, 2);
        message.content = column_text(stmt, 3);
        message.run_id = column_text(stmt, 4);
        message.created_at = column_text(stmt, 5);
        message.reasoning = column_text(stmt, 6);
        message.is_active = true;
        message.speaker = column_text(stmt, 7);
        message.client_id = sqlite3_column_int64(stmt, 8);
        result.push_back(std::move(message));
    }
    sqlite3_finalize(stmt);
    return result;
}

namespace {
// Monotonic 64-bit message id: (epoch_ms << 20) | per-millisecond counter.
// Every snowflake id exceeds every legacy AUTOINCREMENT rowid by orders of
// magnitude, so ORDER BY id stays chronological across the migration
// boundary, id-based watermarks stay valid, and 0 remains the "no parent"
// sentinel. 20 counter bits (1M ids/ms) leaves ~275 years of headroom.
long long next_snowflake_id() {
    static std::mutex mutex;
    static long long last_ms = 0;
    static unsigned counter = 0;
    std::lock_guard<std::mutex> lock(mutex);
    long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (now_ms <= last_ms) {
        counter = (counter + 1) & 0xFFFFF;
        if (counter == 0) now_ms = last_ms + 1;  // wrapped: advance the clock
    } else {
        counter = 0;
    }
    last_ms = now_ms;
    return (now_ms << 20) | counter;
}
}  // namespace

long long SqliteStore::append_message(const StoredMessage& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    // A-7: the primary id is a daemon-generated snowflake (monotonic in
    // time); the client's optimistic row id rides along in client_id, so
    // snapshots and SSE replay dedupe against it even when the run id
    // mapping is lost (reconnect mid-flight, provisional id race).
    const long long id = message.id != 0 ? message.id : next_snowflake_id();
    const auto insert = [&](long long client_id) -> bool {
        sqlite3_stmt* stmt = nullptr;
        if (!prepare("INSERT INTO messages (id, parent_id, session_id, role, content, "
                     "run_id, created_at, reasoning, is_active, speaker, client_id) "
                     "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)",
                     &stmt)) {
            return false;
        }
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_bind_int64(stmt, 2, message.parent_id);
        bind_text(stmt, 3, message.session_id);
        bind_text(stmt, 4, message.role);
        bind_text(stmt, 5, message.content);
        bind_text(stmt, 6, message.run_id);
        bind_text(stmt, 7, message.created_at);
        bind_text(stmt, 8, message.reasoning);
        sqlite3_bind_int(stmt, 9, message.is_active ? 1 : 0);
        bind_text(stmt, 10, message.speaker);
        sqlite3_bind_int64(stmt, 11, client_id);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    };
    bool ok = insert(message.client_id);
    if (!ok && message.client_id != 0) {
        // The client id is already anchored to another row (a retried send
        // after a reconnect): keep the row, drop the anchor.
        ok = insert(0);
    }
    return ok ? id : 0;
}

std::optional<StoredMessage> SqliteStore::get_message(long long id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, parent_id, session_id, role, content, run_id, created_at, "
                 "reasoning, is_active, speaker, client_id FROM messages WHERE id = ?1",
                 &stmt)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    StoredMessage message;
    message.id = sqlite3_column_int64(stmt, 0);
    message.parent_id = sqlite3_column_int64(stmt, 1);
    message.session_id = column_text(stmt, 2);
    message.role = column_text(stmt, 3);
    message.content = column_text(stmt, 4);
    message.run_id = column_text(stmt, 5);
    message.created_at = column_text(stmt, 6);
    message.reasoning = column_text(stmt, 7);
    message.is_active = sqlite3_column_int(stmt, 8) == 1;
    message.speaker = column_text(stmt, 9);
    message.client_id = sqlite3_column_int64(stmt, 10);
    sqlite3_finalize(stmt);
    return message;
}

std::optional<StoredMessage> SqliteStore::message_by_run(
        const std::string& run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    // Prefer the assistant row so the caller sees the turn's full shape; a
    // failed run only persists its user row (the resend case).
    if (!prepare("SELECT id, parent_id, session_id, role, content, run_id, "
                 "created_at, reasoning, is_active, speaker, client_id FROM messages "
                 "WHERE run_id = ?1 "
                 "ORDER BY CASE WHEN role = 'assistant' THEN 0 ELSE 1 END, id DESC "
                 "LIMIT 1",
                 &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, run_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    StoredMessage message;
    message.id = sqlite3_column_int64(stmt, 0);
    message.parent_id = sqlite3_column_int64(stmt, 1);
    message.session_id = column_text(stmt, 2);
    message.role = column_text(stmt, 3);
    message.content = column_text(stmt, 4);
    message.run_id = column_text(stmt, 5);
    message.created_at = column_text(stmt, 6);
    message.reasoning = column_text(stmt, 7);
    message.is_active = sqlite3_column_int(stmt, 8) == 1;
    message.speaker = column_text(stmt, 9);
    message.client_id = sqlite3_column_int64(stmt, 10);
    sqlite3_finalize(stmt);
    return message;
}

std::optional<StoredMessage> SqliteStore::user_message_by_run(
        const std::string& run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, parent_id, session_id, role, content, run_id, "
                 "created_at, reasoning, is_active, speaker, client_id FROM messages "
                 "WHERE run_id = ?1 AND role = 'user' "
                 "ORDER BY id DESC LIMIT 1",
                 &stmt)) {
        return std::nullopt;
    }
    bind_text(stmt, 1, run_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    StoredMessage message;
    message.id = sqlite3_column_int64(stmt, 0);
    message.parent_id = sqlite3_column_int64(stmt, 1);
    message.session_id = column_text(stmt, 2);
    message.role = column_text(stmt, 3);
    message.content = column_text(stmt, 4);
    message.run_id = column_text(stmt, 5);
    message.created_at = column_text(stmt, 6);
    message.reasoning = column_text(stmt, 7);
    message.is_active = sqlite3_column_int(stmt, 8) == 1;
    message.speaker = column_text(stmt, 9);
    message.client_id = sqlite3_column_int64(stmt, 10);
    sqlite3_finalize(stmt);
    return message;
}

void SqliteStore::update_message_content(long long message_id, const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("UPDATE messages SET content = ?2 WHERE id = ?1", &stmt)) {
        return;
    }
    sqlite3_bind_int64(stmt, 1, message_id);
    bind_text(stmt, 2, content);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SqliteStore::set_message_parent(long long message_id, long long parent_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("UPDATE messages SET parent_id = ?2 WHERE id = ?1", &stmt)) {
        return;
    }
    sqlite3_bind_int64(stmt, 1, message_id);
    sqlite3_bind_int64(stmt, 2, parent_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool SqliteStore::is_descendant(long long start, long long ancestor) const {
    std::lock_guard<std::mutex> lock(mutex_);
    long long current = start;
    for (int guard = 0; guard < 10000 && current > 0; ++guard) {
        if (current == ancestor) return true;
        sqlite3_stmt* stmt = nullptr;
        if (!prepare("SELECT parent_id FROM messages WHERE id = ?1", &stmt)) break;
        sqlite3_bind_int64(stmt, 1, current);
        long long parent = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) parent = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        current = parent;
    }
    return false;
}

void SqliteStore::delete_message(long long id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    // A queued turn's message may already be the parent of a later queued
    // turn's message. Re-parent the children onto this message's parent so
    // the branch stays connected instead of dangling.
    if (prepare("UPDATE messages SET parent_id = "
                "(SELECT parent_id FROM messages WHERE id = ?1) "
                "WHERE parent_id = ?1",
                &stmt)) {
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (prepare("DELETE FROM attachments WHERE message_id = ?1", &stmt)) {
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (prepare("DELETE FROM messages WHERE id = ?1", &stmt)) {
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

bool SqliteStore::trim_descendants(const std::string& session_id,
                                    long long message_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_id.empty() || message_id < 0) return false;

    char* error = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &error) != SQLITE_OK) {
        if (error) sqlite3_free(error);
        return false;
    }

    const auto execute = [&](const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (!prepare(sql, &stmt)) return false;
        bind_text(stmt, 1, session_id);
        if (message_id > 0) sqlite3_bind_int64(stmt, 2, message_id);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    };

    bool ok = false;
    if (message_id == 0) {
        // A root rewind clears the entire tree, including all inactive
        // branches, while leaving the session and its event audit intact.
        ok = execute("DELETE FROM attachments WHERE message_id IN ("
                     "SELECT id FROM messages WHERE session_id = ?1)") &&
             execute("DELETE FROM messages WHERE session_id = ?1");
    } else {
        // UNION, rather than UNION ALL, makes this safe if a damaged database
        // contains a parent cycle. Only descendants are removed, so sibling
        // alternatives at the checkpoint remain available.
        constexpr const char* descendants =
            "WITH RECURSIVE descendants(id) AS ("
            "  SELECT id FROM messages WHERE id = ?2 AND session_id = ?1"
            "  UNION"
            "  SELECT child.id FROM messages child "
            "  JOIN descendants parent ON child.parent_id = parent.id "
            "  WHERE child.session_id = ?1"
            ") ";
        const std::string delete_attachments =
            std::string(descendants) +
            "DELETE FROM attachments WHERE message_id IN ("
            "SELECT id FROM descendants WHERE id != ?2)";
        const std::string delete_messages =
            std::string(descendants) +
            "DELETE FROM messages WHERE id IN ("
            "SELECT id FROM descendants WHERE id != ?2)";
        ok = execute(delete_attachments.c_str()) && execute(delete_messages.c_str());
    }

    if (ok) {
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &error) != SQLITE_OK) {
            if (error) sqlite3_free(error);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
        return true;
    }
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    return false;
}

bool SqliteStore::delete_message_tail(const std::string& session_id,
                                      long long message_id) {
    if (session_id.empty() || message_id <= 0) return false;

    long long parent_id = 0;
    bool was_active = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sqlite3_stmt* stmt = nullptr;
        if (!prepare("SELECT parent_id, is_active FROM messages "
                     "WHERE id = ?1 AND session_id = ?2",
                     &stmt)) {
            return false;
        }
        sqlite3_bind_int64(stmt, 1, message_id);
        bind_text(stmt, 2, session_id);
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return false;
        }
        parent_id = sqlite3_column_int64(stmt, 0);
        was_active = sqlite3_column_int(stmt, 1) == 1;
        sqlite3_finalize(stmt);
    }

    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        char* error = nullptr;
        if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &error) != SQLITE_OK) {
            if (error) sqlite3_free(error);
            return false;
        }

        const auto execute = [&](const char* sql) {
            sqlite3_stmt* stmt = nullptr;
            if (!prepare(sql, &stmt)) return false;
            bind_text(stmt, 1, session_id);
            sqlite3_bind_int64(stmt, 2, message_id);
            const int rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            return rc == SQLITE_DONE;
        };

        // UNION, rather than UNION ALL, makes this safe if a damaged database
        // contains a parent cycle. Unlike trim_descendants the target itself
        // is removed, so siblings under its parent are the only survivors.
        constexpr const char* subtree =
            "WITH RECURSIVE subtree(id) AS ("
            "  SELECT id FROM messages WHERE id = ?2 AND session_id = ?1"
            "  UNION"
            "  SELECT child.id FROM messages child "
            "  JOIN subtree parent ON child.parent_id = parent.id "
            "  WHERE child.session_id = ?1"
            ") ";
        const std::string delete_attachments =
            std::string(subtree) +
            "DELETE FROM attachments WHERE message_id IN (SELECT id FROM subtree)";
        const std::string delete_messages =
            std::string(subtree) +
            "DELETE FROM messages WHERE id IN (SELECT id FROM subtree)";

        ok = execute(delete_attachments.c_str()) && execute(delete_messages.c_str());

        if (ok) {
            if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &error) != SQLITE_OK) {
                if (error) sqlite3_free(error);
                sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                return false;
            }
        } else {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
    }

    // Re-anchor the active path. If the deleted message was active, prefer
    // the newest surviving sibling under its parent - the chat stays on a
    // branch instead of collapsing to the parent, and the next message
    // chains onto the promoted branch rather than forking off the parent.
    // The promoted leaf is the deepest node of that sibling's subtree (the
    // newest leaf, so a sub-branch's latest iteration wins). Only re-anchor
    // at the parent (or clear, for a root) when no sibling remains. An
    // inactive branch never touches the active path, so deleting one leaves
    // the projection alone. mutex_ is non-recursive, so this runs after the
    // transaction's lock is released.
    if (was_active) {
        long long sibling = 0;
        long long promoted = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sqlite3_stmt* stmt = nullptr;
            if (prepare("SELECT MAX(id) FROM messages "
                        "WHERE session_id = ?1 AND parent_id = ?2",
                        &stmt)) {
                bind_text(stmt, 1, session_id);
                sqlite3_bind_int64(stmt, 2, parent_id);
                if (sqlite3_step(stmt) == SQLITE_ROW)
                    sibling = sqlite3_column_int64(stmt, 0);
                sqlite3_finalize(stmt);
            }
            // Deepest node (no children) of the sibling's subtree, so the
            // promoted path runs to the branch's end, not just its fork.
            if (sibling > 0 &&
                prepare("WITH RECURSIVE sub(id) AS ("
                        "  SELECT id FROM messages WHERE id = ?2 AND session_id = ?1"
                        "  UNION"
                        "  SELECT child.id FROM messages child "
                        "  JOIN sub parent ON child.parent_id = parent.id "
                        "  WHERE child.session_id = ?1"
                        ") "
                        "SELECT MAX(m.id) FROM messages m "
                        "WHERE m.session_id = ?1 AND m.id IN (SELECT id FROM sub) "
                        "  AND NOT EXISTS (SELECT 1 FROM messages c "
                        "                  WHERE c.session_id = ?1 AND c.parent_id = m.id)",
                        &stmt)) {
                bind_text(stmt, 1, session_id);
                sqlite3_bind_int64(stmt, 2, sibling);
                if (sqlite3_step(stmt) == SQLITE_ROW)
                    promoted = sqlite3_column_int64(stmt, 0);
                sqlite3_finalize(stmt);
            }
        }
        set_active_leaf(session_id, promoted > 0 ? promoted : parent_id);
    }
    return true;
}

void SqliteStore::insert_attachment(const StoredAttachment& attachment) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Content-addressed dedup: when this session already stores the same
    // content, keep a reference row (empty content) instead of duplicating
    // the bytes. Counting only rows that carry content keeps this
    // self-healing if the canonical row was ever trimmed.
    std::string hash = attachment.content_hash;
    if (hash.empty() && !attachment.content.empty()) {
        hash = sha256_hex(attachment.content);
    }
    std::string content = attachment.content;
    if (!hash.empty()) {
        sqlite3_stmt* probe = nullptr;
        if (prepare("SELECT COUNT(*) FROM attachments a "
                    "JOIN messages m ON m.id = a.message_id "
                    "WHERE a.content_hash = ?1 AND a.content != '' AND "
                    "m.session_id = (SELECT session_id FROM messages WHERE id = ?2)",
                    &probe)) {
            bind_text(probe, 1, hash);
            sqlite3_bind_int64(probe, 2, attachment.message_id);
            if (sqlite3_step(probe) == SQLITE_ROW && sqlite3_column_int64(probe, 0) > 0) {
                content.clear();
            }
            sqlite3_finalize(probe);
        }
    }
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO attachments "
                 "(message_id, name, kind, mime_type, size, width, height, content, content_hash) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
                 &stmt)) {
        return;
    }
    sqlite3_bind_int64(stmt, 1, attachment.message_id);
    bind_text(stmt, 2, attachment.name);
    bind_text(stmt, 3, attachment.kind);
    bind_text(stmt, 4, attachment.mime_type);
    sqlite3_bind_int64(stmt, 5, attachment.size);
    if (attachment.width.has_value()) {
        sqlite3_bind_int64(stmt, 6, *attachment.width);
    } else {
        sqlite3_bind_null(stmt, 6);
    }
    if (attachment.height.has_value()) {
        sqlite3_bind_int64(stmt, 7, *attachment.height);
    } else {
        sqlite3_bind_null(stmt, 7);
    }
    bind_text(stmt, 8, content);
    bind_text(stmt, 9, hash);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

long long SqliteStore::last_attachment_id() {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT last_insert_rowid()", &stmt)) return 0;
    long long id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}

std::vector<StoredAttachment> SqliteStore::attachments_for_message(
    long long message_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id, message_id, name, kind, mime_type, size, width, height, content, content_hash "
                 "FROM attachments WHERE message_id = ?1 ORDER BY id ASC",
                 &stmt)) {
        return {};
    }
    sqlite3_bind_int64(stmt, 1, message_id);
    std::vector<StoredAttachment> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        StoredAttachment attachment;
        attachment.id = sqlite3_column_int64(stmt, 0);
        attachment.message_id = sqlite3_column_int64(stmt, 1);
        attachment.name = column_text(stmt, 2);
        attachment.kind = column_text(stmt, 3);
        attachment.mime_type = column_text(stmt, 4);
        attachment.size = sqlite3_column_int64(stmt, 5);
        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
            attachment.width = sqlite3_column_int(stmt, 6);
        }
        if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
            attachment.height = sqlite3_column_int(stmt, 7);
        }
        attachment.content = column_text(stmt, 8);
        attachment.content_hash = column_text(stmt, 9);
        result.push_back(std::move(attachment));
    }
    sqlite3_finalize(stmt);
    // Reference rows borrow the session's canonical content.
    std::string session_id;
    {
        sqlite3_stmt* message = nullptr;
        if (prepare("SELECT session_id FROM messages WHERE id = ?1", &message)) {
            sqlite3_bind_int64(message, 1, message_id);
            if (sqlite3_step(message) == SQLITE_ROW) {
                session_id = column_text(message, 0);
            }
            sqlite3_finalize(message);
        }
    }
    if (!session_id.empty()) {
        fill_deduped_contents(session_id, result);
    }
    return result;
}

std::optional<StoredAttachment> SqliteStore::get_attachment(
    long long attachment_id, const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT a.id, a.message_id, a.name, a.kind, a.mime_type, a.size, "
                 "a.width, a.height, a.content, a.content_hash FROM attachments a "
                 "JOIN messages m ON m.id = a.message_id "
                 "WHERE a.id = ?1 AND m.session_id = ?2", &stmt)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, attachment_id);
    bind_text(stmt, 2, session_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    StoredAttachment attachment;
    attachment.id = sqlite3_column_int64(stmt, 0);
    attachment.message_id = sqlite3_column_int64(stmt, 1);
    attachment.name = column_text(stmt, 2);
    attachment.kind = column_text(stmt, 3);
    attachment.mime_type = column_text(stmt, 4);
    attachment.size = sqlite3_column_int64(stmt, 5);
    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
        attachment.width = sqlite3_column_int(stmt, 6);
    }
    if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
        attachment.height = sqlite3_column_int(stmt, 7);
    }
    attachment.content = column_text(stmt, 8);
    attachment.content_hash = column_text(stmt, 9);
    sqlite3_finalize(stmt);
    if (attachment.content.empty() && !attachment.content_hash.empty()) {
        std::vector<StoredAttachment> single = {attachment};
        fill_deduped_contents(session_id, single);
        attachment = std::move(single.front());
    }
    return attachment;
}

void SqliteStore::fill_deduped_contents(const std::string& session_id,
                                        std::vector<StoredAttachment>& rows) const {
    bool needs_content = false;
    for (const StoredAttachment& row : rows) {
        if (row.content.empty() && !row.content_hash.empty()) {
            needs_content = true;
            break;
        }
    }
    if (!needs_content) return;
    // One canonical row per (session, hash) by the insert invariant.
    std::map<std::string, std::string> canonical;
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT a.content_hash, a.content FROM attachments a "
                 "JOIN messages m ON m.id = a.message_id "
                 "WHERE m.session_id = ?1 AND a.content != '' AND a.content_hash != ''",
                 &stmt)) {
        return;
    }
    bind_text(stmt, 1, session_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        canonical[column_text(stmt, 0)] = column_text(stmt, 1);
    }
    sqlite3_finalize(stmt);
    for (StoredAttachment& row : rows) {
        if (!row.content.empty() || row.content_hash.empty()) continue;
        const auto found = canonical.find(row.content_hash);
        if (found != canonical.end()) {
            row.content = found->second;
        }
    }
}

long long SqliteStore::active_leaf_id(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT COALESCE(MAX(id), 0) FROM messages WHERE session_id = ?1 AND "
                 "is_active = 1",
                 &stmt)) {
        return 0;
    }
    bind_text(stmt, 1, session_id);
    long long leaf = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) leaf = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return leaf;
}

void SqliteStore::set_active_leaf(const std::string& session_id, long long leaf_id,
                                  bool keep_leaf_subtree) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Walk the parent chain from the leaf to the root, collecting the active
    // path. Bounded: a parent cycle (e.g. a corrupted tree) must not hang the
    // run thread that owns the store's lock.
    std::vector<long long> path;
    long long current = leaf_id;
    for (int guard = 0; guard < 10000 && current > 0; ++guard) {
        if (std::find(path.begin(), path.end(), current) != path.end()) break;
        path.push_back(current);
        sqlite3_stmt* stmt = nullptr;
        if (!prepare("SELECT parent_id FROM messages WHERE id = ?1", &stmt)) break;
        sqlite3_bind_int64(stmt, 1, current);
        long long parent = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) parent = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        current = parent;
    }
    sqlite3_stmt* clear = nullptr;
    if (keep_leaf_subtree) {
        // Deactivate only messages outside the new active path AND outside the
        // subtree below the leaf (see the header comment for why the subtree
        // must survive).
        if (prepare("WITH RECURSIVE kept(id) AS ("
                    "  SELECT ?2 UNION ALL"
                    "  SELECT m.id FROM messages m JOIN kept k ON m.parent_id = k.id"
                    "  WHERE m.session_id = ?1"
                    ") UPDATE messages SET is_active = 0"
                    " WHERE session_id = ?1 AND is_active = 1"
                    "   AND id NOT IN (SELECT id FROM kept)",
                    &clear)) {
            bind_text(clear, 1, session_id);
            sqlite3_bind_int64(clear, 2, leaf_id);
            sqlite3_step(clear);
            sqlite3_finalize(clear);
        }
    } else if (prepare("UPDATE messages SET is_active = 0 WHERE session_id = ?1", &clear)) {
        bind_text(clear, 1, session_id);
        sqlite3_step(clear);
        sqlite3_finalize(clear);
    }
    sqlite3_stmt* mark = nullptr;
    if (prepare("UPDATE messages SET is_active = 1 WHERE id = ?1", &mark)) {
        for (const long long id : path) {
            sqlite3_bind_int64(mark, 1, id);
            sqlite3_step(mark);
            sqlite3_reset(mark);
        }
        sqlite3_finalize(mark);
    }
}

void SqliteStore::select_message(const std::string& session_id, long long message_id) {
    long long leaf = message_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sqlite3_stmt* stmt = nullptr;
        if (prepare(
                "WITH RECURSIVE descendants(id) AS ("
                "  SELECT ?1"
                "  UNION ALL"
                "  SELECT m.id FROM messages m JOIN descendants d ON m.parent_id = d.id "
                "  WHERE m.session_id = ?2"
                ") SELECT id FROM descendants ORDER BY id DESC LIMIT 1",
                &stmt)) {
            sqlite3_bind_int64(stmt, 1, leaf);
            bind_text(stmt, 2, session_id);
            if (sqlite3_step(stmt) == SQLITE_ROW) leaf = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }
    if (leaf > 0) set_active_leaf(session_id, leaf);
}

void SqliteStore::select_iteration(const std::string& session_id,
                                   const std::string& run_id) {
    long long leaf = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sqlite3_stmt* stmt = nullptr;
        if (prepare("SELECT id FROM messages WHERE session_id = ?1 AND run_id = ?2 AND "
                    "role = 'assistant' ORDER BY id DESC LIMIT 1",
                    &stmt)) {
            bind_text(stmt, 1, session_id);
            bind_text(stmt, 2, run_id);
            if (sqlite3_step(stmt) == SQLITE_ROW) leaf = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }
    if (leaf > 0) select_message(session_id, leaf);
}

}  // namespace nyaxus
