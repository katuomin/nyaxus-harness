#include "nyaxus/core.hpp"
#include "nyaxus/store.hpp"

#include <cassert>
#include <filesystem>
#include <string>

int main() {
    const auto root = std::filesystem::temp_directory_path() / "nyaxus-core-smoke";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    nyaxus::ProfileDefaults defaults;
    defaults.identity_prompt = "default identity";
    defaults.skills = {"research"};
    defaults.tools = {"terminal", "file"};
    defaults.parameters["temperature"] = "0.7";

    nyaxus::ProfileDefinition definition;
    definition.id = "researcher";
    definition.revision = "r3";
    definition.identity_prompt = "";
    definition.skills = std::vector<std::string>{};
    definition.tools = std::vector<std::string>{"file"};
    definition.parameters["temperature"] = "0.2";

    const auto resolved = nyaxus::resolve_profile(defaults, definition);
    assert(resolved.identity_prompt.empty());
    assert(resolved.skills.empty());
    assert(resolved.tools.size() == 1 && resolved.tools.front() == "file");
    assert(resolved.parameters.at("temperature") == "0.2");
    assert(resolved.snapshot_id.rfind("r3-", 0) == 0);
    assert(nyaxus::strip_inline_thinking(
               "before<think>hidden</think>after<think>more</think>end") ==
           "beforeafterend");
    assert(nyaxus::strip_inline_thinking("visible<think>unfinished") == "visible");
    assert(nyaxus::extract_inline_thinking(
               "before<think>hidden</think>after<think>more</think>end") ==
           "hidden\nmore");
    assert(nyaxus::extract_inline_thinking("visible<think>unfinished") == "unfinished");

    nyaxus::ProfileDefinition inherited;
    inherited.id = "inherited";
    const auto inherited_profile = nyaxus::resolve_profile(defaults, inherited);
    assert(inherited_profile.identity_prompt == "default identity");
    assert(inherited_profile.skills.size() == 1);
    assert(inherited_profile.tools.size() == 2);

    assert(nyaxus::can_transition(nyaxus::RunStatus::Accepted,
                                  nyaxus::RunStatus::Queued));
    assert(nyaxus::can_transition(nyaxus::RunStatus::Running,
                                  nyaxus::RunStatus::WaitingApproval));
    assert(!nyaxus::can_transition(nyaxus::RunStatus::Completed,
                                   nyaxus::RunStatus::Running));

    const auto store_path = root / "events.db";
    {
        nyaxus::SqliteStore store(store_path);
        const auto first = store.append_event(
            "session", "run", "", "run.accepted", {{"x", "1"}, {"text", "a } b"}});
        const auto second = store.append_event("session", "run", "", "run.started");
        assert(first.sequence == 1);
        assert(second.sequence == 2);
        assert(first.event_id == "run:1");
        assert(store.replay_events("run", 1).size() == 1);

        store.upsert_session({.id = "session", .title = "first"});
        nyaxus::StoredMessage m1;
        m1.session_id = "session";
        m1.role = "user";
        m1.content = "hello";
        m1.run_id = "run";
        m1.created_at = "t1";
        const auto user_id = store.append_message(m1);
        assert(user_id > 0);
    }
    {
        nyaxus::SqliteStore reloaded(store_path);
        const auto next = reloaded.append_event("session", "run", "", "run.completed");
        assert(next.sequence == 3);
        assert(next.row_id == 3);
        assert(reloaded.replay_events("run").size() == 3);
        assert(reloaded.replay_session_events("session").size() == 3);
        // Cursor-based replay: only ledger rows after the cursor are returned.
        assert(reloaded.replay_session_events_after("session", 1).size() == 2);
        assert(reloaded.replay_session_events_after("session", 1).front().event_id == "run:2");
        assert(reloaded.replay_session_events_after("session", 3).empty());
        assert(reloaded.replay_events("run").front().payload.at("text") == "a } b");
        assert(reloaded.session_messages("session").size() == 1);
        assert(reloaded.session_messages("session").front().content == "hello");
        const auto stored = reloaded.get_session("session");
        assert(stored.has_value() && stored->title == "first");
        assert(stored->context_mode == "selected");
        assert(stored->context_status == "ready");
        assert(stored->context_summary.empty());
        assert(stored->context_error.empty());
        nyaxus::StoredSession context = *stored;
        context.context_mode = "summary";
        context.context_status = "failed";
        context.context_summary = "not used";
        context.context_error = "provider unavailable";
        context.source_session_id = "source";
        reloaded.upsert_session(context);
        const auto persisted_context = reloaded.get_session("session");
        assert(persisted_context.has_value());
        assert(persisted_context->context_mode == "summary");
        assert(persisted_context->context_status == "failed");
        assert(persisted_context->context_summary == "not used");
        assert(persisted_context->context_error == "provider unavailable");
        assert(persisted_context->source_session_id == "source");

        // Branching: an assistant child becomes the active leaf, the sibling does not.
        const auto user_id = reloaded.session_messages("session").front().id;
        nyaxus::StoredMessage a1;
        a1.parent_id = user_id;
        a1.session_id = "session";
        a1.role = "assistant";
        a1.content = "A";
        a1.run_id = "run-a1";
        a1.created_at = "t2";
        const auto a1_id = reloaded.append_message(a1);
        reloaded.set_active_leaf("session", a1_id);

        nyaxus::StoredMessage a2;
        a2.parent_id = user_id;
        a2.session_id = "session";
        a2.role = "assistant";
        a2.content = "B";
        a2.run_id = "run-a2";
        a2.created_at = "t3";
        const auto a2_id = reloaded.append_message(a2);
        reloaded.set_active_leaf("session", a2_id);
        assert(reloaded.active_leaf_id("session") == a2_id);
        assert(reloaded.active_messages("session").size() == 2);

        reloaded.set_active_leaf("session", a1_id);
        assert(reloaded.active_leaf_id("session") == a1_id);
        assert(reloaded.active_messages("session").size() == 2);
        assert(reloaded.active_messages("session").front().content == "hello");
        assert(reloaded.active_messages("session").back().content == "A");

        // Selecting an iteration re-enters its latest continuation rather than
        // hiding descendants that were created on that branch.
        nyaxus::StoredMessage follow_up;
        follow_up.parent_id = a2_id;
        follow_up.session_id = "session";
        follow_up.role = "user";
        follow_up.content = "continue B";
        follow_up.run_id = "run-follow-up";
        follow_up.created_at = "t4";
        const auto follow_up_id = reloaded.append_message(follow_up);
        nyaxus::StoredMessage follow_up_answer;
        follow_up_answer.parent_id = follow_up_id;
        follow_up_answer.session_id = "session";
        follow_up_answer.role = "assistant";
        follow_up_answer.content = "B continuation";
        follow_up_answer.run_id = "run-follow-up-answer";
        follow_up_answer.created_at = "t5";
        const auto follow_up_answer_id = reloaded.append_message(follow_up_answer);

        reloaded.select_iteration("session", "run-a2");
        assert(reloaded.active_leaf_id("session") == follow_up_answer_id);
        assert(reloaded.active_messages("session").back().content == "B continuation");

        // Store primitives behind the edit/rewind endpoints: a message's
        // content can be rewritten in place, and the active path can be
        // truncated at any message while the rest stays re-enterable.
        reloaded.update_message_content(user_id, "hello (edited)");
        const auto edited = reloaded.get_message(user_id);
        assert(edited.has_value() && edited->content == "hello (edited)");

        // Rewind: the active leaf moves to an earlier message; the tail stays
        // in the tree as an inactive branch and can be re-entered.
        reloaded.set_active_leaf("session", a1_id);
        assert(reloaded.active_leaf_id("session") == a1_id);
        assert(reloaded.active_messages("session").size() == 2);
        assert(reloaded.active_messages("session").back().content == "A");
        const auto tail = reloaded.get_message(follow_up_id);
        assert(tail.has_value() && !tail->is_active);
        assert(reloaded.get_message(follow_up_answer_id)->is_active == false);
        reloaded.select_iteration("session", "run-follow-up-answer");
        assert(reloaded.active_leaf_id("session") == follow_up_answer_id);
        assert(reloaded.active_messages("session").back().content == "B continuation");

        // Hard rewind removes the entire descendant subtree, including
        // inactive branches and attachments, while preserving the checkpoint
        // and sibling alternatives that branch before it.
        nyaxus::StoredMessage trim_user;
        trim_user.parent_id = a1_id;
        trim_user.session_id = "session";
        trim_user.role = "user";
        trim_user.content = "trim me";
        trim_user.run_id = "run-trim-user";
        trim_user.created_at = "t6";
        const auto trim_user_id = reloaded.append_message(trim_user);
        nyaxus::StoredMessage trim_answer;
        trim_answer.parent_id = trim_user_id;
        trim_answer.session_id = "session";
        trim_answer.role = "assistant";
        trim_answer.content = "trim answer";
        trim_answer.run_id = "run-trim-answer";
        trim_answer.created_at = "t7";
        const auto trim_answer_id = reloaded.append_message(trim_answer);
        nyaxus::StoredAttachment trim_attachment;
        trim_attachment.message_id = trim_user_id;
        trim_attachment.name = "trim.txt";
        trim_attachment.kind = "text";
        trim_attachment.size = 5;
        trim_attachment.content = "prune";
        reloaded.insert_attachment(trim_attachment);
        assert(reloaded.trim_descendants("session", a1_id));
        assert(reloaded.get_message(a1_id).has_value());
        assert(!reloaded.get_message(trim_user_id).has_value());
        assert(!reloaded.get_message(trim_answer_id).has_value());
        assert(reloaded.attachments_for_message(trim_user_id).empty());
        assert(reloaded.get_message(a2_id).has_value());
        assert(reloaded.get_message(follow_up_id).has_value());
    }

    // Content-addressed attachment dedup: identical content in one session is
    // stored once and readers resolve the reference row; different content and
    // different sessions keep their own copies.
    {
        // Known-answer check for the hash (FIPS 180-4 test vectors).
        assert(nyaxus::sha256_hex("abc") ==
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        assert(nyaxus::sha256_hex("") ==
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

        nyaxus::SqliteStore dedup_store(store_path);
        nyaxus::StoredMessage dup_user;
        dup_user.session_id = "dedup-session";
        dup_user.role = "user";
        dup_user.content = "first copy";
        dup_user.created_at = "t10";
        const auto dup_user_id = dedup_store.append_message(dup_user);
        nyaxus::StoredAttachment first;
        first.message_id = dup_user_id;
        first.name = "note.txt";
        first.kind = "text";
        first.size = 14;
        first.content = "shared payload";
        dedup_store.insert_attachment(first);

        nyaxus::StoredMessage dup_user2;
        dup_user2.session_id = "dedup-session";
        dup_user2.role = "user";
        dup_user2.content = "second copy";
        dup_user2.created_at = "t11";
        const auto dup_user2_id = dedup_store.append_message(dup_user2);
        nyaxus::StoredAttachment second;
        second.message_id = dup_user2_id;
        second.name = "note-copy.txt";
        second.kind = "text";
        second.size = 14;
        second.content = "shared payload";
        dedup_store.insert_attachment(second);

        // Both reads resolve to the same content under their own metadata.
        const auto first_rows = dedup_store.attachments_for_message(dup_user_id);
        const auto second_rows = dedup_store.attachments_for_message(dup_user2_id);
        assert(first_rows.size() == 1 && first_rows.front().content == "shared payload");
        assert(second_rows.size() == 1 && second_rows.front().content == "shared payload");
        assert(second_rows.front().name == "note-copy.txt");
        assert(!first_rows.front().content_hash.empty());
        assert(first_rows.front().content_hash == second_rows.front().content_hash);

        // The content route serves the reference row's bytes.
        const auto served = dedup_store.get_attachment(second_rows.front().id, "dedup-session");
        assert(served.has_value() && served->content == "shared payload");

        // Different content is not deduped away.
        nyaxus::StoredAttachment other;
        other.message_id = dup_user_id;
        other.name = "other.txt";
        other.kind = "text";
        other.size = 17;
        other.content = "different payload";
        dedup_store.insert_attachment(other);
        const auto other_rows = dedup_store.attachments_for_message(dup_user_id);
        assert(other_rows.size() == 2);
        assert(other_rows.front().content == "shared payload");
        assert(other_rows.back().content == "different payload");

        // Another session keeps its own canonical copy of the same content.
        nyaxus::StoredMessage elsewhere_user;
        elsewhere_user.session_id = "dedup-session-2";
        elsewhere_user.role = "user";
        elsewhere_user.content = "same bytes elsewhere";
        elsewhere_user.created_at = "t12";
        const auto elsewhere_user_id = dedup_store.append_message(elsewhere_user);
        nyaxus::StoredAttachment elsewhere;
        elsewhere.message_id = elsewhere_user_id;
        elsewhere.name = "note.txt";
        elsewhere.kind = "text";
        elsewhere.size = 14;
        elsewhere.content = "shared payload";
        dedup_store.insert_attachment(elsewhere);
        const auto elsewhere_rows = dedup_store.attachments_for_message(elsewhere_user_id);
        assert(elsewhere_rows.size() == 1 && elsewhere_rows.front().content == "shared payload");
    }

    nyaxus::CancellationSource source;
    assert(!source.token().cancelled());
    source.cancel();
    assert(source.token().cancelled());

    std::filesystem::remove_all(root);
    return 0;
}
