#include "nyaxus/memory_custom.hpp"
#include "nyaxus/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace nyaxus {
namespace memory {
namespace custom {
namespace {

// ── Small helpers ────────────────────────────────────────────────────────

std::string strip_code_fences(const std::string& raw) {
    auto ltrim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        return s;
    };
    auto rtrim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    };
    std::string text = rtrim(ltrim(raw));
    if (text.size() >= 4 && text.compare(0, 4, "```") == 0) {
        const std::size_t first_newline = text.find('\n');
        if (first_newline != std::string::npos) text = text.substr(first_newline + 1);
        text = rtrim(ltrim(text));
        if (text.size() >= 3 && text.compare(text.size() - 3, 3, "```") == 0) {
            text.erase(text.size() - 3);
            text = rtrim(text);
        }
    }
    return text;
}

std::string lowercase(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Distinct lowercase alphanumeric tokens of at least 4 characters.
std::set<std::string> content_tokens(const std::string& text) {
    std::set<std::string> tokens;
    std::string token;
    auto flush = [&]() {
        if (token.size() >= 4) tokens.insert(token);
        token.clear();
    };
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (std::isalnum(c)) {
            token.push_back(static_cast<char>(std::tolower(c)));
        } else {
            flush();
        }
    }
    flush();
    return tokens;
}

// Value tokens: the load-bearing specifics of a memory - numbers (ports,
// versions, quantities), ISO dates, and alphanumeric compounds that carry a
// digit (seagull-42, v12, Q4), plus quoted strings. Two same-topic memories
// with disjoint value sets assert different facts (the conflict detector).
std::vector<std::string> value_tokens(const std::string& text) {
    std::vector<std::string> values;
    std::string lower;
    lower.reserve(text.size());
    for (char raw : text) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw))));
    // Quoted strings are values verbatim (a name, a phrase, an error).
    for (std::size_t i = 0; i < lower.size(); ++i) {
        const char quote = lower[i];
        if (quote != '"' && quote != '\'') continue;
        const std::size_t close = lower.find(quote, i + 1);
        if (close != std::string::npos && close > i + 1) {
            values.push_back(lower.substr(i + 1, close - i - 1));
        }
    }
    // Tokens containing a digit (numbers, dates, versions, compounds).
    std::string token;
    auto flush = [&]() {
        if (!token.empty()) {
            bool has_digit = false;
            for (char c : token) {
                if (std::isdigit(static_cast<unsigned char>(c))) { has_digit = true; break; }
            }
            if (has_digit && std::find(values.begin(), values.end(), token) == values.end()) {
                values.push_back(token);
            }
            token.clear();
        }
    };
    for (char c : lower) {
        const bool keep = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.';
        if (keep) {
            token.push_back(c);
        } else {
            flush();
        }
    }
    flush();
    return values;
}

// Deterministic conflict test (high precision by design, like the anchor
// stage): two same-topic texts conflict when they share strong context (a
// shared exact anchor, or >= 2 shared content tokens) AND carry disjoint
// value sets. Overlapping values mean the same fact, not a contradiction.
// False negatives are acceptable (the pair simply stays unflagged); false
// positives cost one entry in the approval queue, never a data change.
bool detect_conflict(const std::string& a, const std::string& b, std::string& why) {
    const std::set<std::string> ta = content_tokens(a);
    const std::set<std::string> tb = content_tokens(b);
    std::set<std::string> shared;
    std::set_intersection(ta.begin(), ta.end(), tb.begin(), tb.end(),
                          std::inserter(shared, shared.begin()));
    const std::vector<std::string> va = value_tokens(a);
    const std::vector<std::string> vb = value_tokens(b);
    if (va.empty() || vb.empty()) return false;
    for (const std::string& v : va) {
        if (std::find(vb.begin(), vb.end(), v) != vb.end()) return false;  // same value
    }
    const std::vector<Anchor> anchors_a = extract_anchors(a);
    const std::vector<Anchor> anchors_b = extract_anchors(b);
    bool shared_anchor = false;
    for (const Anchor& x : anchors_a) {
        for (const Anchor& y : anchors_b) {
            if (x.kind == y.kind && x.value == y.value) shared_anchor = true;
        }
    }
    if (!shared_anchor && shared.size() < 2) return false;  // not the same topic
    why = "disjoint values (" + std::to_string(va.size()) + " vs " +
          std::to_string(vb.size()) + " tokens) under shared context" +
          (shared_anchor ? " (shared anchor)"
                         : " (" + std::to_string(shared.size()) + " shared tokens)");
    return true;
}

// Grounding check: a candidate is grounded in the submitted source when a
// large majority of its distinct content tokens appear in the source.
// Paraphrase-tolerant (no exact substring requirement), hallucination-hostile
// (invented proper nouns fail).
bool grounded(const std::string& candidate, const std::string& source) {
    const std::set<std::string> candidate_tokens = content_tokens(candidate);
    if (candidate_tokens.empty()) return true;  // nothing to check
    const std::string lowered_source = lowercase(source);
    std::size_t present = 0;
    for (const std::string& token : candidate_tokens) {
        if (lowered_source.find(token) != std::string::npos) ++present;
    }
    return static_cast<double>(present) >= 0.7 * static_cast<double>(candidate_tokens.size());
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        norm_a += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        norm_b += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }
    if (norm_a <= 0.0 || norm_b <= 0.0) return 0.0;
    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

// Embedding input cap (~1000 tokens, the deployment n_ctx cap; the model
// itself supports 32K). Full text stays in SQLite.
constexpr int kEmbedMaxTokens = 1000;

// Conflict-detection band floor: pairs with cosine >= this are "same topic"
// enough to assert a value; pairs below it are merely "same area". The top
// of the band is the near-duplicate threshold (the operator's sweep
// setting), so the band is [kConflictBandLower, threshold).
constexpr double kConflictBandLower = 0.55;

std::vector<float> embed_truncated(const qwen3::Model& model, const std::string& text) {
    std::vector<int> ids = qwen3::encode(model, text);
    if (static_cast<int>(ids.size()) > kEmbedMaxTokens) {
        ids.resize(kEmbedMaxTokens);
    }
    return qwen3::embed_ids(model, ids);
}

std::string iso_utc(std::chrono::system_clock::time_point when) {
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()) % 1000;
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(when);
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

// Optional per-stage timing: set NYAXUS_MEM_TIMING=1 to log stage durations
// to stderr. Off by default; the recall path is latency-sensitive.
struct StageTimer {
    const char* stage;
    std::chrono::steady_clock::time_point start;
    bool enabled;
    explicit StageTimer(const char* stage_name)
        : stage(stage_name),
          start(std::chrono::steady_clock::now()),
          enabled(std::getenv("NYAXUS_MEM_TIMING") != nullptr) {}
    ~StageTimer() {
        if (!enabled) return;
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        fprintf(stderr, "[mem-timing] %s: %lldms\n", stage, ms);
    }
};

// FTS5 match expression: quoted tokens joined with OR (unicode61 has no
// stemming, so AND over all query tokens is too strict). BM25 still ranks
// documents matching more tokens higher, and the candidate pool is only a
// generator - RRF + rerank decide the final order.
std::string fts_match(const std::string& query) {
    std::string expression;
    std::string token;
    auto flush = [&]() {
        if (token.size() >= 3) {
            if (!expression.empty()) expression += " OR ";
            expression += "\"" + token + "\"";
        }
        token.clear();
    };
    for (const char raw : query) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (std::isalnum(c)) {
            token.push_back(static_cast<char>(std::tolower(c)));
        } else {
            flush();
        }
    }
    flush();
    return expression;
}

// ISO dates (YYYY-MM-DD) present in the text, in order of appearance.
std::vector<std::string> dates_in(const std::string& text) {
    std::vector<std::string> dates;
    auto is_d = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    for (std::size_t i = 0; i + 9 < text.size(); ++i) {
        if (is_d(text[i]) && is_d(text[i + 1]) && is_d(text[i + 2]) &&
            is_d(text[i + 3]) && text[i + 4] == '-' && is_d(text[i + 5]) &&
            is_d(text[i + 6]) && text[i + 7] == '-' && is_d(text[i + 8]) &&
            is_d(text[i + 9])) {
            const std::string date = text.substr(i, 10);
            if (date == "0000-00-00") continue;
            if (dates.empty() || dates.back() != date) dates.push_back(date);
            i += 9;
        }
    }
    return dates;
}

}  // namespace

bool valid_type(const std::string& type) {
    return type == "world" || type == "experience" || type == "observation" ||
           type == "mental_model" || type == "procedure" || type == "lesson";
}

// ── Anchor extraction ────────────────────────────────────────────────────

