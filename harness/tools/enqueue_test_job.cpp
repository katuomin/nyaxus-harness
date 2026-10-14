// Test helper: enqueue one custom-provider retain job (direct text path)
// into the daemon's outbox, so the worker's full path can be exercised
// without a completed turn. Usage: enqueue_test_job <nyaxus.db>
#include <cstdio>

#include "nyaxus/json.hpp"
#include "nyaxus/store.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: enqueue_test_job <nyaxus.db>\n");
        return 2;
    }
    nyaxus::SqliteStore store(argv[1]);
    nyaxus::json::Object payload;
    payload["direct_text"] = nyaxus::json::Value(
        "The e2e smoke test stored a direct memory via the custom provider.");
    payload["direct_type"] = nyaxus::json::Value("world");
    payload["document_id"] = nyaxus::json::Value("nyaxus-turn:run-e2e-1");
    payload["session_id"] = nyaxus::json::Value("sess-e2e");
    payload["run_id"] = nyaxus::json::Value("run-e2e-1");
    payload["model_id"] = nyaxus::json::Value("direct");
    const long long id = store.enqueue_memory_job(
        "retain:run-e2e-1", "custom", "retain",
        nyaxus::json::stringify(payload));
    std::printf("enqueued job %lld\n", id);
    return 0;
}
