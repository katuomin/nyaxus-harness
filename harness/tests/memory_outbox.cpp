#include "nyaxus/json.hpp"
#include "nyaxus/memory.hpp"
#include "nyaxus/store.hpp"

#include <cassert>
#include <filesystem>
#include <string>

namespace {

nyaxus::json::Object retain_payload() {
    nyaxus::json::Object payload;
    payload["content"] = nyaxus::json::Value("User: hello\n\nAssistant: hi");
    payload["context"] = nyaxus::json::Value("Nyaxus session s1: completed turn");
    payload["document_id"] = nyaxus::json::Value("nyaxus-turn:run-1");
    nyaxus::json::Array tags;
    tags.push_back(nyaxus::json::Value("nyaxus.session:s1"));
    payload["tags"] = nyaxus::json::Value(std::move(tags));
    return payload;
}

void expect_job(const nyaxus::SqliteStore& store, long long id,
                const std::string& status, int retry_count,
                bool next_attempt_set, const std::string& error_prefix = "") {
    const auto job = store.get_memory_job(id);
    assert(job.has_value());
    assert(job->status == status);
    assert(job->retry_count == retry_count);
    assert((!job->next_attempt_at.empty()) == next_attempt_set);
    if (!error_prefix.empty()) {
        assert(job->last_error.rfind(error_prefix, 0) == 0);
    }
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "nyaxus-memory-outbox";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        // ── store semantics ────────────────────────────────────────────────
        nyaxus::SqliteStore store(root / "store.db");

        const long long first = store.enqueue_memory_job(
            "retain:run-1", "hindsight", "retain",
            nyaxus::json::stringify(retain_payload()));
        assert(first > 0);
        // Duplicate idempotency key returns the existing job, no second row.
        const long long dup = store.enqueue_memory_job(
            "retain:run-1", "hindsight", "retain",
            nyaxus::json::stringify(retain_payload()));
        assert(dup == first);
        const long long other = store.enqueue_memory_job(
            "retain:run-2", "hindsight", "retain",
            nyaxus::json::stringify(retain_payload()));
        assert(other > first);

        auto due = store.due_memory_jobs(10);
        assert(due.size() == 2);
        assert(due.front().id == first);
        assert(due.front().status == "pending");
        assert(due.front().retry_count == 0);
        assert(due.front().next_attempt_at.empty());
        assert(due.front().payload.find("nyaxus-turn:run-1") != std::string::npos);

        // Retryable failure: counter advances, due date is deferred.
        store.retry_memory_job(first, "boom", "2099-01-01T00:00:00.000Z");
        due = store.due_memory_jobs(10);
        assert(due.size() == 1 && due.front().id == other);
        auto counts = store.memory_job_counts();
        assert(counts.at("pending") == 2);

        // Permanent failure is terminal and counted.
        store.fail_memory_job(other, "gave up");
        counts = store.memory_job_counts();
        assert(counts.at("pending") == 1);
        assert(counts.at("failed") == 1);

        // Completion records the provider operation id.
        store.mark_memory_job_completed(first, "op-42");
        counts = store.memory_job_counts();
        assert(counts.at("completed") == 1);
        due = store.due_memory_jobs(10);
        assert(due.empty());
    }

    {
        // ── process_due_jobs against a dead provider ──────────────────────
        nyaxus::SqliteStore store(root / "dead.db");
        nyaxus::memory::HindsightConfig config;
        config.endpoint = "http://127.0.0.1:1";
        config.bank_id = "outbox-test";
        config.timeout_ms = 1500;

        // max_retries = 1: the first failure is already the last attempt.
        config.max_retries = 1;
        store.enqueue_memory_job("retain:dead-1", "hindsight", "retain",
                                 nyaxus::json::stringify(retain_payload()));
        const int completed = nyaxus::memory::process_due_jobs(store, config);
        assert(completed == 0);
        auto counts = store.memory_job_counts();
        assert(counts.at("failed") == 1);
        expect_job(store, 1, "failed", 0, false);

        // max_retries = 3: the first failure defers with backoff instead of
        // failing permanently, and the job is no longer due.
        config.max_retries = 3;
        const long long id2 = store.enqueue_memory_job(
            "retain:dead-2", "hindsight", "retain",
            nyaxus::json::stringify(retain_payload()));
        const int completed2 = nyaxus::memory::process_due_jobs(store, config);
        assert(completed2 == 0);
        counts = store.memory_job_counts();
        assert(counts.at("pending") == 1);
        assert(counts.at("failed") == 2);
        expect_job(store, id2, "pending", 1, true, "could not reach Hindsight");
        // Deferred: not due again until the backoff elapses.
        assert(store.due_memory_jobs(10).empty());

        // A malformed payload fails permanently without touching the network.
        nyaxus::json::Object bad;
        bad["content"] = nyaxus::json::Value("");
        bad["document_id"] = nyaxus::json::Value("");
        store.enqueue_memory_job("retain:bad-1", "hindsight", "retain",
                                 nyaxus::json::stringify(bad));
        nyaxus::memory::HindsightConfig unreachable;
        unreachable.endpoint = "http://127.0.0.1:1";
        unreachable.bank_id = "outbox-test";
        unreachable.timeout_ms = 1500;
        unreachable.max_retries = 5;
        nyaxus::memory::process_due_jobs(store, unreachable);
        counts = store.memory_job_counts();
        assert(counts.at("failed") == 3);
    }

    std::filesystem::remove_all(root);
    return 0;
}