std::vector<Anchor> extract_anchors(const std::string& text) {
    std::vector<Anchor> out;
    std::set<std::string> seen;
    std::vector<std::pair<std::size_t, std::size_t>> used_ranges;
    auto add = [&](const std::string& kind, const std::string& value) {
        if (value.size() < 4) return;
        const std::string key = kind + "\x00" + value;
        if (!seen.insert(key).second) return;
        out.push_back({kind, value});
    };
    auto in_range = [&](std::size_t i, std::size_t j) {
        for (const auto& [b, e] : used_ranges) {
            if (i >= b && j <= e) return true;
        }
        return false;
    };

    // URLs.
    for (std::size_t i = 0; i + 7 <= text.size(); ++i) {
        if (text.compare(i, 7, "http://") != 0 && text.compare(i, 8, "https://") != 0) continue;
        std::size_t j = i;
        while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j])) &&
               text[j] != '"' && text[j] != '\'' && text[j] != '<' && text[j] != '>' &&
               text[j] != ')' && text[j] != '(') {
            ++j;
        }
        if (j - i >= 10) {
            add("url", text.substr(i, j - i));
            used_ranges.push_back({i, j});
        }
        i = j;
    }

    // UUIDs (8-4-4-4-12 hex).
    for (std::size_t i = 0; i + 35 < text.size(); ++i) {
        auto hex = [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        };
        auto seg = [&](std::size_t at, std::size_t len) {
            for (std::size_t k = 0; k < len; ++k) {
                if (!hex(text[at + k])) return false;
            }
            return true;
        };
        if (seg(i, 8) && text[i + 8] == '-' && seg(i + 9, 4) && text[i + 13] == '-' &&
            seg(i + 14, 4) && text[i + 18] == '-' && seg(i + 19, 4) && text[i + 23] == '-' &&
            seg(i + 24, 12)) {
            add("uuid", text.substr(i, 36));
            used_ranges.push_back({i, i + 36});
            i += 35;
        }
    }

    // Hashes: maximal hex runs of 8+ chars, not inside a UUID/URL.
    for (std::size_t i = 0; i < text.size();) {
        auto hex = [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        };
        if (!hex(text[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && hex(text[j])) ++j;
        if (j - i >= 8 && j - i <= 64 && !in_range(i, j)) {
            add("hash", text.substr(i, j - i));
            used_ranges.push_back({i, j});
        }
        i = j;
    }

    // File paths: /a/b (two or more segments) or C:\... drive paths.
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] != '/') {
            std::size_t j = i;
            int slashes = 1;
            while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j])) &&
                   text[j] != '"' && text[j] != '\'' && text[j] != '<' && text[j] != '>' &&
                   text[j] != ')' && text[j] != '(') {
                if (text[j] == '/') ++slashes;
                ++j;
            }
            if (slashes >= 2 && j - i >= 4 && !in_range(i, j)) {
                add("path", text.substr(i, j - i));
                used_ranges.push_back({i, j});
            }
            i = j;
            continue;
        }
        if ((text[i] >= 'A' && text[i] <= 'Z') && i + 2 < text.size() && text[i + 1] == ':' &&
            (text[i + 2] == '\\' || text[i + 2] == '/')) {
            std::size_t j = i;
            while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j])) &&
                   text[j] != '"' && text[j] != '\'' && text[j] != '<' && text[j] != '>') {
                ++j;
            }
            if (j - i >= 4 && !in_range(i, j)) {
                add("path", text.substr(i, j - i));
                used_ranges.push_back({i, j});
            }
            i = j;
        }
    }

    // Model / artifact names: identifier runs (letters, digits, . _ -) of
    // 6+ chars containing at least one letter and one digit.
    for (std::size_t i = 0; i < text.size();) {
        auto ident = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-';
        };
        if (!ident(text[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && ident(text[j])) ++j;
        if (j - i >= 6 && !in_range(i, j)) {
            const std::string value = text.substr(i, j - i);
            bool has_letter = false, has_digit = false;
            for (const char c : value) {
                if (std::isalpha(static_cast<unsigned char>(c))) has_letter = true;
                if (std::isdigit(static_cast<unsigned char>(c))) has_digit = true;
            }
            if (has_letter && has_digit) {
                // Dotted names are accepted only when a digit precedes the
                // first dot (Qwen3-0.6B, v1.2.3); this drops file-like
                // tokens (main.cpp) and bare decimals.
                const std::size_t first_dot = value.find('.');
                const std::size_t first_digit = value.find_first_of("0123456789");
                if (first_dot == std::string::npos || first_digit < first_dot) {
                    add("model", value);
                }
            }
        }
        i = j;
    }

    return out;
}

int count_tokens(const qwen3::Model& model, const std::string& text) {
    if (!model.ok) return static_cast<int>(text.size() / 4);  // rough fallback
    return static_cast<int>(qwen3::encode(model, text).size());
}

// ── Retain ───────────────────────────────────────────────────────────────

namespace {

struct Candidate {
    std::string type = "experience";
    std::string text;
    double confidence = 0.5;
};

bool parse_candidates(const std::string& raw, std::vector<Candidate>& out, std::string& error) {
    const std::string text = strip_code_fences(raw);
    if (text.empty()) {
        error = "aux extraction output is empty";
        return false;
    }
    json::Value parsed;
    try {
        parsed = json::parse(text);
    } catch (const std::exception& e) {
        error = std::string("aux extraction output is not valid JSON: ") + e.what();
        return false;
    }
    if (!parsed.has("candidates")) {
        error = "aux extraction output has no candidates array";
        return false;
    }
    for (const json::Value& item : parsed.at("candidates").as_array()) {
        Candidate candidate;
        candidate.type = item.at("type").as_string("experience");
        candidate.text = item.at("text").as_string("");
        candidate.confidence = item.at("confidence").as_number(0.5);
        out.push_back(std::move(candidate));
    }
    if (out.empty()) {
        error = "aux extraction returned zero candidates";
        return false;
    }
    return true;
}

// ── LLM semantic dedup (extract path) ─────────────────────────────────────
// Top-K similar existing units per candidate (fast, no LLM), then one batched
// LLM judgment (store/skip/update/merge). Adapted to the local 6-type
// vocabulary and the consolidation approval gate. store-all fallback on any
// failure.
constexpr int kDedupTopK = 5;
constexpr double kDedupFloor = kConflictBandLower;  // 0.55: "same topic" floor

struct DedupOutcome {
    std::string action;  // "store" | "skip" | "replace"
    std::string text;    // text to store (candidate text, or merged text)
    std::string type;    // type to store
    std::vector<long long> retire;  // units to propose retiring (replace only)
    double sim = 0.0;  // max cosine to a related unit (the proposal's similarity)
};

struct DedupPoolEntry {
    long long id = 0;
    std::string text;
    std::string type;
};

struct DedupNewEntry {
    std::string id;  // stable id within this batch (c0, c1, ...)
    std::string text;
    std::string type;
    std::vector<long long> related;  // existing unit ids (the pool)
    double max_sim = 0.0;
};

// Top-K active units whose current version embedding is at least `floor`
// cosine to the query vector, best first. Returns (unit_id, cosine).
std::vector<std::pair<long long, double>> similar_units(
    MemoryDb& db, const std::vector<float>& query, int top_k, double floor,
    std::string& error) {
    std::vector<std::pair<long long, double>> out;
    if (query.empty()) return out;
    std::vector<MemoryDb::UnitRow> units = db.units_by_state("active", "", error);
    if (!error.empty() || units.empty()) return out;
    std::vector<long long> version_ids;
    for (const MemoryDb::UnitRow& unit : units) {
        if (unit.current_version_id > 0) version_ids.push_back(unit.current_version_id);
    }
    std::vector<MemoryDb::VersionRow> versions = db.versions_by_ids(version_ids, error);
    if (!error.empty()) return out;
    std::unordered_map<long long, long long> version_to_unit;
    for (const MemoryDb::VersionRow& version : versions) {
        version_to_unit[version.id] = version.memory_id;
    }
    std::vector<MemoryDb::EmbedRow> embeddings = db.active_embeddings(error);
    if (!error.empty()) return out;
    for (const MemoryDb::EmbedRow& row : embeddings) {
        const auto it = version_to_unit.find(row.version_id);
        if (it == version_to_unit.end()) continue;
        const double score = cosine(query, row.vector);
        if (score >= floor) out.push_back({it->second, score});
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (static_cast<int>(out.size()) > top_k) out.resize(top_k);
    return out;
}

std::string build_dedup_user_prompt(const std::vector<DedupPoolEntry>& pool,
                                    const std::vector<DedupNewEntry>& new_entries) {
    json::Array pool_arr;
    for (const DedupPoolEntry& entry : pool) {
        json::Object o;
        o["id"] = json::Value(entry.id);
        o["content"] = json::Value(entry.text);
        o["type"] = json::Value(entry.type);
        pool_arr.push_back(json::Value(std::move(o)));
    }
    json::Array cand_arr;
    for (const DedupNewEntry& entry : new_entries) {
        json::Object o;
        o["id"] = json::Value(entry.id);
        o["content"] = json::Value(entry.text);
        o["type"] = json::Value(entry.type);
        json::Array rel;
        for (const long long id : entry.related) rel.push_back(json::Value(id));
        o["related"] = json::Value(std::move(rel));
        cand_arr.push_back(json::Value(std::move(o)));
    }
    std::ostringstream user;
    user << "## Existing memory pool (" << pool.size() << " records)\n";
    user << json::stringify(json::Value(std::move(pool_arr))) << "\n\n";
    user << "## New candidates (" << new_entries.size() << ")\n";
    user << json::stringify(json::Value(std::move(cand_arr))) << "\n\n";
    user << "Judge each candidate and output the decision JSON array.";
    return user.str();
}

// Parse the LLM's decision array into one outcome per candidate (in order).
// A missing/invalid decision, an invalid action, or a non-JSON response falls
// back to "store" (the candidate's own text) - the store-all safety net.
std::vector<DedupOutcome> parse_dedup_decisions(
    const std::string& raw, const std::vector<DedupNewEntry>& new_entries,
    const std::unordered_set<long long>& pool_ids) {
    std::vector<DedupOutcome> outcomes;
    outcomes.reserve(new_entries.size());
    std::unordered_map<std::string, std::size_t> index_by_id;
    for (std::size_t i = 0; i < new_entries.size(); ++i) {
        index_by_id[new_entries[i].id] = i;
        DedupOutcome outcome;
        outcome.action = "store";
        outcome.text = new_entries[i].text;
        outcome.type = new_entries[i].type;
        outcomes.push_back(std::move(outcome));
    }
    const std::string text = strip_code_fences(raw);
    json::Value parsed;
    try {
        parsed = json::parse(text);
    } catch (...) {
        return outcomes;  // store-all
    }
    const json::Value* decisions = nullptr;
    if (parsed.type() == json::Value::Type::Array) {
        decisions = &parsed;
    } else if (parsed.has("decisions")) {
        decisions = &parsed.at("decisions");
    }
    if (!decisions) return outcomes;
    for (const json::Value& item : decisions->as_array()) {
        if (item.type() != json::Value::Type::Object) continue;
        const std::string id = item.at("id").as_string("");
        const auto it = index_by_id.find(id);
        if (it == index_by_id.end()) continue;  // hallucinated id
        DedupOutcome& outcome = outcomes[it->second];
        const std::string action = item.at("action").as_string("store");
        if (action == "skip") {
            outcome.action = "skip";
            continue;
        }
        if (action == "update" || action == "merge") {
            const std::string merged = item.at("merged_content").as_string("");
            if (merged.empty()) continue;  // no merged text -> keep store
            const std::string merged_type = item.at("merged_type").as_string("");
            std::vector<long long> targets;
            if (item.has("target_ids")) {
                for (const json::Value& t : item.at("target_ids").as_array()) {
                    const long long tid = t.as_integer(0);
                    if (tid > 0 && pool_ids.count(tid) > 0 &&
                        std::find(targets.begin(), targets.end(), tid) == targets.end()) {
                        targets.push_back(tid);
                    }
                }
            }
            outcome.action = "replace";
            outcome.text = merged;
            outcome.type = valid_type(merged_type)
                               ? merged_type
                               : new_entries[it->second].type;
            outcome.retire = std::move(targets);
        }
        // "store" (or unknown) -> keep the default store outcome.
    }
    return outcomes;
}

std::vector<DedupOutcome> dedup_extract(
    MemoryDb& db, const std::vector<Candidate>& candidates,
    const std::vector<std::vector<float>>& embeddings,
    const std::function<std::string(const std::string&)>& dedup) {
    // Default: store all.
    std::vector<DedupOutcome> outcomes;
    outcomes.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        DedupOutcome outcome;
        outcome.action = "store";
        outcome.text = candidate.text;
        outcome.type = candidate.type;
        outcomes.push_back(std::move(outcome));
    }
    if (!dedup) return outcomes;

    // Phase 1: candidate recall per candidate (fast, no LLM).
    std::vector<DedupNewEntry> new_entries;
    new_entries.reserve(candidates.size());
    bool any_related = false;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        DedupNewEntry entry;
        entry.id = "c" + std::to_string(i);
        entry.text = candidates[i].text;
        entry.type = candidates[i].type;
        if (i < embeddings.size() && !embeddings[i].empty()) {
            std::string error;
            const std::vector<std::pair<long long, double>> similar =
                similar_units(db, embeddings[i], kDedupTopK, kDedupFloor, error);
            for (const auto& pair : similar) {
                entry.related.push_back(pair.first);
                if (pair.second > entry.max_sim) entry.max_sim = pair.second;
            }
        }
        if (!entry.related.empty()) any_related = true;
        new_entries.push_back(std::move(entry));
    }
    if (!any_related) return outcomes;  // store-all: nothing similar to judge

