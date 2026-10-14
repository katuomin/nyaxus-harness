#include "nyaxus/memory.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

#include "nyaxus/http.hpp"
#include "nyaxus/json.hpp"
#include "nyaxus/log.hpp"

namespace nyaxus {
namespace memory {
namespace {

std::string percent_encode(const std::string& input) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : input) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string bank_url(const HindsightConfig& config) {
    return config.endpoint + "/v1/default/banks/" + percent_encode(config.bank_id);
}

std::map<std::string, std::string> request_headers(const HindsightConfig& config) {
    std::map<std::string, std::string> headers;
    headers["content-type"] = "application/json";
    if (!config.api_key.empty()) headers["authorization"] = "Bearer " + config.api_key;
    return headers;
}

bool is_2xx(int status) { return status >= 200 && status < 300; }

// Fixed-width ISO-UTC (same shape as the store's now_utc) so the outbox's
// lexicographic due-date comparison stays correct.
std::string timestamp_utc(std::chrono::system_clock::time_point when) {
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()) %
        1000;
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

}  // namespace

bool ensure_bank(const HindsightConfig& config, std::string& error) {
    std::string body;
    int status = 0;
    // PUT is Hindsight's create-or-update bank operation (GET is 405 on
    // older builds), so it is safe to repeat on every pass.
    if (!http::request_body("PUT", bank_url(config), "{}", request_headers(config),
                            body, config.timeout_ms, &status)) {
        error = "could not reach Hindsight at " + config.endpoint;
        return false;
    }
    if (!is_2xx(status)) {
        error = "bank create returned HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

bool retain(const HindsightConfig& config, const RetainTurn& turn,
            std::string& operation_id, std::string& error) {
    const auto started = std::chrono::steady_clock::now();
    auto elapsed_ms = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started)
            .count();
    };
    json::Object item;
    item["content"] = json::Value(turn.content);
    item["context"] = json::Value(turn.context);
    item["document_id"] = json::Value(turn.document_id);
    json::Array tags;
    for (const std::string& tag : turn.tags) tags.push_back(json::Value(tag));
    item["tags"] = json::Value(std::move(tags));
    json::Object body;
    json::Array items;
    items.push_back(json::Value(std::move(item)));
    body["items"] = json::Value(std::move(items));
    body["async"] = json::Value(false);

    std::string response;
    int status = 0;
    if (!http::post_body(bank_url(config) + "/memories", json::stringify(body),
                         request_headers(config), response, config.timeout_ms, &status)) {
        error = "retain request to " + config.endpoint + " failed after " +
                std::to_string(elapsed_ms()) + "ms";
        return false;
    }
    if (!is_2xx(status)) {
        error = "retain returned HTTP " + std::to_string(status);
        if (response.size() <= 300) error += " " + response;
        return false;
    }
    const json::Value parsed = json::parse(response);
    if (parsed.has("operation_id")) operation_id = parsed.at("operation_id").as_string("");
    log::info("memory: retain took " + std::to_string(elapsed_ms()) + "ms");
    return true;
}

bool healthy(const HindsightConfig& config, std::string& error) {
    std::string body;
    if (!http::get_body(config.endpoint + "/version", request_headers(config), body,
                        config.timeout_ms)) {
        error = "could not reach Hindsight at " + config.endpoint;
        return false;
    }
    return true;
}

bool recall(const HindsightConfig& config, const RecallRequest& request,
            std::vector<RecallHit>& hits, std::string& error) {
    json::Object body;
    body["query"] = json::Value(request.query);
    body["budget"] = json::Value(request.budget);
    if (request.max_tokens > 0) body["max_tokens"] = json::Value(request.max_tokens);
    if (!request.tags.empty()) {
        json::Array tags;
        for (const std::string& tag : request.tags) tags.push_back(json::Value(tag));
        body["tags"] = json::Value(std::move(tags));
        body["tags_match"] = json::Value("all_strict");
    }

    std::string response;
    int status = 0;
    if (!http::post_body(bank_url(config) + "/memories/recall",
                         json::stringify(body), request_headers(config), response,
                         config.timeout_ms, &status)) {
        error = "recall request to " + config.endpoint + " failed";
        return false;
    }
    if (!is_2xx(status)) {
        error = "recall returned HTTP " + std::to_string(status);
        if (response.size() <= 300) error += " " + response;
        return false;
    }
    const json::Value parsed = json::parse(response);
    if (parsed.has("results")) {
        for (const json::Value& item : parsed.at("results").as_array()) {
            RecallHit hit;
            hit.id = item.at("id").as_string("");
            hit.text = item.at("text").as_string("");
            hit.type = item.at("type").as_string("");
            hit.document_id = item.at("document_id").as_string("");
            if (item.has("tags")) {
                for (const json::Value& tag : item.at("tags").as_array()) {
                    const std::string text = tag.as_string("");
                    if (!text.empty()) hit.tags.push_back(text);
                }
            }
            if (!hit.text.empty()) hits.push_back(std::move(hit));
        }
    }
    return true;
}

namespace {
// Extract the bare id from a Nyaxus document id or tag, if present.
std::string nyaxus_bare_id(const std::string& value) {
    static const char* prefixes[] = {
        "nyaxus-turn:", "nyaxus.run:", "nyaxus.session:", "nyaxus.profile:",
        "nyaxus.branch:", "nyaxus.source:",
    };
    for (const char* prefix : prefixes) {
        const std::size_t len = std::strlen(prefix);
        if (value.size() > len && value.compare(0, len, prefix) == 0) {
            return value.substr(len);
        }
    }
    return "";
}
}  // namespace

bool list_memories(const HindsightConfig& config, std::vector<MemoryUnit>& units,
                   std::string& error) {
    const int page = 100;
    int offset = 0;
    int total = -1;
    while (true) {
        const std::string url = bank_url(config) + "/memories/list?limit=" +
                                std::to_string(page) + "&offset=" + std::to_string(offset);
        std::string response;
        if (!http::get_body(url, request_headers(config), response, config.timeout_ms)) {
            error = "memories/list request to " + config.endpoint + " failed";
            return false;
        }
        json::Value parsed;
        try {
            parsed = json::parse(response);
        } catch (...) {
            error = "memories/list returned a non-JSON response";
            return false;
        }
        if (!parsed.has("items")) {
            error = "memories/list returned no items (" +
                    response.substr(0, response.size() <= 200 ? response.size() : 200) + ")";
            return false;
        }
        if (total < 0) {
            // A missing or non-numeric "total" must not stop pagination after
            // the first page; keep going until the server returns an empty page.
            const int t = parsed.has("total") ? parsed.at("total").as_integer(0) : 0;
            total = t > 0 ? t : -1;
        }
        const json::Array& items = parsed.at("items").as_array();
        for (const json::Value& item : items) {
            MemoryUnit unit;
            unit.id = item.at("id").as_string("");
            unit.text = item.at("text").as_string("");
            unit.context = item.at("context").as_string("");
            unit.fact_type = item.at("fact_type").as_string("");
            unit.document_id = item.at("document_id").as_string("");
            unit.state = item.at("state").as_string("");
            if (item.has("metadata")) {
                unit.session_id = item.at("metadata").at("session_id").as_string("");
            }
            if (item.has("tags")) {
                for (const json::Value& tag : item.at("tags").as_array()) {
                    const std::string t = tag.as_string("");
                    if (!t.empty()) unit.tags.push_back(t);
                }
            }
            if (!unit.text.empty()) units.push_back(std::move(unit));
        }
        offset += static_cast<int>(items.size());
        if (items.empty() || (total > 0 && offset >= total)) break;
    }
    return true;
}

bool recall_hit_suppressed(const std::string& document_id,
                           const std::vector<std::string>& tags,
                           const std::set<std::string>& suppressed) {
    if (suppressed.empty()) return false;
    if (!document_id.empty()) {
        if (suppressed.count(document_id)) return true;
        const std::string bare = nyaxus_bare_id(document_id);
        if (!bare.empty() && suppressed.count(bare)) return true;
    }
    for (const std::string& tag : tags) {
        if (suppressed.count(tag)) return true;
        const std::string bare = nyaxus_bare_id(tag);
        if (!bare.empty() && suppressed.count(bare)) return true;
    }
    return false;
}

int process_due_jobs(SqliteStore& store, const HindsightConfig& config, int max_jobs) {
    // Parse and validate payloads first: a malformed job fails permanently
    // without touching the network, and only valid work reaches the provider.
    std::vector<MemoryOutboxJob> jobs;
    std::vector<RetainTurn> turns;
    for (const MemoryOutboxJob& job : store.due_memory_jobs(max_jobs)) {
        if (job.provider != "hindsight" || job.operation != "retain") continue;
        const json::Value payload = json::parse(job.payload);
        RetainTurn turn;
        turn.content = payload.at("content").as_string("");
        turn.context = payload.at("context").as_string("");
        turn.document_id = payload.at("document_id").as_string("");
        if (payload.has("tags")) {
            for (const json::Value& tag : payload.at("tags").as_array()) {
                const std::string text = tag.as_string("");
                if (!text.empty()) turn.tags.push_back(text);
            }
        }
        if (turn.content.empty() || turn.document_id.empty()) {
            store.fail_memory_job(job.id, "malformed payload");
            log::error("memory: job " + std::to_string(job.id) +
                       " has a malformed payload");
            continue;
        }
        jobs.push_back(job);
        turns.push_back(std::move(turn));
    }
    if (jobs.empty()) return 0;

    auto defer_failure = [&](const MemoryOutboxJob& job, const std::string& error) {
        if (job.retry_count + 1 >= config.max_retries) {
            store.fail_memory_job(job.id, error);
            log::error("memory: job " + std::to_string(job.id) +
                       " permanently failed: " + error);
            return;
        }
        const long long backoff_ms =
            std::min(30000LL * (1LL << job.retry_count), 300000LL);
        const std::string next = timestamp_utc(
            std::chrono::system_clock::now() + std::chrono::milliseconds(backoff_ms));
        store.retry_memory_job(job.id, error, next);
        log::warn("memory: job " + std::to_string(job.id) + " failed (" + error +
                  "); retry " + std::to_string(job.retry_count + 1) + " in " +
                  std::to_string(backoff_ms / 1000) + "s");
    };

    std::string error;
    if (!ensure_bank(config, error)) {
        for (const MemoryOutboxJob& job : jobs) defer_failure(job, error);
        return 0;
    }

    int completed = 0;
    for (std::size_t i = 0; i < jobs.size(); i++) {
        const MemoryOutboxJob& job = jobs[i];
        store.mark_memory_job_sending(job.id);
        std::string operation_id;
        if (retain(config, turns[i], operation_id, error)) {
            store.mark_memory_job_completed(job.id, operation_id);
            log::info("memory: retained " + turns[i].document_id + " (job " +
                      std::to_string(job.id) + ")");
            completed++;
        } else {
            defer_failure(job, error);
        }
    }
    return completed;
}

}  // namespace memory
}  // namespace nyaxus
