#include "nyaxus/provider.hpp"

#include <chrono>
#include <thread>

#include "nyaxus/http.hpp"

namespace nyaxus {
namespace provider {
namespace {

std::string trim(std::string text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n' ||
                             text.front() == '\r' || text.front() == '\t')) {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' ||
                             text.back() == '\r' || text.back() == '\t')) {
        text.pop_back();
    }
    return text;
}

// Tool output is whatever a local shell printed: Windows console output is
// cp850/cp1252, not UTF-8, and raw bytes embedded in the request make the
// provider's JSON parser reject the whole call (HTTP 500, parse_error.101).
// Replace each ill-formed sequence with U+FFFD so every message the request
// carries is valid UTF-8.
std::string sanitize_utf8(std::string text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        int extra = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            out += "\xEF\xBF\xBD";  // stray continuation byte
            ++i;
            continue;
        }
        bool ok = i + static_cast<std::size_t>(extra) < text.size();
        unsigned cp = 0;
        for (int k = 0; ok && k <= extra; ++k) {
            const unsigned char b = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if (k == 0) cp = c & (extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07);
            else {
                if ((b & 0xC0) != 0x80) {
                    ok = false;
                } else {
                    cp = (cp << 6) | (b & 0x3F);
                }
            }
        }
        // Strict code-point validation: overlong encodings, UTF-16
        // surrogate halves (D800-DFFF) and beyond-plane garbage are valid
        // BYTES but invalid UTF-8 - llama.cpp's JSON parser rejects the
        // whole request for one such sequence (parse_error.101), so the
        // replacement character covers them all.
        if (ok) {
            if (extra == 1) ok = cp >= 0x80;
            else if (extra == 2) ok = cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF);
            else ok = cp >= 0x10000 && cp <= 0x10FFFF;
        }
        if (!ok) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(text, i, 1 + static_cast<std::size_t>(extra));
            i += 1 + static_cast<std::size_t>(extra);
        }
    }
    return out;
}

}  // namespace

json::Value build_request(const Config& config, const std::vector<ChatMessage>& messages) {
    json::Value body(json::Value::Type::Object);
    body["model"] = json::Value(config.model);
    body["stream"] = json::Value(true);
    if (config.temperature > 0.0) body["temperature"] = json::Value(config.temperature);
    if (config.max_tokens > 0) body["max_tokens"] = json::Value(config.max_tokens);
    if (config.top_p.has_value()) body["top_p"] = json::Value(*config.top_p);
    if (config.top_k.has_value()) body["top_k"] = json::Value(*config.top_k);
    if (config.min_p.has_value()) body["min_p"] = json::Value(*config.min_p);
    if (config.frequency_penalty.has_value()) body["frequency_penalty"] = json::Value(*config.frequency_penalty);
    if (config.presence_penalty.has_value()) body["presence_penalty"] = json::Value(*config.presence_penalty);
    if (config.repeat_penalty.has_value()) body["repeat_penalty"] = json::Value(*config.repeat_penalty);
    if (config.seed.has_value()) body["seed"] = json::Value(*config.seed);
    // Custom passthrough parameters: sent verbatim, so a model-specific knob
    // can be configured without a code change. Servers ignore unknown fields.
    for (const auto& entry : config.extra_params) {
        body[entry.first] = entry.second;
    }

    // Tool declarations (OpenAI function-calling). Only sent when the run has
    // tools; servers that do not support the field ignore it, and the model
    // simply never emits a tool call (the run loop breaks on zero calls).
    if (!config.tools.empty()) {
        body["tools"] = json::Value(config.tools);
    }

    // llama.cpp's OpenAI server forwards chat_template_kwargs to the model's
    // chat template; Qwen3-family templates honor enable_thinking. Servers
    // that do not know the field ignore it.
    if (config.no_thinking) {
        json::Value kwargs(json::Value::Type::Object);
        kwargs["enable_thinking"] = json::Value(false);
        body["chat_template_kwargs"] = json::Value(std::move(kwargs));
    }

    // llama.cpp's OpenAI server only appends the final usage chunk when the
    // client asks for it; without this the context meter has no data.
    json::Value stream_options(json::Value::Type::Object);
    stream_options["include_usage"] = json::Value(true);
    body["stream_options"] = json::Value(std::move(stream_options));

    json::Array message_array;
    for (const ChatMessage& message : messages) {
        const std::string content = sanitize_utf8(message.content);
        json::Value item(json::Value::Type::Object);
        item["role"] = json::Value(message.role);
        if (message.role == "assistant" && !message.tool_calls.empty()) {
            // An assistant turn that issued tool calls: carry the text (if
            // any) alongside the calls so the model can read the results and
            // continue the run.
            if (!content.empty()) {
                item["content"] = json::Value(content);
            }
            json::Array calls;
            for (const ChatMessage::ToolCall& tc : message.tool_calls) {
                json::Value fn(json::Value::Type::Object);
                fn["name"] = json::Value(sanitize_utf8(tc.name));
                fn["arguments"] = json::Value(sanitize_utf8(tc.arguments));
                json::Value call(json::Value::Type::Object);
                call["id"] = json::Value(sanitize_utf8(tc.id));
                call["type"] = json::Value("function");
                call["function"] = std::move(fn);
                calls.push_back(std::move(call));
            }
            item["tool_calls"] = json::Value(std::move(calls));
        } else if (message.role == "tool") {
            // A tool result; tool_call_id ties it to the call it answers.
            item["content"] = json::Value(content);
            if (!message.tool_call_id.empty()) {
                item["tool_call_id"] = json::Value(message.tool_call_id);
            }
        } else if (message.image_data_urls.empty()) {
            item["content"] = json::Value(content);
        } else {
            json::Array parts;
            if (!content.empty()) {
                json::Value text_part(json::Value::Type::Object);
                text_part["type"] = json::Value("text");
                text_part["text"] = json::Value(content);
                parts.push_back(std::move(text_part));
            }
            for (const std::string& data_url : message.image_data_urls) {
                json::Value image_part(json::Value::Type::Object);
                image_part["type"] = json::Value("image_url");
                json::Value image_url(json::Value::Type::Object);
                image_url["url"] = json::Value(data_url);
                image_part["image_url"] = std::move(image_url);
                parts.push_back(std::move(image_part));
            }
            item["content"] = json::Value(std::move(parts));
        }
        message_array.push_back(std::move(item));
    }
    body["messages"] = json::Value(std::move(message_array));
    return body;
}