    // Unified pool (de-duplicated across candidates), with text + type.
    std::unordered_set<long long> pool_ids;
    std::vector<DedupPoolEntry> pool;
    std::vector<long long> pool_unit_ids;
    for (const DedupNewEntry& entry : new_entries) {
        for (const long long id : entry.related) {
            if (pool_ids.count(id) > 0) continue;
            pool_ids.insert(id);
            pool.push_back({id, "", ""});
            pool_unit_ids.push_back(id);
        }
    }
    std::string error;
    std::vector<MemoryDb::UnitRow> units = db.units_by_ids(pool_unit_ids, error);
    if (!error.empty()) return outcomes;  // store-all
    std::unordered_map<long long, MemoryDb::UnitRow> unit_by_id;
    std::vector<long long> version_ids;
    for (const MemoryDb::UnitRow& unit : units) {
        unit_by_id[unit.id] = unit;
        if (unit.current_version_id > 0) version_ids.push_back(unit.current_version_id);
    }
    std::vector<MemoryDb::VersionRow> versions = db.versions_by_ids(version_ids, error);
    if (!error.empty()) return outcomes;  // store-all
    std::unordered_map<long long, std::string> version_text;
    for (const MemoryDb::VersionRow& version : versions) {
        version_text[version.id] = version.text;
    }
    for (DedupPoolEntry& entry : pool) {
        const auto unit_it = unit_by_id.find(entry.id);
        if (unit_it == unit_by_id.end()) continue;
        entry.type = unit_it->second.type;
        entry.text = version_text[unit_it->second.current_version_id];
    }

    // Phase 2: one batched LLM judgment. Any failure -> store-all.
    const std::string prompt = build_dedup_user_prompt(pool, new_entries);
    std::string raw;
    try {
        raw = dedup(prompt);
    } catch (const std::exception&) {
        return outcomes;  // store-all
    }
    if (raw.empty()) return outcomes;
    outcomes = parse_dedup_decisions(raw, new_entries, pool_ids);
    for (std::size_t i = 0; i < outcomes.size() && i < new_entries.size(); ++i) {
        outcomes[i].sim = new_entries[i].max_sim;
    }
    return outcomes;
}

}  // namespace

RetainResult retain(MemoryDb& db, const qwen3::Model& embed_model, const RetainSpec& spec,
                    const std::function<std::string(const std::string&)>& extract,
                    std::string& error,
                    const std::function<std::string(const std::string&)>& dedup) {
    RetainResult result;
    std::vector<Candidate> candidates;
    if (!spec.direct_text.empty()) {
        Candidate candidate;
        candidate.type = spec.direct_type.empty() ? "experience" : spec.direct_type;
        candidate.text = spec.direct_text;
        candidates.push_back(std::move(candidate));
    } else {
        if (!extract) {
            error = "no extractor available and no direct text";
            return result;
        }
        std::string raw;
        try {
            raw = extract(spec.content);
        } catch (const std::exception& e) {
            error = std::string("aux extraction call failed: ") + e.what();
            return result;
        }
        if (!parse_candidates(raw, candidates, error)) {
            return result;
        }
    }

    const std::string scope =
        spec.session_id.empty() ? "global" : "session:" + spec.session_id;

    // Dedupe policy: exact content-hash duplicates are rejected here; NEAR
    // duplicates are left to the consolidation sweep. On the extract path an
    // LLM dedup judge additionally decides store/skip/update/merge per
    // candidate (see dedup_extract); skip discards the candidate and
    // update/merge store the merged text plus a gated proposal to retire the
    // superseded units. A dedup failure falls back to store-all.
    const std::string default_reason =
        "retained from turn " + (spec.run_id.empty() ? spec.document_id : spec.run_id) +
        " (model " + (spec.model_id.empty() ? "direct" : spec.model_id) +
        (spec.prompt_revision.empty() ? std::string("") : ", prompt " + spec.prompt_revision) +
        "); grounded in the submitted source; source event in the Nyaxus ledger";

    // Pass 1: cheap deterministic gates (length/type/grounding/exact-hash) +
    // embedding. Surviving candidates carry their embedding for dedup recall.
    struct Prepared {
        const Candidate* candidate = nullptr;
        std::vector<float> embedding;
        std::string embedding_model;
    };
    std::vector<Prepared> prepared;
    for (const Candidate& candidate : candidates) {
        if (candidate.text.empty() || candidate.text.size() > 4096) {
            ++result.invalid;
            continue;
        }
        if (!valid_type(candidate.type)) {
            ++result.invalid;
            continue;
        }
        if (spec.direct_text.empty() && !grounded(candidate.text, spec.content)) {
            ++result.ungrounded;
            continue;
        }
        const std::string hash = memory_content_hash(candidate.text);
        if (db.version_exists_by_hash(hash, error)) {
            if (!error.empty()) return result;
            ++result.duplicate;
            continue;
        }
        Prepared p;
        p.candidate = &candidate;
        if (embed_model.ok) {
            p.embedding = embed_truncated(embed_model, candidate.text);
            p.embedding_model = "qwen3-embedding-0.6b";
        }
        prepared.push_back(std::move(p));
    }

    // LLM semantic dedup (extract path only). store-all fallback on any
    // failure: a dedup problem never loses a candidate.
    std::vector<DedupOutcome> outcomes;
    if (spec.direct_text.empty() && !prepared.empty()) {
        std::vector<Candidate> prepared_candidates;
        std::vector<std::vector<float>> prepared_embeddings;
        prepared_candidates.reserve(prepared.size());
        prepared_embeddings.reserve(prepared.size());
        for (const Prepared& p : prepared) {
            prepared_candidates.push_back(*p.candidate);
            prepared_embeddings.push_back(p.embedding);
        }
        outcomes = dedup_extract(db, prepared_candidates, prepared_embeddings, dedup);
    } else {
        for (const Prepared& p : prepared) {
            DedupOutcome outcome;
            outcome.action = "store";
            outcome.text = p.candidate->text;
            outcome.type = p.candidate->type;
            outcomes.push_back(std::move(outcome));
        }
    }

    // Pass 2: apply the outcomes. store -> insert the candidate; skip ->
    // drop; replace -> insert the merged text + propose retiring the targets
    // (the consolidation approval gate).
    for (std::size_t i = 0; i < prepared.size(); ++i) {
        const Prepared& p = prepared[i];
        const DedupOutcome& outcome = outcomes[i];
        if (outcome.action == "skip") {
            ++result.skipped;
            continue;
        }
        const std::string& text = outcome.text;
        const std::string& type = outcome.type;
        if (text.empty() || text.size() > 4096 || !valid_type(type)) {
            ++result.invalid;
            continue;
        }
        // Embed the stored text (a merged text differs from the candidate).
        std::vector<float> embedding;
        std::string embedding_model;
        if (embed_model.ok) {
            embedding = embed_truncated(embed_model, text);
            embedding_model = "qwen3-embedding-0.6b";
        }
        MemoryDb::UnitSpec unit_spec;
        unit_spec.type = type;
        unit_spec.text = text;
        unit_spec.scope = scope;
        unit_spec.session_id = spec.session_id;
        unit_spec.profile = spec.profile;
        unit_spec.run_id = spec.run_id;
        unit_spec.store_reason = spec.store_reason.empty() ? default_reason : spec.store_reason;
        unit_spec.model_id = spec.model_id;
        unit_spec.prompt_revision = spec.prompt_revision;
        unit_spec.generation_id = spec.generation_id;
        unit_spec.confidence = p.candidate->confidence;
        if (!spec.run_id.empty()) {
            unit_spec.sources.push_back({spec.run_id, ""});
        }
        const std::vector<Anchor> anchors = extract_anchors(text);
        for (const Anchor& anchor : anchors) {
            unit_spec.anchors.push_back({anchor.kind, anchor.value});
            unit_spec.entities.push_back(anchor.value);
        }
        unit_spec.embedding = std::move(embedding);
        unit_spec.embedding_model = std::move(embedding_model);
        const long long unit_id = db.insert_unit(unit_spec, error);
        if (!error.empty()) return result;
        if (unit_id <= 0) {
            ++result.invalid;
            continue;
        }
        ++result.stored;
        result.unit_ids.push_back(unit_id);
        // Gated: propose retiring each superseded unit (approval queue). A
        // pair the operator already decided (or that is pending) is skipped;
        // both units get the 24h cooldown so they do not churn.
        for (const long long target : outcome.retire) {
            std::string cerr;
            if (db.has_consolidation_for_pair(target, unit_id, cerr)) {
                if (!cerr.empty()) return result;
                continue;
            }
            if (db.add_consolidation(target, unit_id, outcome.sim, scope, cerr,
                                     "dedup") == 0 &&
                !cerr.empty()) {
                return result;
            }
            db.mark_consolidated(target, cerr);
            db.mark_consolidated(unit_id, cerr);
            if (!cerr.empty()) return result;
        }
    }
    return result;
}

