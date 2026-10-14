#include "nyaxus/core.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace nyaxus {
namespace {

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

std::string quote(const std::string& value) {
    return "\"" + json_escape(value) + "\"";
}

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

std::uint64_t fnv1a(const std::string& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << value;
    return out.str();
}

std::string optional_string(const std::optional<std::string>& value,
                            const std::string& fallback) {
    return value.has_value() ? *value : fallback;
}

std::string profile_fingerprint(const ResolvedProfile& profile) {
    std::ostringstream canonical;
    canonical << profile.id << '\n' << profile.revision << '\n' << profile.provider << '\n'
              << profile.endpoint << '\n' << profile.base_model << '\n'
              << profile.identity_prompt << '\n';
    for (const std::string& skill : profile.skills) canonical << "skill=" << skill << '\n';
    for (const std::string& tool : profile.tools) canonical << "tool=" << tool << '\n';
    for (const auto& [key, value] : profile.parameters) canonical << key << '=' << value << '\n';
    if (profile.adapter.has_value()) {
        canonical << "adapter=" << (profile.adapter->disabled ? "disabled" : "enabled")
                  << ':' << profile.adapter->id << '@' << profile.adapter->revision << ':'
                  << profile.adapter->scale << '\n';
    } else {
        canonical << "adapter=none\n";
    }
    return profile.revision + "-" + hex(fnv1a(canonical.str()));
}

}  // namespace

CancellationToken::CancellationToken(std::shared_ptr<std::atomic<bool>> flag)
    : flag_(std::move(flag)) {}

bool CancellationToken::cancelled() const {
    return flag_ && flag_->load(std::memory_order_acquire);
}

CancellationSource::CancellationSource()
    : flag_(std::make_shared<std::atomic<bool>>(false)) {}

CancellationToken CancellationSource::token() const {
    return CancellationToken(flag_);
}

void CancellationSource::cancel() const {
    flag_->store(true, std::memory_order_release);
}

ResolvedProfile resolve_profile(const ProfileDefaults& defaults,
                                const ProfileDefinition& definition) {
    ResolvedProfile resolved;
    resolved.id = definition.id;
    resolved.revision = definition.revision;
    resolved.provider = optional_string(definition.provider, defaults.provider);
    resolved.endpoint = optional_string(definition.endpoint, defaults.endpoint);
    resolved.base_model = optional_string(definition.base_model, defaults.base_model);
    resolved.identity_prompt =
        optional_string(definition.identity_prompt, defaults.identity_prompt);
    resolved.skills = definition.skills.value_or(defaults.skills);
    resolved.tools = definition.tools.value_or(defaults.tools);
    resolved.adapter = definition.adapter.has_value() ? definition.adapter : defaults.adapter;
    resolved.parameters = defaults.parameters;
    for (const auto& [key, value] : definition.parameters) resolved.parameters[key] = value;
    resolved.snapshot_id = profile_fingerprint(resolved);
    return resolved;
}

std::string run_status_name(RunStatus status) {
    switch (status) {
        case RunStatus::Accepted:
            return "accepted";
        case RunStatus::Queued:
            return "queued";
        case RunStatus::Running:
            return "running";
        case RunStatus::WaitingApproval:
            return "waiting_approval";
        case RunStatus::WaitingInput:
            return "waiting_input";
        case RunStatus::Completed:
            return "completed";
        case RunStatus::Cancelled:
            return "cancelled";
        case RunStatus::Failed:
            return "failed";
        case RunStatus::BudgetExhausted:
            return "budget_exhausted";
        case RunStatus::Orphaned:
            return "orphaned";
    }
    return "unknown";
}

bool can_transition(RunStatus from, RunStatus to) {
    if (from == RunStatus::Completed || from == RunStatus::Cancelled ||
        from == RunStatus::Failed || from == RunStatus::BudgetExhausted) {
        return false;
    }
    if (to == RunStatus::Accepted || to == RunStatus::Orphaned) return false;
    if (from == RunStatus::Accepted) {
        return to == RunStatus::Queued || to == RunStatus::Running ||
               to == RunStatus::Cancelled || to == RunStatus::Failed;
    }
    if (from == RunStatus::Queued) {
        return to == RunStatus::Running || to == RunStatus::Cancelled ||
               to == RunStatus::Failed;
    }
    if (from == RunStatus::Running || from == RunStatus::WaitingApproval ||
        from == RunStatus::WaitingInput) {
        return to == RunStatus::Running || to == RunStatus::WaitingApproval ||
               to == RunStatus::WaitingInput || to == RunStatus::Completed ||
               to == RunStatus::Cancelled || to == RunStatus::Failed ||
               to == RunStatus::BudgetExhausted;
    }
    return false;
}

std::string event_json(const Event& event) {
    std::ostringstream out;
    out << "{\"schema_version\":" << event.schema_version
        << ",\"event_id\":" << quote(event.event_id)
        << ",\"session_id\":" << quote(event.session_id)
        << ",\"run_id\":" << quote(event.run_id)
        << ",\"parent_run_id\":" << quote(event.parent_run_id)
        << ",\"seq\":" << event.sequence
        << ",\"id\":" << event.row_id
        << ",\"event_type\":" << quote(event.event_type)
        << ",\"emitted_at\":" << quote(event.emitted_at) << ",\"payload\":{";
    bool first = true;
    for (const auto& [key, value] : event.payload) {
        if (!first) out << ',';
        first = false;
        out << quote(key) << ':' << quote(value);
    }
    out << "}}";
    return out.str();
}

std::string strip_inline_thinking(const std::string& content) {
    constexpr const char* open = "<think>";
    constexpr const char* close = "</think>";
    constexpr std::size_t open_size = 7;
    constexpr std::size_t close_size = 8;

    std::string visible;
    std::size_t cursor = 0;
    while (cursor < content.size()) {
        const std::size_t start = content.find(open, cursor);
        if (start == std::string::npos) {
            visible.append(content, cursor, std::string::npos);
            break;
        }
        visible.append(content, cursor, start - cursor);
        const std::size_t end = content.find(close, start + open_size);
        if (end == std::string::npos) break;
        cursor = end + close_size;
    }
    return visible;
}

std::string extract_inline_thinking(const std::string& content) {
    constexpr const char* open = "<think>";
    constexpr const char* close = "</think>";
    constexpr std::size_t open_size = 7;
    constexpr std::size_t close_size = 8;

    std::string thinking;
    std::size_t cursor = 0;
    while (cursor < content.size()) {
        const std::size_t start = content.find(open, cursor);
        if (start == std::string::npos) break;
        const std::size_t end = content.find(close, start + open_size);
        if (end == std::string::npos) {
            thinking.append(content, start + open_size, std::string::npos);
            break;
        }
        if (!thinking.empty()) thinking += "\n";
        thinking.append(content, start + open_size, end - start - open_size);
        cursor = end + close_size;
    }
    return thinking;
}

}  // namespace nyaxus
