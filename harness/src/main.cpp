#include "nyaxus/core.hpp"
#include "nyaxus/store.hpp"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::filesystem::path store_path = "nyaxus-events.db";
    std::string prompt = "inspect the workspace and report the current harness state";

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--ledger" && index + 1 < argc) {
            store_path = argv[++index];
        } else if (argument == "--prompt" && index + 1 < argc) {
            prompt = argv[++index];
        } else if (argument == "--help") {
            std::cout << "nyaxus_harness [--ledger PATH] [--prompt TEXT]\n";
            return 0;
        }
    }

    const std::string session_id = "demo-session";
    const std::string run_id = "demo-run";
    nyaxus::SqliteStore store(store_path);

    nyaxus::ProfileDefaults defaults;
    nyaxus::ProfileDefinition definition;
    definition.id = "orchestrator";
    definition.identity_prompt = "Keep answers direct and report actions precisely.";
    const nyaxus::ResolvedProfile profile = nyaxus::resolve_profile(defaults, definition);

    const auto emit = [&](const std::string& type,
                          std::map<std::string, std::string> payload = {}) {
        std::cout << nyaxus::event_json(
                         store.append_event(session_id, run_id, "", type, std::move(payload)))
                  << '\n';
    };

    emit("profile.resolved", {{"profile_id", profile.id}, {"snapshot_id", profile.snapshot_id}});
    emit("run.accepted", {{"prompt", prompt}});
    emit("run.queued", {{"reason", "demo scheduler"}});
    emit("run.started", {{"model", profile.base_model}, {"provider", profile.provider}});
    emit("reasoning.delta", {{"text", "The native event ledger is active."}});
    emit("text.delta", {{"text", "The harness core is ready for the provider and tool layers."}});
    emit("run.completed", {{"output_tokens", "13"}});
    return 0;
}