bool process_retain_payload(MemoryDb& db, const qwen3::Model& embed_model,
                            const std::string& payload_json,
                            const std::function<std::string(const std::string&)>& extract,
                            std::string& operation_id, std::string& error,
                            bool& permanent,
                            const std::function<std::string(const std::string&)>& dedup) {
    permanent = false;
    operation_id.clear();
    error.clear();
    json::Value payload;
    try {
        payload = json::parse(payload_json);
    } catch (const std::exception& e) {
        error = std::string("malformed payload: ") + e.what();
        permanent = true;
        return false;
    }
    RetainSpec spec;
    spec.content = payload.at("content").as_string("");
    spec.context = payload.at("context").as_string("");
    spec.document_id = payload.at("document_id").as_string("");
    spec.session_id = payload.at("session_id").as_string("");
    spec.profile = payload.at("profile").as_string("");
    spec.run_id = payload.at("run_id").as_string("");
    if (payload.has("tags")) {
        for (const json::Value& tag : payload.at("tags").as_array()) {
            const std::string text = tag.as_string("");
            if (!text.empty()) spec.tags.push_back(text);
        }
    }
    spec.direct_text = payload.at("direct_text").as_string("");
    spec.direct_type = payload.at("direct_type").as_string("");
    spec.model_id = payload.at("model_id").as_string("");
    spec.store_reason = payload.at("store_reason").as_string("");
    spec.generation_id = payload.at("generation_id").as_string("");
    spec.prompt_id = payload.at("prompt_id").as_string("");
    spec.prompt_sha256 = payload.at("prompt_sha256").as_string("");
    spec.prompt_revision = payload.at("prompt_revision").as_string("");
    if (spec.model_id.empty()) {
        spec.model_id = spec.direct_text.empty() ? "aux" : "direct";
    }
    if (spec.content.empty() && spec.direct_text.empty()) {
        error = "malformed payload: neither content nor direct_text";
        permanent = true;
        return false;
    }

    const auto generation_started = std::chrono::steady_clock::now();
    // Provenance: one best-effort log row per generation. A failure to
    // write the row is logged, never propagated (the generation itself has
    // already happened).
    const std::string gen_kind =
        spec.document_id.rfind("nyaxus-record:", 0) == 0 ? "approved" : "extract";
    auto emit_generation_log =
        [&](const std::string& status, const std::string& message,
            const std::vector<long long>& unit_ids) {
            if (spec.generation_id.empty()) return;
            MemoryDb::GenerationLog gen;
            gen.generation_id = spec.generation_id;
            gen.kind = gen_kind;
            gen.status = status;
            gen.model_id = spec.model_id;
            gen.prompt_id = spec.prompt_id;
            gen.prompt_sha256 = spec.prompt_sha256;
            json::Array inputs;
            for (const std::string& ref : {spec.document_id, spec.session_id,
                                           spec.run_id}) {
                if (!ref.empty()) inputs.push_back(json::Value(ref));
            }
            gen.input_refs = json::stringify(json::Value(std::move(inputs)));
            json::Array outputs;
            for (const long long id : unit_ids) outputs.push_back(json::Value(id));
            gen.output_refs = json::stringify(json::Value(std::move(outputs)));
            gen.session_id = spec.session_id;
            gen.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - generation_started)
                                  .count();
            gen.error = message;
            std::string log_error;
            if (db.insert_generation(gen, log_error) <= 0) {
                log::warn("memory generations: log insert failed: " + log_error);
            }
        };

    // Extraction (aux model) runs before retain: a provider failure is a
    // retryable job failure, while an empty candidate list is a legitimate
    // terminal outcome ("nothing worth remembering").
    std::string raw;
    if (spec.direct_text.empty()) {
        if (!extract) {
            error = "no extractor available and no direct text";
            emit_generation_log("failed", error, {});
            return false;
        }
        try {
            raw = extract(spec.content);
        } catch (const std::exception& e) {
            error = std::string("aux extraction call failed: ") + e.what();
            emit_generation_log("failed", error, {});
            return false;
        }
        if (raw.empty()) {
            error = "aux extraction returned no text";
            emit_generation_log("failed", error, {});
            return false;
        }
        try {
            const json::Value parsed = json::parse(strip_code_fences(raw));
            if (parsed.has("candidates") && parsed.at("candidates").as_array().empty()) {
                emit_generation_log("succeeded", {}, {});
                operation_id = "no candidates extracted";
                return true;
            }
        } catch (...) {
            // Not (yet) valid JSON - parse_candidates below reports it.
        }
    }

    // The extractor is already resolved; retain() only needs the raw output
    // to parse. (The callback form keeps the provider-neutral contract; a
    // direct path never invokes it.)
    auto resolved = [raw](const std::string&) { return raw; };
    RetainResult result = retain(db, embed_model, spec, resolved, error, dedup);
    if (!error.empty()) {
        emit_generation_log("failed", error, result.unit_ids);
        return false;
    }
    emit_generation_log("succeeded", {}, result.unit_ids);
    operation_id = "stored=" + std::to_string(result.stored) +
                   " duplicate=" + std::to_string(result.duplicate) +
                   " skipped=" + std::to_string(result.skipped) +
                   " ungrounded=" + std::to_string(result.ungrounded) +
                   " invalid=" + std::to_string(result.invalid);
    return true;
}

// ── Recall ───────────────────────────────────────────────────────────────

namespace {

struct GatedUnit {
    MemoryDb::UnitRow unit;
    MemoryDb::VersionRow version;
    bool in_scope = false;
};

}  // namespace

