#include "nyaxus/json.hpp"
#include "nyaxus/memory.hpp"
#include "nyaxus/store.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

// A real function (not a macro) so braced-init-lists with commas are handled
// by the compiler, not the preprocessor.
[[noreturn]] void check_fail(const char* what) {
    std::fprintf(stderr, "memory_lifecycle FAIL: %s\n", what);
    std::abort();
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* what) {
    if (!(a == b)) check_fail(what);
}

void check(bool condition, const char* what) {
    if (!condition) check_fail(what);
}

nyaxus::StoredMessage make_message(const std::string& sid, long long parent,
                                   const std::string& role, const std::string& run_id) {
    nyaxus::StoredMessage message;
    message.parent_id = parent;
    message.session_id = sid;
    message.role = role;
    message.content = "content of " + run_id;
    message.run_id = run_id;
    message.created_at = "2026-01-01T00:00:00.000Z";
    message.is_active = true;
    return message;
}

std::vector<std::string> sorted(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    return values;
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "nyaxus-memory-lifecycle";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        // ── suppression store semantics ────────────────────────────────────
        nyaxus::SqliteStore store(root / "suppression.db");

        const long long first = store.suppress_memory_key("run-1", "regenerated");
        check(first > 0, "first suppression id");
        // Idempotent: the same key returns the same id and adds no second row.
        const long long dup = store.suppress_memory_key("run-1", "regenerated");
        check_eq(dup, first, "duplicate suppression id");
        const long long second = store.suppress_memory_key("session-abc", "session_deleted");
        check(second > first, "second suppression id");

        auto keys = store.memory_suppression_keys();
        check_eq(keys.size(), static_cast<std::size_t>(2), "suppression key count");
        std::sort(keys.begin(), keys.end());
        check_eq(keys[0], std::string("run-1"), "suppression key 0");
        check_eq(keys[1], std::string("session-abc"), "suppression key 1");

        auto counts = store.memory_suppression_counts();
        check_eq(counts.at("regenerated"), 1LL, "regenerated count");
        check_eq(counts.at("session_deleted"), 1LL, "session_deleted count");
    }

    {
        // ── recall_hit_suppressed matching ─────────────────────────────────
        using nyaxus::memory::recall_hit_suppressed;
        const std::set<std::string> suppressed = {"run-1", "session-abc"};

        // Bare run id recognized from the document id (nyaxus-turn:<run>).
        check(recall_hit_suppressed("nyaxus-turn:run-1", {}, suppressed),
              "suppressed by document id");
        // Bare run id recognized from a tag.
        check(recall_hit_suppressed("", {"nyaxus.run:run-1"}, suppressed),
              "suppressed by run tag");
        // Bare session id recognized from a tag.
        check(recall_hit_suppressed("", {"nyaxus.session:session-abc"}, suppressed),
              "suppressed by session tag");
        // An exact document id stored as a key also matches.
        check(recall_hit_suppressed("nyaxus-turn:run-9", {}, {"nyaxus-turn:run-9"}),
              "suppressed by exact document key");
        // A hit whose ids are not suppressed passes through.
        check(!recall_hit_suppressed("nyaxus-turn:run-2", {"nyaxus.session:other"},
                                     suppressed),
              "unsuppressed hit passes");
        // An empty suppression set never matches.
        check(!recall_hit_suppressed("nyaxus-turn:run-1", {}, {}),
              "empty suppression set");
    }

    {
        // ── run-id collection over a message tree ──────────────────────────
        nyaxus::SqliteStore store(root / "tree.db");
        nyaxus::StoredSession session;
        session.id = "s1";
        store.upsert_session(session);

        // Tree:
        //   1 (user, run-A)
        //     2 (assistant, run-A)
        //       3 (user, run-B)
        //         4 (assistant, run-B)
        //       5 (assistant, run-C)   <- branch under 2
        const long long m1 = store.append_message(make_message("s1", 0, "user", "run-A"));
        const long long m2 = store.append_message(make_message("s1", m1, "assistant", "run-A"));
        const long long m3 = store.append_message(make_message("s1", m2, "user", "run-B"));
        const long long m4 = store.append_message(make_message("s1", m3, "assistant", "run-B"));
        const long long m5 = store.append_message(make_message("s1", m2, "assistant", "run-C"));
        check(m1 > 0 && m2 > m1 && m5 > m4, "message ids");

        // Descendants of 2 = {3,4,5} -> run-B, run-C.
        check_eq(sorted(store.run_ids_of_descendants("s1", m2)),
                 std::vector<std::string>{"run-B", "run-C"}, "descendants of 2");
        // Descendants of a leaf = none.
        check(store.run_ids_of_descendants("s1", m4).empty(), "descendants of leaf");
        // Descendants of the root = every run in the tree.
        check_eq(sorted(store.run_ids_of_descendants("s1", m1)),
                 std::vector<std::string>{"run-A", "run-B", "run-C"}, "descendants of root");

        // Active path to 4 = {1,2,3,4}: on-path run-A, run-B; off-path run-C.
        store.set_active_leaf("s1", m4);
        check_eq(sorted(store.run_ids_off_active_path("s1", m4)),
                 std::vector<std::string>{"run-C"}, "off-path at 4");
        // Active path to 5 = {1,2,5}: on-path run-A, run-C; off-path run-B.
        store.set_active_leaf("s1", m5);
        check_eq(sorted(store.run_ids_off_active_path("s1", m5)),
                 std::vector<std::string>{"run-B"}, "off-path at 5");
        // No active leaf -> every run is off-path.
        store.set_active_leaf("s1", 0);
        check_eq(sorted(store.run_ids_off_active_path("s1", 0)),
                 std::vector<std::string>{"run-A", "run-B", "run-C"}, "off-path at 0");
    }

    {
        // ── memory record queue semantics ──────────────────────────────────
        nyaxus::SqliteStore store(root / "records.db");

        nyaxus::MemoryRecord candidate;
        candidate.text = "the user prefers tabs over spaces";
        candidate.type = "preference";
        candidate.scope = "session";
        candidate.session_id = "s1";
        candidate.provenance = "{\"model\":\"qwen3-4b\",\"revision\":\"local-1\"}";
        const long long id = store.add_memory_record(candidate);
        check(id > 0, "record id");

        const auto fetched = store.get_memory_record(id);
        check(fetched.has_value(), "record fetched");
        check_eq(fetched->status, std::string("candidate"), "record status");
        check_eq(fetched->text, std::string("the user prefers tabs over spaces"),
                 "record text");
        check(fetched->provenance.find("qwen3-4b") != std::string::npos,
              "record provenance");
        check(!fetched->created_at.empty(), "record created_at");

        // List all, then filter by status.
        check_eq(store.list_memory_records("").size(), static_cast<std::size_t>(1),
                 "list all");
        check_eq(store.list_memory_records("candidate").size(),
                 static_cast<std::size_t>(1), "list candidate");
        check(store.list_memory_records("approved").empty(), "list approved empty");

        // Approve: status advances and the decision time is stamped.
        check(store.set_memory_record_status(id, "approved"), "approve");
        const auto approved = store.get_memory_record(id);
        check_eq(approved->status, std::string("approved"), "approved status");
        check(!approved->decided_at.empty(), "decided_at stamped");
        check_eq(store.list_memory_records("approved").size(),
                 static_cast<std::size_t>(1), "list approved");

        // A second record can be rejected independently.
        nyaxus::MemoryRecord other;
        other.text = "irrelevant note";
        const long long other_id = store.add_memory_record(other);
        check(other_id > id, "second record id");
        check(store.set_memory_record_status(other_id, "rejected"), "reject");

        auto counts = store.memory_record_counts();
        check_eq(counts.at("approved"), 1LL, "approved count");
        check_eq(counts.at("rejected"), 1LL, "rejected count");

        // An unknown id does not transition.
        check(!store.set_memory_record_status(99999, "approved"), "unknown id");
    }

    std::filesystem::remove_all(root);
    std::fputs("memory_lifecycle: all checks passed\n", stderr);
    return 0;
}