namespace {

void emit_delta(EventSink sink,
                const std::string& type,
                const std::string& text) {
    if (text.empty() || !sink) return;
    json::Value payload(json::Value::Type::Object);
    payload["text"] = json::Value(text);
    sink(type, payload);
}

// OpenAI-compatible servers stream function.arguments as fragments. Some
// servers send cumulative prefixes, others send append-only pieces; accept
// both forms without emitting a tool until the complete response is received.
void append_stream_piece(std::string& target, const std::string& piece) {
    if (piece.empty()) return;
    if (target.empty()) {
        target = piece;
    } else if (piece.size() >= target.size() &&
               piece.compare(0, target.size(), target) == 0) {
        target = piece;
    } else if (target.size() >= piece.size() &&
               target.compare(0, piece.size(), piece) == 0) {
        // The provider repeated the prefix we already have.
    } else {
        target += piece;
    }
}

}  // namespace

bool discover_models(const Config& config, std::vector<ModelInfo>& out) {
    if (config.endpoint.empty()) return false;
    std::string base = config.endpoint;
    while (!base.empty() && base.back() == '/') base.pop_back();

    std::map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    if (!config.api_key.empty()) headers["Authorization"] = "Bearer " + config.api_key;

    std::string models_url = base;
    if (models_url.size() > 7 &&
        models_url.compare(models_url.size() - 7, 7, "/models") == 0) {
        // Endpoint already includes the /models path; query it as-is.
    } else {
        models_url += "/models";
    }
    std::string body;
    if (!http::get_body(models_url, headers, body)) return false;

    json::Value root;
    try {
        root = json::parse(body);
    } catch (...) {
        return false;
    }

    const json::Array& models = root["data"].as_array();
    for (const json::Value& item : models) {
        ModelInfo info;
        info.id = item["id"].as_string();
        if (info.id.empty()) info.id = item["name"].as_string();
        info.context_window = item["meta"]["n_ctx"].as_integer(0);
        out.push_back(std::move(info));
    }
    return !out.empty();
}