RecallResult recall(MemoryDb& db, const qwen3::Model& embed_model,
                    const qwen3::Model& rerank_model, const RecallRequest& req) {
    RecallResult result;
    std::string error;
    StageTimer total("recall (total)");

    // Wall-clock budget (0 = no deadline): checked at stage boundaries so an
    // expired recall returns partial results (RRF order, no rerank) instead
    // of blocking the caller past the operator's timeout.
    const std::chrono::steady_clock::time_point deadline =
        req.deadline_ms > 0
            ? std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(req.deadline_ms)
            : std::chrono::steady_clock::time_point::max();
    const auto deadline_exceeded = [&]() {
        return std::chrono::steady_clock::now() >= deadline;
    };

    // Load active units with their current versions.
    StageTimer stage_load("load units+versions");
    std::vector<MemoryDb::UnitRow> units = db.units_by_state("active", "", error);
    if (!error.empty()) {
        result.error = error;
        return result;
    }
    std::vector<long long> version_ids;
    for (const MemoryDb::UnitRow& unit : units) {
        if (unit.current_version_id > 0) version_ids.push_back(unit.current_version_id);
    }
    std::vector<MemoryDb::VersionRow> versions = db.versions_by_ids(version_ids, error);
    if (!error.empty()) {
        result.error = error;
        return result;
    }
    std::unordered_map<long long, MemoryDb::VersionRow> version_by_id;
    for (MemoryDb::VersionRow& version : versions) {
        version_by_id.emplace(version.id, std::move(version));
    }

    // Scope/state/suppression gate (blueprint stage 5). Applied uniformly to
    // every signal so no path can bypass it.
    auto suppressed_unit = [&](const MemoryDb::UnitRow& unit) {
        if (req.suppressed.empty()) return false;
        if (req.suppressed.count(unit.run_id) || req.suppressed.count(unit.session_id)) {
            return true;
        }
        if (!unit.run_id.empty() && req.suppressed.count("nyaxus-turn:" + unit.run_id)) {
            return true;
        }
        return false;
    };
    auto in_scope = [&](const MemoryDb::UnitRow& unit) {
        if (!req.session_id.empty()) {
            const bool ok = unit.scope == "global" || unit.session_id == req.session_id ||
                            unit.scope == "session:" + req.session_id;
            if (!ok) return false;
        }
        if (!req.profile.empty()) {
            // A unit with no profile is not tied to any profile (operator
            // records, imported memories) and stays visible under any profile
            // request; profile is provenance metadata, not a hard isolation
            // boundary (settled scope decision, 2026-09-11).
            const bool ok = unit.scope == "global" || unit.profile.empty() ||
                            unit.profile == req.profile ||
                            unit.scope == "profile:" + req.profile;
            if (!ok) return false;
        }
        return true;
    };
    std::unordered_map<long long, MemoryDb::UnitRow> gated;  // unit_id -> row
    for (const MemoryDb::UnitRow& unit : units) {
        if (!in_scope(unit) || suppressed_unit(unit)) continue;
        gated[unit.id] = unit;
    }
    auto version_text = [&](long long unit_id) -> const std::string* {
        const auto unit_it = gated.find(unit_id);
        if (unit_it == gated.end()) return nullptr;
        const auto version_it = version_by_id.find(unit_it->second.current_version_id);
        if (version_it == version_by_id.end()) return nullptr;
        return &version_it->second.text;
    };
    auto version_of = [&](long long unit_id) -> long long {
        const auto unit_it = gated.find(unit_id);
        return unit_it == gated.end() ? 0 : unit_it->second.current_version_id;
    };

    if (deadline_exceeded()) {
        result.ok = true;
        result.timed_out = true;
        result.error = "recall deadline expired before the signal stages; "
                       "no results";
        return result;
    }

    // Anchors are extracted once and shared by Stage 1 (exact) and Stage 4a
    // (entity/model).
    const std::vector<Anchor> anchors = extract_anchors(req.query);

    // Stage 1: exact anchors (url/path/hash/uuid).
    StageTimer stage_exact("exact anchors");
    std::vector<long long> exact_list;
    for (const Anchor& anchor : anchors) {
        if (anchor.kind != "url" && anchor.kind != "path" && anchor.kind != "hash" &&
            anchor.kind != "uuid") {
            continue;
        }
        for (const MemoryDb::AnchorHit& hit :
             db.anchors_matching({anchor.value}, error)) {
            if (!error.empty()) {
                result.error = error;
                return result;
            }
            const auto version_it = version_by_id.find(hit.version_id);
            if (version_it == version_by_id.end()) continue;
            const long long unit_id = version_it->second.memory_id;
            if (gated.count(unit_id) == 0) continue;
            if (std::find(exact_list.begin(), exact_list.end(), unit_id) == exact_list.end()) {
                exact_list.push_back(unit_id);
            }
        }
    }

    // Stage 2: semantic (embedding scan over active versions).
    StageTimer stage_semantic("semantic");
    std::vector<long long> semantic_list;
    std::vector<float> query_vector;
    if (embed_model.ok) {
        query_vector = embed_truncated(embed_model, req.query);
        // TODO(scaling): full-table load per recall (~16 MB at 4K memories).
        // Fine at current scale; stream or cache when N > 10K.
        const std::vector<MemoryDb::EmbedRow> embeddings = db.active_embeddings(error);
        if (!error.empty()) {
            result.error = error;
            return result;
        }
        struct Scored {
            long long unit_id;
            double score;
        };
        std::vector<Scored> scored;
        for (const MemoryDb::EmbedRow& row : embeddings) {
            const auto version_it = version_by_id.find(row.version_id);
            if (version_it == version_by_id.end()) continue;
            const long long unit_id = version_it->second.memory_id;
            if (gated.count(unit_id) == 0) continue;
            scored.push_back({unit_id, cosine(query_vector, row.vector)});
        }
        std::sort(scored.begin(), scored.end(),
                  [](const Scored& a, const Scored& b) { return a.score > b.score; });
        for (std::size_t i = 0; i < scored.size() && i < static_cast<std::size_t>(req.semantic_top);
             ++i) {
            semantic_list.push_back(scored[i].unit_id);
        }
    }

    // Stage 3: lexical (FTS5/BM25).
    StageTimer stage_lexical("lexical");
    std::vector<long long> lexical_list;
    const std::string match = fts_match(req.query);
    if (!match.empty()) {
        const std::vector<MemoryDb::FtsHit> hits =
            db.fts_search(match, req.lexical_top, error);
        if (!error.empty()) {
            result.error = error;
            return result;
        }
        for (const MemoryDb::FtsHit& hit : hits) {
            if (gated.count(hit.memory_id) == 0) continue;
            lexical_list.push_back(hit.memory_id);
        }
    }

    // Stage 4a: entity (model-name / entity anchors in the query).
    StageTimer stage_entity("entity+temporal");
    std::vector<long long> entity_list;
    for (const Anchor& anchor : anchors) {
        if (anchor.kind != "model") continue;
        for (const MemoryDb::AnchorHit& hit : db.anchors_matching({anchor.value}, error)) {
            if (!error.empty()) {
                result.error = error;
                return result;
            }
            const auto version_it = version_by_id.find(hit.version_id);
            if (version_it == version_by_id.end()) continue;
            const long long unit_id = version_it->second.memory_id;
            if (gated.count(unit_id) == 0) continue;
            if (std::find(entity_list.begin(), entity_list.end(), unit_id) == entity_list.end()) {
                entity_list.push_back(unit_id);
            }
        }
    }

    // Stage 4b: temporal (ISO dates in the query vs mentioned_at/occurred).
    std::vector<long long> temporal_list;
    for (const std::string& date : dates_in(req.query)) {
        for (const auto& [unit_id, unit] : gated) {
            (void)unit_id;
            const bool matches =
                (unit.mentioned_at.size() >= 10 && unit.mentioned_at.compare(0, 10, date) == 0) ||
                (unit.occurred_start.size() >= 10 &&
                 unit.occurred_start.compare(0, 10, date) == 0) ||
                (unit.occurred_end.size() >= 10 && unit.occurred_end.compare(0, 10, date) == 0) ||
                (unit.occurred_start.size() >= 10 && unit.occurred_end.size() >= 10 &&
                 unit.occurred_start.compare(0, 10, date) <= 0 &&
                 unit.occurred_end.compare(0, 10, date) >= 0);
            if (matches && std::find(temporal_list.begin(), temporal_list.end(), unit.id) ==
                               temporal_list.end()) {
                temporal_list.push_back(unit.id);
            }
        }
    }

    // Stage 6: RRF merge over the non-empty lists.
    StageTimer stage_rrf("rrf merge");
    std::map<long long, double> rrf_scores;
    std::map<long long, std::map<std::string, int>> ranks;
    auto add_list = [&](const std::vector<long long>& list, const char* signal) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            rrf_scores[list[i]] += 1.0 / (req.rrf_k + static_cast<double>(i + 1));
            ranks[list[i]][signal] = static_cast<int>(i + 1);
        }
    };
    add_list(exact_list, "exact");
    add_list(semantic_list, "semantic");
    add_list(lexical_list, "lexical");
    add_list(entity_list, "entity");
    add_list(temporal_list, "temporal");

    std::vector<long long> pool;
    pool.reserve(rrf_scores.size());
    for (const auto& [unit_id, score] : rrf_scores) pool.push_back(unit_id);
    std::sort(pool.begin(), pool.end(),
              [&](long long a, long long b) { return rrf_scores[a] > rrf_scores[b]; });
    if (static_cast<int>(pool.size()) > req.rrf_pool) {
        pool.resize(static_cast<std::size_t>(req.rrf_pool));
    }
    result.pool_size = static_cast<int>(pool.size());
    if (pool.empty()) {
        result.ok = true;  // a valid query with no matches is not an error
        return result;
    }
    // RRF rank per unit, captured before stage 7 re-sorts the pool - the
    // final pool order is the reranked order, not the RRF order.
    std::map<long long, long long> rrf_rank;
    for (std::size_t i = 0; i < pool.size(); i++)
        rrf_rank[pool[i]] = static_cast<long long>(i + 1);

    // Stage 7: cross-encoder rerank of the top of the pool (RRF-only
    // fallback when the reranker is unavailable). The rerank is the
    // expensive stage (one cross-encoder pass over the top of the pool),
    // so the deadline check lands here: on expiry the pool keeps its RRF
    // order and the result is flagged partial.
    StageTimer stage_rerank("rerank");
    bool reranked = false;
    std::map<long long, float> rerank_scores;
    const std::size_t rerank_count =
        std::min(pool.size(), static_cast<std::size_t>(req.rerank_top));
    if (deadline_exceeded()) {
        result.timed_out = true;
        result.error = "recall deadline expired before rerank; partial "
                       "results in RRF order";
    }
    if (rerank_model.ok && !req.skip_rerank && !result.timed_out) {
        bool all_ok = true;
        std::vector<std::string> texts;
        texts.reserve(rerank_count);
        for (std::size_t i = 0; i < rerank_count; ++i) {
            const std::string* text = version_text(pool[i]);
            if (!text) {
                all_ok = false;
                break;
            }
            texts.push_back(*text);
        }
        if (all_ok && !texts.empty()) {
            // One call for all docs: the template+query prefix is prefilled
            // once into the KV cache, then each doc suffix is scored against
            // it (scores are identical to per-doc rerank calls).
            std::vector<float> many =
                qwen3::rerank_many(rerank_model, req.query, texts);
            for (std::size_t i = 0; i < rerank_count && i < many.size(); ++i)
                rerank_scores[pool[i]] = many[i];
        }
        if (all_ok && !rerank_scores.empty()) {
            std::vector<long long> ordered(pool.begin(), pool.begin() + rerank_count);
            std::sort(ordered.begin(), ordered.end(), [&](long long a, long long b) {
                if (rerank_scores[a] != rerank_scores[b]) {
                    return rerank_scores[a] > rerank_scores[b];
                }
                return rrf_scores[a] > rrf_scores[b];
            });
            ordered.insert(ordered.end(), pool.begin() + rerank_count, pool.end());
            pool = std::move(ordered);
            reranked = true;
        }
    }
    result.reranker_used = reranked;

    // Stage 8: token budget (priority: exact-anchor > lexical > relevance;
    // observation type is a soft tiebreaker, not a hard class).
    StageTimer stage_budget("budget+reason");
    struct Final {
        long long unit_id;
        double score;
        bool exact;
        bool lexical;
        bool observation;
        double confidence = 0.5;
    };
    std::vector<Final> finals;
    for (const long long unit_id : pool) {
        const auto unit_it = gated.find(unit_id);
        if (unit_it == gated.end()) continue;
        Final final;
        final.unit_id = unit_id;
        const auto score_it = rerank_scores.find(unit_id);
        final.score = (reranked && score_it != rerank_scores.end())
                          ? score_it->second
                          : rrf_scores[unit_id];
        final.exact = ranks[unit_id].count("exact") != 0;
        final.lexical = ranks[unit_id].count("lexical") != 0;
        final.observation = unit_it->second.type == "observation";
        final.confidence = unit_it->second.confidence;
        finals.push_back(std::move(final));
    }
    std::sort(finals.begin(), finals.end(), [](const Final& a, const Final& b) {
        if (a.exact != b.exact) return a.exact;
        // A lexical hit is direct evidence for the user's words. It should not
        // be buried under unrelated observation rows merely because the latter
        // have a preferred memory type.
        if (a.lexical != b.lexical) return a.lexical;
        // Relevance dominates within a class: the rerank score (or the RRF
        // rank across all signals when the reranker is unavailable) is the
        // primary ordering. A memory that ranks first on every signal must not
        // be buried below weaker rows that merely carry a preferred type.
        if (a.score > b.score) return true;
        if (a.score < b.score) return false;
        // Confidence (validation/rejection lifecycle): at equal relevance, a
        // well-validated memory ranks above an unvalidated one. Recall alone
        // does not move confidence, so this only reflects use/rejection.
        if (a.confidence != b.confidence) return a.confidence > b.confidence;
        // Type preference is a soft tiebreaker for equal relevance, not a hard
        // class.
        return a.observation && !b.observation;
    });

    // Per-item cap: exact token truncation (keep the first cap tokens,
    // mark the cut so the model sees the bound, re-count the marked text so
    // tokens_used stays honest). Without the truncation the cap was
    // accounting-only and the injected total could exceed the budget by up
    // to (actual - cap) tokens per item.
    auto bounded_text = [&](const std::string& text, int& tokens) -> std::string {
        tokens = count_tokens(embed_model, text);
        if (tokens <= req.per_item_cap_tokens) return text;
        std::vector<int> ids = qwen3::encode(embed_model, text);
        ids.resize(static_cast<std::size_t>(req.per_item_cap_tokens));
        std::string bounded = qwen3::decode(embed_model, ids) + " [truncated]";
        tokens = count_tokens(embed_model, bounded);
        result.truncated++;
        return bounded;
    };

    int remaining = req.budget_tokens;
    for (const Final& final : finals) {
        if (remaining <= 0) break;
        // Recall floor (Hillock's honest no-data): a below-floor hit is a
        // near-miss, and priming the model with one reads as evidence. The
        // score is the rerank logit when the reranker ran, the RRF score in
        // the RRF-only fallback, so the operator tunes one number per regime.
        if (req.recall_floor > 0.0 && final.score < req.recall_floor) {
            result.suppressed_below_floor++;
            continue;
        }
        const std::string* text = version_text(final.unit_id);
        if (!text) continue;
        int tokens = 0;
        std::string injected = bounded_text(*text, tokens);
        if (tokens > remaining) {
            result.budget_dropped++;
            continue;
        }
        remaining -= tokens;
        result.tokens_used += tokens;

        RecallHit hit;
        hit.unit_id = final.unit_id;
        hit.version_id = version_of(final.unit_id);
        hit.text = std::move(injected);
        hit.tokens = tokens;
        const auto unit_it = gated.find(final.unit_id);
        hit.type = unit_it->second.type;
        hit.scope = unit_it->second.scope;
        hit.session_id = unit_it->second.session_id;
        hit.profile = unit_it->second.profile;
        hit.run_id = unit_it->second.run_id;
        const auto version_it = version_by_id.find(hit.version_id);
        hit.store_reason =
            version_it != version_by_id.end() ? version_it->second.store_reason : "";
        hit.reranked = reranked;
        const auto score_it = rerank_scores.find(final.unit_id);
        if (score_it != rerank_scores.end()) hit.rerank_score = score_it->second;
        hit.rrf_score = rrf_scores[final.unit_id];
        for (const auto& [signal, rank] : ranks[final.unit_id]) {
            hit.signals.push_back(signal);
        }
        std::sort(hit.signals.begin(), hit.signals.end());

        // Stage 9: required deterministic reason.
        json::Object reason;
        json::Array signals;
        for (const std::string& signal : hit.signals) signals.push_back(json::Value(signal));
        reason["signals"] = json::Value(std::move(signals));
        reason["rrf_score"] = json::Value(hit.rrf_score);
        const auto rrank_it = rrf_rank.find(final.unit_id);
        reason["rrf_rank"] = json::Value(rrank_it != rrf_rank.end()
                                              ? static_cast<long long>(rrank_it->second)
                                              : 0);
        reason["reranked"] = json::Value(reranked);
        reason["rerank_score"] = json::Value(static_cast<double>(hit.rerank_score));
        json::Object ranks_obj;
        for (const char* signal : {"exact", "semantic", "lexical", "entity", "temporal"}) {
            const auto rank_it = ranks[final.unit_id].find(signal);
            ranks_obj[signal] = rank_it != ranks[final.unit_id].end()
                                   ? json::Value(static_cast<long long>(rank_it->second))
                                   : json::Value();
        }
        reason["ranks"] = json::Value(std::move(ranks_obj));
        reason["tokens"] = json::Value(static_cast<long long>(tokens));
        json::Object source;
        source["session"] = json::Value(hit.session_id);
        source["run"] = json::Value(hit.run_id);
        source["profile"] = json::Value(hit.profile);
        source["document"] = json::Value(hit.run_id.empty() ? "" : "nyaxus-turn:" + hit.run_id);
        reason["source"] = json::Value(std::move(source));
        reason["store_reason"] = json::Value(hit.store_reason);
        hit.reason = std::move(reason);

        result.hits.push_back(std::move(hit));
    }

    // Stage 10: bounded 1-hop link expansion on the survivors.
    StageTimer stage_links("link expansion");
    if (result.hits.size() < 64 && req.link_expansion_max > 0) {
        const int link_budget =
            static_cast<int>(req.budget_tokens * req.link_expansion_budget_ratio);
        int link_tokens = 0;
        int link_slots = req.link_expansion_max;
        std::set<long long> included;
        for (const RecallHit& hit : result.hits) included.insert(hit.unit_id);
        // Snapshot the survivors: the loop below appends to result.hits.
        std::vector<RecallHit> survivors(result.hits.begin(), result.hits.end());
        for (const RecallHit& hit : survivors) {
            if (link_slots == 0) break;
            const std::vector<MemoryDb::LinkRow> links =
                db.links_from(hit.unit_id, "active", error);
            if (!error.empty()) {
                result.error = error;
                return result;
            }
            for (const MemoryDb::LinkRow& link : links) {
                if (included.count(link.target_id)) continue;
                const auto unit_it = gated.find(link.target_id);
                if (unit_it == gated.end()) continue;
                const std::string* text = version_text(link.target_id);
                if (!text) continue;
                int tokens = 0;
                std::string injected = bounded_text(*text, tokens);
                if (link_tokens + tokens > link_budget) {
                    result.budget_dropped++;
                    continue;
                }
                link_tokens += tokens;
                result.tokens_used += tokens;

                RecallHit extra;
                extra.unit_id = link.target_id;
                extra.version_id = version_of(link.target_id);
                extra.text = std::move(injected);
                extra.tokens = tokens;
                const auto version_it = version_by_id.find(extra.version_id);
                extra.store_reason =
                    version_it != version_by_id.end() ? version_it->second.store_reason : "";
                extra.type = unit_it->second.type;
                extra.scope = unit_it->second.scope;
                extra.session_id = unit_it->second.session_id;
                extra.profile = unit_it->second.profile;
                extra.run_id = unit_it->second.run_id;
                extra.signals.push_back("link");
                json::Object reason;
                json::Array signals;
                signals.push_back(json::Value("link"));
                reason["signals"] = json::Value(std::move(signals));
                reason["link_type"] = json::Value(link.link_type);
                reason["via_unit"] = json::Value(static_cast<long long>(hit.unit_id));
                reason["tokens"] = json::Value(static_cast<long long>(tokens));
                extra.reason = std::move(reason);
                result.hits.push_back(std::move(extra));
                included.insert(link.target_id);
                --link_slots;
                break;
            }
        }
    }

    result.ok = true;
    return result;
}

