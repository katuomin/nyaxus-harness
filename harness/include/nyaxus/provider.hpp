#pragma once

#include <functional>
#include <string>
#include <vector>

#include "nyaxus/core.hpp"
#include "nyaxus/json.hpp"

namespace nyaxus {
namespace provider {

// Stall watchdog silence caps, shared with the daemon's run.stall_timeout_s
// default so the two cannot drift apart:
//   kDefaultStallTimeoutMs - the default cap (60 s) a run stream tolerates
//   kLegacyStallTimeoutMs  - the pre-watchdog cap (300 s), used when the
//                            operator sets run.stall_timeout_s = 0, and by
//                            the one-shot auxiliary calls (extraction,
//                            journey, summary), which are not operator-tuned
constexpr long long kDefaultStallTimeoutMs = 60 * 1000;
constexpr long long kLegacyStallTimeoutMs = 300 * 1000;

struct ChatMessage {
    std::string role;
    std::string content;
    // OpenAI-compatible image data URLs. The default keeps existing
    // {role, content} aggregate initializers source-compatible.
    std::vector<std::string> image_data_urls = {};

    // Native function-calling round-trip (only used by the agent loop).
    // When role is "assistant" and tool_calls is non-empty, the message is
    // serialized with a `tool_calls` array (and content, if any). When role
    // is "tool", tool_call_id identifies which call this result answers.
    struct ToolCall {
        std::string id;
        std::string name;
        std::string arguments;  // JSON string, as the model emitted it
    };
    std::string tool_call_id;
    std::vector<ToolCall> tool_calls;
};

struct Config {
    std::string endpoint;  // e.g. http://llama-cpp-manager:8080/v1
    std::string model;
    std::string api_key;
    double temperature = 0.7;
    int max_tokens = 0;       // 0 = provider default
    int context_window = 0;   // 0 = unknown, resolved from discovery
    bool vision = false;      // capability of the configured endpoint/model
    // Ask the chat template to disable model thinking (llama.cpp
    // `chat_template_kwargs.enable_thinking`). Small auxiliary models burn
    // significant latency on thinking for tiny structured outputs.
    bool no_thinking = false;
    // Optional sampling parameters. nullopt = not sent to the API (the server
    // uses its own default). Only the common ones are first-class fields; any
    // other parameter the operator wants lives in extra_params.
    std::optional<double> top_p;
    std::optional<int> top_k;
    std::optional<double> min_p;
    std::optional<double> frequency_penalty;
    std::optional<double> presence_penalty;
    std::optional<double> repeat_penalty;
    std::optional<int> seed;
    // Custom passthrough parameters, sent verbatim to the request body. Lets a
    // model-specific knob (e.g. a DRY setting) be configured without a code
    // change. Servers that do not know a field ignore it.
    json::Object extra_params;
    // Tool declarations in OpenAI function-calling format. Empty = the model
    // has no tools for this run. The daemon populates this per run from the
    // enabled capabilities (terminal always; memory tools when the memory
    // bank is reachable). The response parser aggregates streamed
    // `delta.tool_calls`, so a non-empty array is all that is needed for the
    // model to emit calls.
    json::Array tools;
    // Stall watchdog: max silence (ms) tolerated on the SSE stream, both
    // before the first payload and between payloads. Used as-is (a positive
    // value; callers resolve 0 to kLegacyStallTimeoutMs). A stall is treated
    // like a transport failure: the stream is retried once when nothing was
    // delivered, and fails (partial text persisted by the daemon) when
    // content already streamed, so a replay never duplicates text.
    long long stall_timeout_ms = 0;
    // Patient pre-delivery retry ladder (0 = legacy single retry after
    // 1.5s). When budgeted (>0), a STARTING-shaped pre-delivery failure -
    // llama.cpp answers 503 "Loading model" (or drops the connection)
    // while the model boots back up after an OOM, which takes tens of
    // seconds on CPU - retries through a longer backoff ladder instead of
    // failing after one quick attempt. Non-starting failures (502/504,
    // dead endpoint, busy-slot 503, pre-delivery stall) keep the legacy
    // behavior. The run stays cancellable at every attempt; every retry
    // publishes run.retrying so the web shows the attempt count. Only the
    // main pipeline sets this - aux/embed/summary calls keep the legacy
    // curve so a slow endpoint cannot crawl the memory paths.
    long long retry_budget_ms = 0;
};

struct ModelInfo {
    std::string id;
    long long context_window = 0;
};

// Query the OpenAI-compatible `/models` endpoint and populate available models
// and their context windows. Returns false on transport/parse error.
bool discover_models(const Config& config, std::vector<ModelInfo>& out);

// Serialize one chat request body (model, sampling, messages, tools) exactly
// as the provider client sends it. Exposed for token-budget estimation.
json::Value build_request(const Config& config,
                          const std::vector<ChatMessage>& messages);

// Emitted during a provider stream. `type` is one of:
//   reasoning.delta, text.delta, metrics, tool.call
using EventSink = std::function<void(const std::string& type, const json::Value& payload)>;

class Provider {
   public:
    virtual ~Provider() = default;

    // Stream one conversation turn. Returns false on transport/provider error;
    // `error_out` (when non-null) then carries a human-readable reason for the
    // run.failed event.
    virtual bool stream(const Config& config,
                        const std::vector<ChatMessage>& messages,
                        EventSink sink,
                        CancellationToken cancellation,
                        std::string* error_out = nullptr) = 0;
};

// OpenAI-compatible chat completions streaming (llama.cpp / vLLM / any
// OpenAI-compatible endpoint). Plain HTTP only (internal network).
class OpenAICompatibleProvider : public Provider {
   public:
    bool stream(const Config& config,
                const std::vector<ChatMessage>& messages,
                EventSink sink,
                CancellationToken cancellation,
                std::string* error_out = nullptr) override;
};

// Deterministic canned output for local tests before a model endpoint exists.
class FakeProvider : public Provider {
   public:
    bool stream(const Config& config,
                const std::vector<ChatMessage>& messages,
                EventSink sink,
                CancellationToken cancellation,
                std::string* error_out = nullptr) override;
};

}  // namespace provider
}  // namespace nyaxus