bool OpenAICompatibleProvider::stream(const Config& config,
                                      const std::vector<ChatMessage>& messages,
                                      EventSink sink,
                                      CancellationToken cancellation,
                                      std::string* error_out) {
    if (cancellation.cancelled()) return true;
    if (config.endpoint.empty()) {
        if (error_out) *error_out = "no model endpoint configured";
        return false;
    }

    std::string base = config.endpoint;
    while (!base.empty() && base.back() == '/') base.pop_back();
    // discover_models() tolerates an endpoint that includes the /models
    // path (the connections-tab reachability check passes on it), so chat
    // must tolerate it too - otherwise a config the UI validates would
    // POST to /v1/models/chat/completions and 404 ("File Not Found",
    // llama.cpp's static-file handler). Strip the suffix the same way.
    if (base.size() > 7 && base.compare(base.size() - 7, 7, "/models") == 0) {
        base.resize(base.size() - 7);
    }
    const std::string url = base + "/chat/completions";

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Accept"] = "text/event-stream";
    if (!config.api_key.empty()) {
        headers["Authorization"] = "Bearer " + config.api_key;
    }

    const std::string body = json::stringify(build_request(config, messages));

    std::string accumulated_text;
    std::string accumulated_reasoning;

    // llama.cpp answers 503 while a slot is occupied (and can drop the
    // connection under load). A first attempt that delivered nothing is
    // retried once after a short backoff, which fixes the "send the message
    // twice to wake the model" case. Once any content has streamed, a failure
    // is reported as-is instead of replayed, so no text is duplicated.
    // Stall watchdog: the provider's silence cap, resolved by the caller
    // (the daemon maps a 0 setting to kLegacyStallTimeoutMs; the one-shot
    // auxiliary calls pass it directly).
    const long long stall_ms = config.stall_timeout_ms;
    // Pre-delivery retry behavior: legacy (budget 0) = exactly one extra
    // attempt after 1.5s, whatever the failure. Budgeted: a STARTING-shaped
    // failure (HTTP 503 whose body carries llama.cpp's "Loading model" -
    // the model boots back up after an OOM, tens of seconds on CPU) rides
    // a longer backoff ladder; everything else keeps the legacy curve, so
    // a dead or busy endpoint still fails fast to the run.failure path.
    // Pre-delivery only: once any content streamed, a failure is reported
    // as-is instead of replayed (no duplicated text).
    const bool budgeted_retry = config.retry_budget_ms > 0;
    auto starting_shaped = [](const std::string& failure) {
        if (failure.find("HTTP 503") != std::string::npos) {
            std::string lowered;
            lowered.reserve(failure.size());
            for (const char c : failure) {
                lowered.push_back(
                    (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c);
            }
            return lowered.find("loading") != std::string::npos;
        }
        // Also patient for the two NO-status close shapes - llama.cpp
        // accepted the connection and died mid-call (an OOM restart or a
        // crash during a huge prefill): nothing streamed, so replaying the
        // SAME request resumes the work exactly (the request already
        // carries every completed tool result). A dead endpoint never gets
        // here - its failure surfaces as "connection failed", status 0.
        return failure.find("before the first payload") != std::string::npos ||
               failure.find("before the provider sent a response") !=
                   std::string::npos;
    };
    // The starting ladder: 2s, 4s, 6s, then 10s per attempt - ~78s total,
    // one boot window on CPU. Cancellation aborts at the previous check or
    // inside the stream callback, so it never waits out a backoff.
    static const int kLegacyAttempts = 2;
    static const int kStartingMaxAttempts = 10;
    const int loop_cap = budgeted_retry ? kStartingMaxAttempts : kLegacyAttempts;
    bool ladder_active = false;
    bool delivered = false;
    for (int attempt = 0; attempt < loop_cap; ++attempt) {
        if (cancellation.cancelled()) return true;
        struct PendingToolCall {
            long long index = -1;
            std::string id;
            std::string name;
            std::string arguments;
        };
        std::vector<PendingToolCall> pending_tool_calls;
        bool ambiguous_tool_calls = false;
        int status = 0;
        std::string error;
        bool stalled = false;
        bool ok = http::post_stream(
            url, body, headers,
            [&](const std::string& payload) -> bool {
                if (cancellation.cancelled()) return false;
                if (payload.empty()) return true;
                if (payload == "[DONE]") return false;
                delivered = true;

                json::Value chunk;
            try {
                chunk = json::parse(payload);
            } catch (...) {
                return true;
            }

            // The final usage chunk carries empty choices (the OpenAI
            // include_usage pattern, which llama.cpp follows), so this must
            // run before the choices early-return.
            if (chunk["usage"].type() == json::Value::Type::Object) {
                json::Value metrics(json::Value::Type::Object);
                metrics["prompt_tokens"] = chunk["usage"]["prompt_tokens"];
                metrics["completion_tokens"] = chunk["usage"]["completion_tokens"];
                metrics["total_tokens"] = chunk["usage"]["total_tokens"];
                // Reasoning models report thinking tokens nested under
                // completion_tokens_details; forward them so the client's
                // graph/run views (metrics.reasoningTokens) can show them.
                const json::Value& details = chunk["usage"]["completion_tokens_details"];
                metrics["reasoning_tokens"] =
                    (details.type() == json::Value::Type::Object)
                        ? details["reasoning_tokens"]
                        : json::Value(0);
                // Cached-prompt tokens: OpenAI-style
                // prompt_tokens_details.cached_tokens, with llama.cpp's
                // prompt_cache_hit_tokens as fallback. Left Null when the
                // provider reports no cache data at all - the daemon's
                // kv_cache_miss tripwire must not fire on "unknown", only on
                // a known miss.
                const json::Value& cache_details =
                    chunk["usage"]["prompt_tokens_details"];
                metrics["cached_tokens"] =
                    (cache_details.type() == json::Value::Type::Object &&
                     cache_details["cached_tokens"].type() == json::Value::Type::Number)
                        ? cache_details["cached_tokens"]
                        : (chunk["usage"]["prompt_cache_hit_tokens"].type() ==
                           json::Value::Type::Number)
                              ? chunk["usage"]["prompt_cache_hit_tokens"]
                              : json::Value();
                if (sink) sink("metrics", metrics);
                return true;
            }

            const auto& choices = chunk["choices"].as_array();
            if (choices.empty()) return true;
            const json::Value& choice = choices.front();
            const json::Value& delta = choice["delta"];

            const std::string reasoning_chunk = delta["reasoning_content"].as_string();
            if (!reasoning_chunk.empty()) {
                std::string reasoning_delta;
                if (reasoning_chunk.size() > accumulated_reasoning.size() &&
                    reasoning_chunk.compare(0, accumulated_reasoning.size(),
                                            accumulated_reasoning) == 0) {
                    reasoning_delta = reasoning_chunk.substr(accumulated_reasoning.size());
                    accumulated_reasoning = reasoning_chunk;
                } else {
                    reasoning_delta = reasoning_chunk;
                    accumulated_reasoning += reasoning_chunk;
                }
                emit_delta(sink, "reasoning.delta", reasoning_delta);
            }

            const std::string content_chunk = delta["content"].as_string();
            if (!content_chunk.empty()) {
                std::string text_delta;
                if (content_chunk.size() > accumulated_text.size() &&
                    content_chunk.compare(0, accumulated_text.size(),
                                          accumulated_text) == 0) {
                    text_delta = content_chunk.substr(accumulated_text.size());
                    accumulated_text = content_chunk;
                } else {
                    text_delta = content_chunk;
                    accumulated_text += content_chunk;
                }
                emit_delta(sink, "text.delta", text_delta);
            }

                const auto& tool_calls = delta["tool_calls"].as_array();
                for (const json::Value& call : tool_calls) {
                    const json::Value& function = call["function"];
                    const long long call_index = call["index"].as_integer(-1);
                    const std::string call_id = call["id"].as_string();
                    std::size_t slot = pending_tool_calls.size();
                    for (std::size_t candidate = 0; candidate < pending_tool_calls.size(); ++candidate) {
                        const PendingToolCall& pending = pending_tool_calls[candidate];
                        if ((call_index >= 0 && pending.index == call_index) ||
                            (!call_id.empty() && pending.id == call_id)) {
                            slot = candidate;
                            break;
                        }
                    }
                    // A single-call stream may omit both index and id on
                    // continuation chunks; attach those fragments to the only
                    // pending call instead of creating a bogus new call.
                    if (slot == pending_tool_calls.size()) {
                        if (call_index < 0 && call_id.empty()) {
                            if (pending_tool_calls.size() == 1) {
                                slot = 0;
                            } else if (pending_tool_calls.size() > 1) {
                                // Do not invent a third call when a provider
                                // omits all identity metadata on a multi-call
                                // continuation; fail the stream instead.
                                ambiguous_tool_calls = true;
                                continue;
                            }
                        } else if (call_index == 0 && call_id.empty() &&
                                   pending_tool_calls.size() == 1 &&
                                   pending_tool_calls.front().index < 0) {
                            // Late index metadata can identify the only call
                            // that was started without an index.
                            slot = 0;
                        }
                    }
                    if (slot == pending_tool_calls.size()) {
                        pending_tool_calls.push_back({});
                    }
                    PendingToolCall& pending = pending_tool_calls[slot];
                    if (pending.index < 0 && call_index >= 0) pending.index = call_index;
                    if (pending.id.empty() && !call_id.empty()) pending.id = call_id;
                    append_stream_piece(pending.name, function["name"].as_string());
                    append_stream_piece(pending.arguments, function["arguments"].as_string());
                }

                return true;
            },
            stall_ms, &status, &error,
            [&] { return cancellation.cancelled(); }, &stalled);

        if (cancellation.cancelled()) return true;
        if (stalled && sink) {
            // Make the stall visible (and ledgered) before the retry or the
            // failure: the web UI shows the warning bar from run.retrying /
            // run.stalled the moment the watchdog fires, not at run end.
            json::Value payload(json::Value::Type::Object);
            payload["attempt"] = json::Value(attempt + 1LL);
            payload["max_attempts"] = json::Value(2LL);
            payload["reason"] = json::Value(
                error.empty() ? std::string("provider stalled") : error);
            sink("run.stalled", payload);
        }
        if (ok) {
            if (ambiguous_tool_calls) {
                if (error_out) *error_out = "provider omitted tool-call identity metadata";
                return false;
            }
            // Only now are tool calls complete enough for the daemon to parse
            // and execute. In particular, never turn an arguments fragment such
            // as `{` or `"query` into its own tool call.
            for (const PendingToolCall& pending : pending_tool_calls) {
                if (!sink || pending.name.empty()) continue;
                json::Value tool_payload(json::Value::Type::Object);
                tool_payload["name"] = json::Value(pending.name);
                tool_payload["arguments"] = json::Value(pending.arguments);
                tool_payload["tool_call_id"] = json::Value(pending.id);
                sink("tool.call", tool_payload);
            }
            return true;
        }
        if (delivered) {
            if (error_out) *error_out = error;
            return false;
        }
        // A pre-delivery stall is retryable like a transport failure: nothing
        // streamed, so a replay cannot duplicate text. A mid-stream stall
        // already returned false above (the delivered check), so no replay.
        // The two no-status close shapes (llama died mid-call) are retryable
        // here for the same reason.
        const bool pre_delivery_close =
            error.find("before the first payload") != std::string::npos ||
            error.find("before the provider sent a response") != std::string::npos;
        const bool retryable = status == 0 || status == 502 || status == 503 ||
                               status == 504 || stalled || pre_delivery_close;
        // The ladder engages on the first starting-shaped failure and stays
        // engaged only while the failure KEEPS that shape: once llama is
        // loaded, a different failure (500, auth, dead port after a crash)
        // must fail now instead of riding the ladder out.
        if (budgeted_retry && !ladder_active && starting_shaped(error)) {
            ladder_active = true;
        }
        const int allowed_attempts = ladder_active ? kStartingMaxAttempts
                                                   : kLegacyAttempts;
        if (!retryable || attempt + 1 >= allowed_attempts) {
            if (error_out) *error_out = error;
            return false;
        }
        if (attempt >= kLegacyAttempts && !starting_shaped(error)) {
            if (error_out) *error_out = error;
            return false;
        }
        // Make the retry visible: the web UI shows an amber warning bar
        // (model busy / still loading) instead of a silent pause.
        if (sink) {
            json::Value payload(json::Value::Type::Object);
            payload["attempt"] = json::Value(
                static_cast<long long>(attempt + 1));
            payload["max_attempts"] = json::Value(
                static_cast<long long>(ladder_active ? kStartingMaxAttempts
                                                     : kLegacyAttempts));
            payload["reason"] = json::Value(
                error.empty() ? (ladder_active ? std::string("model starting")
                                               : std::string("connection failed"))
                              : error);
            sink("run.retrying", payload);
        }
        // Ladder backoff: 2s, 4s, 6s, then 10s per attempt; the legacy
        // curve stays the one 1.5s retry.
        long long backoff_ms = 1500;
        if (ladder_active) {
            backoff_ms = attempt == 0 ? 2000
                       : attempt == 1 ? 4000
                       : attempt == 2 ? 6000
                                      : 10000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
    }
    if (error_out) *error_out = "provider stream failed";
    return false;
}

bool FakeProvider::stream(const Config& config,
                          const std::vector<ChatMessage>& messages,
                          EventSink sink,
                          CancellationToken cancellation,
                          std::string* /*error_out*/) {
    const std::string reply =
        "The harness is running on the fake provider. The real llama.cpp "
        "endpoint has not been configured yet.";
    const std::string reasoning = "No model endpoint is configured, so this turn "
                                  "used the deterministic fake provider.";
    emit_delta(sink, "reasoning.delta", reasoning);
    if (cancellation.cancelled()) return true;
    for (std::size_t i = 0; i < reply.size(); i += 32) {
        if (cancellation.cancelled()) return true;
        emit_delta(sink, "text.delta", reply.substr(i, 32));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

}  // namespace provider
}  // namespace nyaxus