// ── Browse ───────────────────────────────────────────────────────────────

BrowseResult browse(MemoryDb& db, const BrowseRequest& req) {
    BrowseResult result;
    std::string error;
    const std::string match = req.text.empty() ? std::string() : fts_match(req.text);
    if (!req.text.empty() && match.empty()) {
        result.ok = true;  // no usable tokens: a valid, empty result
        return result;
    }
    const MemoryDb::BrowsePage page = db.browse_units(req.state, req.type, req.scope, match,
                                                      req.limit, req.offset, error);
    if (!error.empty()) {
        result.error = error;
        return result;
    }
    result.total = page.total;
    if (!page.units.empty()) {
        std::vector<long long> version_ids;
        for (const MemoryDb::UnitRow& unit : page.units) {
            if (unit.current_version_id > 0) version_ids.push_back(unit.current_version_id);
        }
        std::string verror;
        const std::vector<MemoryDb::VersionRow> versions = db.versions_by_ids(version_ids, verror);
        std::map<long long, const MemoryDb::VersionRow*> by_id;
        for (const MemoryDb::VersionRow& version : versions) {
            by_id[version.id] = &version;
        }
        for (const MemoryDb::UnitRow& unit : page.units) {
            BrowseUnit entry;
            entry.unit_id = unit.id;
            entry.type = unit.type;
            entry.state = unit.state;
            entry.state_reason = unit.state_reason;
            entry.scope = unit.scope;
            entry.session_id = unit.session_id;
            entry.profile = unit.profile;
            entry.run_id = unit.run_id;
            entry.confidence = unit.confidence;
            entry.mentioned_at = unit.mentioned_at;
            entry.created_at = unit.created_at;
            entry.updated_at = unit.updated_at;
            entry.core = unit.core;
            const auto it = by_id.find(unit.current_version_id);
            if (it != by_id.end()) {
                entry.text = it->second->text;
                entry.store_reason = it->second->store_reason;
                entry.model_id = it->second->model_id;
            }
            result.units.push_back(std::move(entry));
        }
    }
    result.ok = true;
    return result;
}

// ── Curate ───────────────────────────────────────────────────────────────

bool supersede(MemoryDb& db, const qwen3::Model& embed_model, long long unit_id,
               const std::string& new_text, const std::string& reason, std::string& error) {
    std::vector<float> embedding;
    std::string embedding_model;
    if (embed_model.ok) {
        embedding = embed_truncated(embed_model, new_text);
        embedding_model = "qwen3-embedding-0.6b";
    }
    return db.supersede(unit_id, new_text, reason, "direct", std::move(embedding),
                        embedding_model, error);
}

bool set_state(MemoryDb& db, long long unit_id, const std::string& state,
               const std::string& reason, std::string& error) {
    return db.set_state(unit_id, state, reason, error);
}

bool erase(MemoryDb& db, long long unit_id, std::string& error) {
    return db.erase(unit_id, error);
}

// ── Consolidation sweep ──────────────────────────────────────────────────

