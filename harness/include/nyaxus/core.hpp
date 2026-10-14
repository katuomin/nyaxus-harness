#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nyaxus {

class CancellationToken {
   public:
    CancellationToken() = default;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> flag);

    bool cancelled() const;

   private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

class CancellationSource {
   public:
    CancellationSource();

    CancellationToken token() const;
    void cancel() const;

   private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

struct AdapterSpec {
    bool disabled = false;
    std::string id;
    std::string revision;
    double scale = 1.0;
};

struct ProfileDefaults {
    std::string provider = "openai-compatible";
    std::string endpoint = "http://127.0.0.1:8080/v1";
    std::string base_model = "Qwen3.8-27B";
    std::string identity_prompt;
    std::vector<std::string> skills;
    std::vector<std::string> tools = {"terminal"};
    std::optional<AdapterSpec> adapter;
    std::map<std::string, std::string> parameters;
};

struct ProfileDefinition {
    std::string id;
    std::string revision = "local-1";
    std::optional<std::string> provider;
    std::optional<std::string> endpoint;
    std::optional<std::string> base_model;
    std::optional<std::string> identity_prompt;
    std::optional<std::vector<std::string>> skills;
    std::optional<std::vector<std::string>> tools;
    std::optional<AdapterSpec> adapter;
    std::map<std::string, std::string> parameters;
};

struct ResolvedProfile {
    std::string id;
    std::string revision;
    std::string provider;
    std::string endpoint;
    std::string base_model;
    std::string identity_prompt;
    std::vector<std::string> skills;
    std::vector<std::string> tools;
    std::optional<AdapterSpec> adapter;
    std::map<std::string, std::string> parameters;
    std::string snapshot_id;
};

ResolvedProfile resolve_profile(const ProfileDefaults& defaults,
                                const ProfileDefinition& definition);

enum class RunStatus {
    Accepted,
    Queued,
    Running,
    WaitingApproval,
    WaitingInput,
    Completed,
    Cancelled,
    Failed,
    BudgetExhausted,
    Orphaned,
};

std::string run_status_name(RunStatus status);
bool can_transition(RunStatus from, RunStatus to);

struct Event {
    int schema_version = 1;
    std::string event_id;
    std::string session_id;
    std::string run_id;
    std::string parent_run_id;
    std::uint64_t sequence = 0;
    std::string event_type;
    std::string emitted_at;
    std::map<std::string, std::string> payload;
    // Global ledger row id (monotonic across runs). Serves as the SSE resume
    // cursor; 0 until the event is persisted or loaded from the ledger.
    std::uint64_t row_id = 0;
};

std::string event_json(const Event& event);

// Remove provider-internal thinking blocks accidentally embedded in assistant
// content before that content is sent back as conversation history. Stored
// content remains unchanged for snapshots and the UI.
std::string strip_inline_thinking(const std::string& content);

// Extract inline thinking blocks for the explicitly enabled diagnostic summary
// mode. Unclosed blocks contribute their remaining text as source material.
std::string extract_inline_thinking(const std::string& content);

}  // namespace nyaxus