SweepResult consolidate_sweep(MemoryDb& db, const qwen3::Model& embed_model,
                              const std::string& trigger, std::string& error,
                              bool full_scan, double threshold) {
    SweepResult result;
    const auto last = db.last_sweep(error);
    if (!error.empty()) return result;
    // Incremental selection by default: only units updated since the last
    // sweep's watermark. A full scan (manual trigger) ignores the watermark
    // and checks the whole active bank - the operator asked for it.
    const std::string watermark = (full_scan || !last) ? std::string() : last->watermark;
    std::vector<MemoryDb::UnitRow> candidates =
        db.active_units_updated_since(watermark, error);
    if (!error.empty()) return result;
    // Total active units, for honest reporting when the watermark filter
    // (or an empty bank) leaves nothing to scan.
    std::string cerr;
    for (const auto& [state, count] : db.unit_state_counts(cerr)) {
        if (state == "active") result.active_total = static_cast<int>(count);
    }

    // Per-memory cooldown: no re-consolidation within ~24h.
    const std::string cooldown_cutoff =
        iso_utc(std::chrono::system_clock::now() - std::chrono::hours(24));
    std::vector<MemoryDb::UnitRow> selected;
    for (const MemoryDb::UnitRow& unit : candidates) {
        if (!unit.last_consolidated_at.empty() && unit.last_consolidated_at > cooldown_cutoff) {
            continue;
        }
        selected.push_back(unit);
    }
    result.selected = static_cast<int>(selected.size());
    result.candidates = static_cast<int>(candidates.size());
    result.in_cooldown = result.candidates - result.selected;

    // Near-duplicate collapse: cosine above the threshold within the same
    // scope. Conflict detection (the mid-band stage below) needs the pair's
    // texts, so load the selected units' current versions alongside.
    std::unordered_map<long long, std::vector<float>> vectors;
    std::unordered_map<long long, std::string> text_by_unit;
    if (embed_model.ok && !selected.empty()) {
        // Non-const local so the vectors are actually moved (a const row
        // would make std::move a silent copy of 1024 floats per row).
        std::vector<MemoryDb::EmbedRow> embeddings = db.active_embeddings(error);
        if (!error.empty()) return result;
        for (MemoryDb::EmbedRow& row : embeddings) {
            vectors[row.version_id] = std::move(row.vector);
        }
        std::vector<long long> version_ids;
        for (const MemoryDb::UnitRow& unit : selected) {
            if (unit.current_version_id > 0) version_ids.push_back(unit.current_version_id);
        }
        std::vector<MemoryDb::VersionRow> versions = db.versions_by_ids(version_ids, error);
        if (!error.empty()) return result;
        std::unordered_map<long long, long long> unit_of_version;
        for (const MemoryDb::UnitRow& unit : selected) {
            unit_of_version[unit.current_version_id] = unit.id;
        }
        for (const MemoryDb::VersionRow& version : versions) {
            const auto unit_it = unit_of_version.find(version.id);
            if (unit_it != unit_of_version.end()) {
                text_by_unit[unit_it->second] = version.text;
            }
        }
    }
    int proposed = 0;
    int conflicts = 0;
    // Track the closest pairs that stayed under the threshold, so an empty
    // sweep can be explained with real numbers ("the closest pair was 0.87,
    // not 0.92+") instead of a bare zero.
    std::vector<std::pair<double, std::string>> closest;
    auto consider_closest = [&](double similarity, const std::string& label) {
        closest.emplace_back(similarity, label);
        std::sort(closest.begin(), closest.end(),
                  [](const std::pair<double, std::string>& x,
                     const std::pair<double, std::string>& y) { return x.first > y.first; });
        if (closest.size() > 3) closest.resize(3);
    };
    // Shared proposal path for near-duplicates and conflicts: keep the newer
    // unit, *propose* retiring the older one, gate on pair decisions. Nothing
    // is superseded until the operator approves the candidate. Returns false
    // only on a hard error (error set); an already-decided pair is a no-op
    // true, not a new proposal.
    auto propose_pair = [&](const MemoryDb::UnitRow& a, const MemoryDb::UnitRow& b,
                            double similarity, const char* kind,
                            const std::string& note) -> bool {
        const bool a_is_older =
            a.updated_at < b.updated_at || (a.updated_at == b.updated_at && a.id < b.id);
        const long long older_id = a_is_older ? a.id : b.id;
        const long long newer_id = a_is_older ? b.id : a.id;
        std::string perr;
        // A pair the operator already decided (or that is still pending)
        // is never re-proposed.
        if (db.has_consolidation_for_pair(older_id, newer_id, perr)) {
            if (!perr.empty()) { error = perr; return false; }
            return true;
        }
        const long long candidate_id =
            db.add_consolidation(older_id, newer_id, similarity,
                                 a_is_older ? a.scope : b.scope, perr, kind);
        if (perr.empty() && candidate_id == 0) perr = "consolidation proposal insert failed";
        if (!perr.empty()) { error = perr; return false; }
        // 24h cooldown so pending/rejected pairs do not churn on every
        // sweep (the pair check above is the real gate).
        db.mark_consolidated(older_id, error);
        db.mark_consolidated(newer_id, error);
        if (!error.empty()) return false;
        std::ostringstream sim;
        sim << std::fixed << std::setprecision(3) << similarity;
        result.proposed_lines.push_back(
            "unit " + std::to_string(older_id) + " proposed for unit " +
            std::to_string(newer_id) + " (" + kind + ", cosine " + sim.str() + ", scope " +
            (a_is_older ? a.scope : b.scope) +
            (note.empty() ? "" : ", " + note) + ", candidate " +
            std::to_string(candidate_id) + ")");
        ++proposed;
        return true;
    };
    for (std::size_t i = 0; i < selected.size(); ++i) {
        for (std::size_t j = i + 1; j < selected.size(); ++j) {
            const MemoryDb::UnitRow& a = selected[i];
            const MemoryDb::UnitRow& b = selected[j];
            if (a.scope != b.scope) continue;
            auto va = vectors.find(a.current_version_id);
            auto vb = vectors.find(b.current_version_id);
            if (va == vectors.end() || vb == vectors.end()) continue;
            const double similarity = cosine(va->second, vb->second);
            if (similarity <= threshold) {
                // Conflict band: same-topic texts (mid-band cosine, not a
                // near-duplicate) with disjoint value sets assert different
                // facts - propose them for the operator like near-dups do.
                // Below the band the pair is just "same area, different
                // thing"; only the closest-pair breadcrumb records it.
                bool conflict = false;
                std::string conflict_why;
                if (similarity >= kConflictBandLower) {
                    const auto ta = text_by_unit.find(a.id);
                    const auto tb = text_by_unit.find(b.id);
                    if (ta != text_by_unit.end() && tb != text_by_unit.end()) {
                        conflict = detect_conflict(ta->second, tb->second, conflict_why);
                    }
                }
                if (conflict) {
                    if (propose_pair(a, b, similarity, "conflict", conflict_why)) {
                        ++conflicts;
                    }
                } else {
                    consider_closest(similarity, "unit " + std::to_string(a.id) +
                                                 " x unit " + std::to_string(b.id) +
                                                 " (scope " + a.scope + ")");
                }
                continue;
            }
            propose_pair(a, b, similarity, "near_dup", "");
        }
    }
    result.proposed = proposed;
    result.conflicts_proposed = conflicts;
    result.fixpoint = (proposed == 0);
    if (proposed == 0) {
        for (const auto& [similarity, label] : closest) {
            std::ostringstream sim;
            sim << std::fixed << std::setprecision(3) << similarity;
            result.closest_below.push_back(label + " (cosine " + sim.str() + ")");
        }
    }
    db.record_sweep(trigger.empty() ? "manual" : trigger, iso_utc(std::chrono::system_clock::now()),
                    static_cast<long long>(result.selected),
                    static_cast<long long>(result.proposed), result.fixpoint, error);
    return result;
}

bool decide_consolidation(MemoryDb& db, long long candidate_id, bool approve,
                          std::string& error) {
    const auto cand = db.get_consolidation(candidate_id);
    if (!cand || cand->status != "pending") {
        error = "consolidation candidate not found or already decided";
        return false;
    }
    if (approve) {
        std::string uerr;
        const std::vector<MemoryDb::UnitRow> units =
            db.units_by_ids({cand->older_unit_id, cand->newer_unit_id}, uerr);
        if (!uerr.empty()) { error = uerr; return false; }
        const MemoryDb::UnitRow* older = nullptr;
        const MemoryDb::UnitRow* newer = nullptr;
        for (const MemoryDb::UnitRow& unit : units) {
            if (unit.id == cand->older_unit_id) older = &unit;
            if (unit.id == cand->newer_unit_id) newer = &unit;
        }
        if (!older || !newer || older->state != "active" || newer->state != "active") {
            error = "consolidation units are no longer both active";
            return false;
        }
        std::ostringstream sim;
        sim << std::fixed << std::setprecision(3) << cand->cosine;
        const bool conflict = cand->kind == "conflict";
        // A conflict approval is a truth decision: the operator has read
        // both texts and chosen the newer one as the standing fact.
        const std::string reason =
            conflict
                ? "conflicts with unit " + std::to_string(cand->newer_unit_id) +
                      " (value mismatch, cosine " + sim.str() +
                      "); conflict candidate approved by operator"
                : "near-duplicate of unit " + std::to_string(cand->newer_unit_id) +
                      " (cosine " + sim.str() +
                      "); consolidation approved by operator";
        if (!db.set_state(cand->older_unit_id, "superseded", reason, error)) {
            if (!error.empty()) return false;
            error = "supersede failed";
            return false;
        }
        // Core-pin transfer: the pin belongs to the fact, not the row. If the
        // retiring unit was pinned, the survivor inherits the pin, so an
        // approval never silently drops a core unit from the injected set.
        if (older->core && !newer->core) {
            db.set_core(cand->newer_unit_id, true, error);
            if (!error.empty()) return false;
        }
        db.add_link(cand->older_unit_id, cand->newer_unit_id,
                    conflict ? "contradiction" : "semantic", "active", 0, error);
        if (!error.empty()) return false;
        db.mark_consolidated(cand->older_unit_id, error);
        db.mark_consolidated(cand->newer_unit_id, error);
        if (!error.empty()) return false;
    }
    return db.set_consolidation_status(candidate_id, approve ? "approved" : "rejected", error);
}

// ── L2 scene refresh + L3 persona (derived distillation) ────────────────
// Layered distillation (L1 units -> L2 scenes -> L3 persona) on the
// relational core: the aux model integrates the bank's new units into the
// scene set and rewrites the persona from the scenes. Both are derived
// artifacts - a failed call leaves the existing state unchanged (the L1
// units are the source of truth), so these paths never lose a memory.
constexpr int kSceneMinNewUnits = 5;   // below this there is no new material
constexpr int kSceneMaxScenes = 20;    // hard cap on the scene set
constexpr int kSceneMaxNewUnits = 100;  // prompt cap on the new units
constexpr int kSceneContentMax = 4000;  // per-scene content cap (chars)
constexpr int kPersonaMaxChars = 4000;  // hard validation cap (prompt asks <= 2000)
constexpr int kPersonaSceneBudget = 12000;  // scene content chars in the prompt

bool valid_scene_slug(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (const char c : name) {
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-') {
            return false;
        }
    }
    return true;
}

std::string build_scenes_user_prompt(
    const std::vector<MemoryDb::SceneRow>& scenes,
    const std::vector<MemoryDb::UnitRow>& units,
    const std::unordered_map<long long, std::string>& texts) {
    json::Array scene_arr;
    for (const MemoryDb::SceneRow& scene : scenes) {
        json::Object o;
        o["id"] = json::Value(scene.id);
        o["name"] = json::Value(scene.name);
        o["summary"] = json::Value(scene.summary);
        o["heat"] = json::Value(static_cast<long long>(scene.heat));
        o["content"] = json::Value(scene.content);
        scene_arr.push_back(json::Value(std::move(o)));
    }
    json::Array unit_arr;
    for (const MemoryDb::UnitRow& unit : units) {
        const auto it = texts.find(unit.id);
        if (it == texts.end() || it->second.empty()) continue;
        json::Object o;
        o["id"] = json::Value(unit.id);
        o["type"] = json::Value(unit.type);
        o["text"] = json::Value(it->second);
        o["created_at"] = json::Value(unit.created_at);
        unit_arr.push_back(json::Value(std::move(o)));
    }
    std::string prompt = "## Existing scenes (" + std::to_string(scenes.size()) + ")\n" +
                         json::stringify(json::Value(std::move(scene_arr))) + "\n\n" +
                         "## New memories (" + std::to_string(units.size()) + ")\n" +
                         json::stringify(json::Value(std::move(unit_arr)));
    return prompt;
}

std::vector<SceneDecision> parse_scene_decisions(const std::string& raw) {
    std::vector<SceneDecision> out;
    const std::string text = strip_code_fences(raw);
    if (text.empty()) return out;
    json::Value parsed;
    try {
        parsed = json::parse(text);
    } catch (const std::exception&) {
        return out;  // not JSON: no change (the scenes are derived)
    }
    // Accept a bare array or a {"scenes": [...]} envelope.
    json::Value arr;
    if (parsed.type() == json::Value::Type::Array) {
        arr = parsed;
    } else if (parsed.type() == json::Value::Type::Object && parsed.has("scenes")) {
        arr = parsed.at("scenes");
    } else {
        return out;
    }
    if (arr.type() != json::Value::Type::Array) return out;
    for (const json::Value& item : arr.as_array()) {
        if (item.type() != json::Value::Type::Object) continue;
        SceneDecision d;
        d.action = item.at("action").as_string("");
        d.name = item.at("name").as_string("");
        d.summary = item.at("summary").as_string("");
        d.content = item.at("content").as_string("");
        d.heat = static_cast<int>(item.at("heat").as_number(1));
        if (item.has("retire") && item.at("retire").type() == json::Value::Type::Array) {
            for (const json::Value& r : item.at("retire").as_array()) {
                const std::string slug = r.as_string("");
                if (!slug.empty()) d.retire.push_back(slug);
            }
        }
        if (d.action != "create" && d.action != "update" && d.action != "merge") {
            continue;  // unknown action: drop the decision, not the refresh
        }
        if (!valid_scene_slug(d.name) || d.content.empty() ||
            d.content.size() > static_cast<std::size_t>(kSceneContentMax)) {
            continue;
        }
        if (d.summary.size() > 200) d.summary.resize(200);
        if (d.heat < 1) d.heat = 1;
        out.push_back(std::move(d));
    }
    return out;
}

SceneRefreshResult refresh_scenes(
    MemoryDb& db,
    const std::function<std::string(const std::string&)>& scenes_llm,
    const std::string& generation_id, std::string& error) {
    SceneRefreshResult result;
    const std::string watermark = db.max_scene_updated_at(error);
    if (!error.empty()) return result;
    const std::vector<MemoryDb::UnitRow> new_units =
        db.active_units_created_since(watermark, kSceneMaxNewUnits, error);
    if (!error.empty()) return result;
    result.new_units = static_cast<int>(new_units.size());
    if (result.new_units < kSceneMinNewUnits) return result;  // not ran

    std::vector<long long> version_ids;
    version_ids.reserve(new_units.size());
    for (const MemoryDb::UnitRow& unit : new_units) {
        if (unit.current_version_id > 0) version_ids.push_back(unit.current_version_id);
    }
    std::vector<MemoryDb::VersionRow> versions = db.versions_by_ids(version_ids, error);
    if (!error.empty()) return result;
    std::unordered_map<long long, std::string> texts;
    for (const MemoryDb::VersionRow& version : versions) {
        texts[version.id] = version.text;
    }
    for (auto it = texts.begin(); it != texts.end();) {
        if (it->second.empty()) it = texts.erase(it); else ++it;
    }
    if (texts.empty()) return result;  // nothing readable to distill
    for (const MemoryDb::UnitRow& unit : new_units) {
        if (texts.count(unit.id) > 0) result.unit_ids.push_back(unit.id);
    }

    const std::vector<MemoryDb::SceneRow> scenes = db.scenes(error);
    if (!error.empty()) return result;

    const std::string user_prompt =
        build_scenes_user_prompt(scenes, new_units, texts);
    result.ran = true;  // the LLM is about to be called (for the log row)
    std::string raw;
    try {
        raw = scenes_llm(user_prompt);
    } catch (const std::exception& e) {
        result.error = std::string("scene model failed: ") + e.what();
        return result;
    }
    if (raw.empty()) {
        result.error = "scene model returned no text";
        return result;
    }
    const std::vector<SceneDecision> decisions = parse_scene_decisions(raw);
    if (decisions.empty()) return result;  // the model says: no change

    // Apply with the cap enforced by the engine (the LLM owns merges; the
    // engine only refuses to grow past kSceneMaxScenes).
    std::set<std::string> names;
    std::map<std::string, int> heat_by_name;
    for (const MemoryDb::SceneRow& scene : scenes) {
        names.insert(scene.name);
        heat_by_name[scene.name] = scene.heat;
    }
    int applied = 0;
    for (const SceneDecision& d : decisions) {
        // A merge with nothing to absorb is an update; an update/create on
        // a taken/free name falls over to the other action (lenient: the
        // model's content must not be lost over a name collision).
        std::string action = d.action;
        if (action == "merge" && d.retire.empty()) action = "update";
        if (action == "create" && names.count(d.name) > 0) action = "update";
        if (action == "update" && names.count(d.name) == 0) action = "create";

        int heat = d.heat;
        if (action == "update") {
            const auto it = heat_by_name.find(d.name);
            heat = std::max(heat, (it == heat_by_name.end() ? 0 : it->second) + 1);
        } else if (action == "merge") {
            int absorbed = 0;
            for (const std::string& retired : d.retire) {
                const auto it = heat_by_name.find(retired);
                if (it != heat_by_name.end()) absorbed += it->second;
            }
            heat = absorbed + 1;
        }

        // Cap check: the set size AFTER this decision (create/merge add one
        // minus the retirees; update changes nothing).
        int after = static_cast<int>(names.size());
        if (action == "create" || action == "merge") after += 1;
        if (action == "merge") {
            int retired_in_set = 0;
            for (const std::string& retired : d.retire) {
                if (names.count(retired) > 0) retired_in_set++;
            }
            after -= retired_in_set;
        }
        if (after > kSceneMaxScenes) {
            result.dropped++;
            result.lines.push_back("scene " + d.name + " dropped (cap " +
                                   std::to_string(kSceneMaxScenes) + ")");
            continue;
        }

        if (!db.upsert_scene(d.name, d.summary, heat, d.content, generation_id, error)) {
            return result;
        }
        names.insert(d.name);
        heat_by_name[d.name] = heat;
        result.applied_names.push_back(d.name);
        applied++;
        if (action == "create") result.created++;
        if (action == "update") result.updated++;
        if (action == "merge") result.merged++;
        result.lines.push_back("scene " + d.name + " " + action + " (heat " +
                               std::to_string(heat) + ")");
        for (const std::string& retired : d.retire) {
            if (!db.delete_scene_by_name(retired, error)) return result;
            names.erase(retired);
            heat_by_name.erase(retired);
            result.lines.push_back("scene " + retired + " retired (merged into " +
                                   d.name + ")");
        }
    }
    result.changed = applied > 0;
    return result;
}

std::string build_persona_user_prompt(
    const std::vector<MemoryDb::SceneRow>& scenes,
    const std::string& existing_persona) {
    json::Array scene_arr;
    long long included = 0;
    for (const MemoryDb::SceneRow& scene : scenes) {
        if (included + scene.content.size() > kPersonaSceneBudget) break;
        included += scene.content.size();
        json::Object o;
        o["name"] = json::Value(scene.name);
        o["summary"] = json::Value(scene.summary);
        o["heat"] = json::Value(static_cast<long long>(scene.heat));
        o["content"] = json::Value(scene.content);
        scene_arr.push_back(json::Value(std::move(o)));
    }
    std::string prompt = "## Scene blocks (" + std::to_string(scenes.size()) + ")\n" +
                         json::stringify(json::Value(std::move(scene_arr))) + "\n\n" +
                         "## Current persona\n" +
                         (existing_persona.empty() ? std::string("(none)") : existing_persona);
    return prompt;
}

std::string parse_persona(const std::string& raw) {
    const std::string text = strip_code_fences(raw);
    if (text.empty()) return "";
    json::Value parsed;
    try {
        parsed = json::parse(text);
    } catch (const std::exception&) {
        return "";
    }
    if (parsed.type() != json::Value::Type::Object || !parsed.has("persona")) {
        return "";
    }
    std::string persona = parsed.at("persona").as_string("");
    while (!persona.empty() && (persona.front() == ' ' || persona.front() == '\n')) {
        persona.erase(persona.begin());
    }
    while (!persona.empty() && (persona.back() == ' ' || persona.back() == '\n')) {
        persona.pop_back();
    }
    if (persona.empty() || persona.size() > static_cast<std::size_t>(kPersonaMaxChars)) {
        return "";
    }
    return persona;
}

PersonaRefreshResult refresh_persona(
    MemoryDb& db,
    const std::function<std::string(const std::string&)>& persona_llm,
    const std::string& generation_id, std::string& error) {
    PersonaRefreshResult result;
    const std::vector<MemoryDb::SceneRow> scenes = db.scenes(error);
    if (!error.empty()) return result;
    if (scenes.empty()) return result;  // not ran: no scenes to distill from
    // The scenes the prompt will carry (heat-first, content budgeted) - the
    // same selection build_persona_user_prompt makes, for the provenance log.
    long long included = 0;
    for (const MemoryDb::SceneRow& scene : scenes) {
        if (included + scene.content.size() > kPersonaSceneBudget) break;
        included += scene.content.size();
        result.scene_names.push_back(scene.name);
    }
    std::string existing;
    const std::optional<MemoryDb::PersonaRow> current = db.persona(error);
    if (!error.empty()) return result;
    if (current) existing = current->content;

    const std::string user_prompt = build_persona_user_prompt(scenes, existing);
    result.ran = true;  // the LLM is about to be called (for the log row)
    std::string raw;
    try {
        raw = persona_llm(user_prompt);
    } catch (const std::exception& e) {
        result.error = std::string("persona model failed: ") + e.what();
        return result;
    }
    if (raw.empty()) {
        result.error = "persona model returned no text";
        return result;
    }
    const std::string persona = parse_persona(raw);
    if (persona.empty()) {
        result.error = "persona output unparseable or over the cap";
        return result;
    }
    if (!db.upsert_persona(persona, generation_id, error)) return result;
    result.changed = persona != existing;
    return result;
}

// ── Status ───────────────────────────────────────────────────────────────

json::Object status(MemoryDb& db, const qwen3::Model& embed_model,
                    const qwen3::Model& rerank_model) {
    std::string error;
    json::Object body;
    json::Object units_obj;
    for (const auto& [state, count] : db.unit_state_counts(error)) {
        units_obj[state] = json::Value(static_cast<long long>(count));
    }
    body["units"] = json::Value(std::move(units_obj));
    body["versions"] = json::Value(static_cast<long long>(db.version_count(error)));
    body["embeddings"] = json::Value(static_cast<long long>(db.embedding_count(error)));
    body["fts_rows"] = json::Value(static_cast<long long>(db.fts_row_count(error)));
    body["links"] = json::Value(static_cast<long long>(db.link_count(error)));
    body["feedback"] = json::Value(static_cast<long long>(db.feedback_count(error)));
    body["sweeps"] = json::Value(static_cast<long long>(db.sweep_count(error)));
    const auto last = db.last_sweep(error);
    if (last) {
        json::Object sweep;
        sweep["trigger"] = json::Value(last->trigger);
        sweep["watermark"] = json::Value(last->watermark);
        sweep["selected"] = json::Value(static_cast<long long>(last->selected));
        sweep["changes"] = json::Value(static_cast<long long>(last->changes));
        sweep["fixpoint"] = json::Value(last->fixpoint);
        sweep["finished_at"] = json::Value(last->finished_at);
        body["last_sweep"] = json::Value(std::move(sweep));
    }
    json::Object engine;
    engine["embed_loaded"] = json::Value(embed_model.ok);
    engine["embed_hidden"] = json::Value(static_cast<long long>(embed_model.hidden));
    engine["rerank_loaded"] = json::Value(rerank_model.ok);
    body["engine"] = json::Value(std::move(engine));
    return body;
}

}  // namespace custom
}  // namespace memory
}  // namespace nyaxus
