// daemon_summary.cpp - Daemon's summary/compaction pipeline as out-of-class
// definitions (docs/daemon-split-analysis.md, step 2). The class declaration
// lives in daemon.hpp; everything here is a verbatim relocation of the
// original in-class bodies.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nyaxus/argvpolicy.hpp"
#include "nyaxus/core.hpp"
#include "nyaxus/daemon.hpp"
#include "nyaxus/http.hpp"
#include "nyaxus/json.hpp"
#include "nyaxus/log.hpp"
#include "nyaxus/loopguard.hpp"
#include "nyaxus/memory.hpp"
#include "nyaxus/memory_custom.hpp"
#include "nyaxus/memorydb.hpp"
#include "nyaxus/promptpacks.hpp"
#include "nyaxus/provider.hpp"
#include "nyaxus/skills.hpp"
#include "nyaxus/qwen3.hpp"
#include "nyaxus/store.hpp"
#include "nyaxus/summary.hpp"

namespace nyaxus {

long long Daemon::summary_budget_bytes() const
{
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            if (config_.summary_source_budget_bytes > 0) {
                return config_.summary_source_budget_bytes;
            }
        }
        const auto summary_provider = summary_provider_config();
        const int context = summary_provider.context_window;
        const int tokens = context > 0 ? std::min(context, 128 * 1024) : 32 * 1024;
        // Conservative 4 bytes/token estimate for the text channel. Images are
        // a separate channel and do not count against this budget.
        return static_cast<long long>(tokens) * 4;
    }

long long Daemon::estimate_request_tokens(const std::string& serialized)
{
        std::lock_guard<std::mutex> lock(title_model_mutex_);
        if (ensure_title_model_locked()) {
            return static_cast<long long>(
                qwen3::encode(title_model_, serialized).size());
        }
        return (static_cast<long long>(serialized.size()) + 1) / 2;
    }

long long Daemon::context_budget_tokens(const provider::Config& cfg) const
{
        const long long window = cfg.context_window;
        if (window <= 0) return 0;
        const long long response = std::max<long long>(
            cfg.max_tokens, kContextBudgetMinResponseTokens);
        return window - response - kContextBudgetSafetyTokens;
    }

long long Daemon::mechanical_document_budget() const
{
        const long long budget_tokens = context_budget_tokens(provider_snapshot());
        // The reserve covers the system prompt, the latest turn, the request
        // envelope, and JSON escaping of the document (measured ~40% growth
        // for sectioned markdown with citations). Deliberately conservative:
        // a mechanical document that overflows defeats the compaction.
        const long long reserve_tokens = 8192;
        if (budget_tokens <= reserve_tokens) return 0;
        return (budget_tokens - reserve_tokens) * 2;
    }

long long Daemon::microcompact_tool_results( std::vector<provider::ChatMessage>& history, std::size_t& cleared, long long deficit_tokens)
{
        cleared = 0;
        // Collected newest-first, so the oldest result is at the end.
        std::vector<std::size_t> tool_indices;
        for (std::size_t i = history.size(); i-- > 0;) {
            if (history[i].role == "tool") tool_indices.push_back(i);
        }
        if (deficit_tokens < kMicrocompactMinSavingsTokens) return 0;
        // Clear OLDEST-first, just until the request fits. The old fixed
        // keep-count finished with the model forgetting commands from one
        // or two turns back (five newest results ≈ one batch). Cleared
        // bodies keep a head snippet (which command, which page), and the
        // spills keep the full text recoverable in-band. The emergency
        // floor is how many results survive even in a deficit - clearing
        // past that is the aux compaction's job.
        long long freed_bytes = 0;
        const long long target_bytes = deficit_tokens * 2;  // conservative 2 bytes/token
        for (std::size_t k = tool_indices.size(); k-- > kMicrocompactEmergencyKeep;) {
            const std::size_t i = tool_indices[k];
            const std::string& body = history[i].content;
            if (body.size() <= kMicrocompactSnippetBytes) continue;
            history[i].content = body.substr(0, kMicrocompactSnippetBytes) +
                "\n[... " + std::to_string(body.size() - kMicrocompactSnippetBytes) +
                " bytes cleared for the context budget - re-run the command "
                "or read the spill locator for the full result ...]";
            freed_bytes += static_cast<long long>(body.size() - kMicrocompactSnippetBytes);
            ++cleared;
            if (freed_bytes >= target_bytes) break;
        }
        if (cleared == 0) return 0;
        return (freed_bytes + 1) / 2;
    }

bool Daemon::compact_chain_to_checkpoint(const std::shared_ptr<Daemon::Session>& session, const provider::Config& provider_config, std::vector<StoredMessage> chain, long long estimated_tokens, CancellationToken cancellation, std::string& detail)
{
        const std::string sid = session->id;
        const long long window = provider_config.context_window;
        const long long tail_budget = std::max(
            static_cast<long long>(window * kContextBudgetTailRatio),
            kContextBudgetMinTailTokens);
        if (chain.size() < 4) {
            detail = "session too short to compact";
            return false;
        }
        // Keep the most recent turns within the tail budget. The walk always
        // keeps the last message, so the cut point is at least one message
        // before the end; head is everything before the tail.
        long long tail_tokens = 0;
        std::size_t tail_start = chain.size();
        for (std::size_t i = chain.size(); i-- > 0;) {
            const StoredMessage& message = chain[i];
            const long long cost =
                estimate_request_tokens(message.content) + 16;
            if (tail_tokens + cost > tail_budget && tail_start < chain.size())
                break;
            tail_tokens += cost;
            tail_start = i;
        }
        if (tail_start >= chain.size() || tail_start == 0) {
            detail = "no usable compaction split";
            return false;
        }
        std::vector<StoredMessage> head(chain.begin(),
                                        chain.begin() + tail_start);
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            session->context_mode = "summary";
            session->context_status = "preparing";
            session->context_summary.clear();
            session->context_error.clear();
            session->context_watermark = head.back().id;
            persist_session_locked(session);
        }
        publish(store_->append_event(
            sid, "context-" + sid, "", "context.compaction_started",
            {{"reason", "budget"},
             {"estimated_tokens", std::to_string(estimated_tokens)},
             {"window", std::to_string(window)},
             {"head_messages", std::to_string(head.size())},
             {"tail_messages", std::to_string(chain.size() - tail_start)}}));
        prepare_summary(session, std::move(head), cancellation);
        bool ready = false;
        long long summary_length = 0;
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            ready = session->context_status == "ready" &&
                    !session->context_summary.empty();
            summary_length = static_cast<long long>(
                session->context_summary.size());
        }
        if (!ready) {
            // Compaction failed: revert to the selected transcript so the
            // run proceeds with the original context (the provider's own
            // 400 is the honest failure if it still does not fit).
            std::lock_guard<std::mutex> lock(session->context_mutex);
            session->context_mode = "selected";
            session->context_status = "ready";
            session->context_summary.clear();
            session->context_error.clear();
            session->context_watermark = 0;
            persist_session_locked(session);
            detail = "budget compaction failed";
            publish(store_->append_event(
                sid, "context-" + sid, "", "context.compaction_failed",
                {{"reason", "budget"}}));
            session->context_cv.notify_all();
            return false;
        }
        publish(store_->append_event(
            sid, "context-" + sid, "", "context.compacted",
            {{"reason", "budget"},
             {"summary_length", std::to_string(summary_length)},
             {"head_messages", std::to_string(head.size())},
             {"tail_messages", std::to_string(chain.size() - tail_start)}}));
        // Story checkpoint: a compaction boundary is a natural place to
        // update the session's journey (durable job, deduped per coverage
        // window).
        if (memory_custom_enabled()) enqueue_journey_job(sid);
        session->context_cv.notify_all();
        return true;
    }

bool Daemon::compact_session_for_budget(const std::shared_ptr<Daemon::Session>& session, const std::shared_ptr<Daemon::Run>& run, const provider::Config& provider_config, long long estimated_tokens, std::string& detail)
{
        std::vector<StoredMessage> chain;
        long long cursor = run->parent_message_id;
        for (int guard = 0; guard < 10000 && cursor > 0; ++guard) {
            const auto message = store_->get_message(cursor);
            if (!message) break;
            chain.push_back(*message);
            cursor = message->parent_id;
        }
        std::reverse(chain.begin(), chain.end());
        return compact_chain_to_checkpoint(
            session, provider_config, std::move(chain), estimated_tokens,
            CancellationToken(run->cancel_flag), detail);
    }

bool Daemon::recompress_session_for_budget(const std::shared_ptr<Daemon::Session>& session, const std::shared_ptr<Daemon::Run>& run, std::string& detail)
{
        const std::string sid = session->id;
        long long watermark = 0;
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            watermark = session->context_watermark;
        }
        std::vector<StoredMessage> tail;
        long long cursor = store_->active_leaf_id(sid);
        for (int guard = 0; guard < 10000 && cursor > 0; ++guard) {
            const auto message = store_->get_message(cursor);
            if (!message) break;
            if (message->id > watermark) tail.push_back(*message);
            cursor = message->parent_id;
        }
        std::reverse(tail.begin(), tail.end());
        if (tail.empty()) {
            detail = "no tail to re-compress";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            session->context_status = "preparing";
            persist_session_locked(session);
        }
        publish(store_->append_event(
            sid, "context-" + sid, "", "context.recompressing",
            {{"reason", "budget"},
             {"tail_message_count", std::to_string(tail.size())}}));
        prepare_recompression(session, std::move(tail),
                              CancellationToken(run->cancel_flag));
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            ready = session->context_status == "ready" &&
                    !session->context_summary.empty();
        }
        if (!ready) {
            // finish/abort already restored the previous checkpoint; nothing
            // to undo.
            detail = "budget re-compression failed";
            return false;
        }
        session->context_cv.notify_all();
        return true;
    }

bool Daemon::build_source_segment(const std::vector<StoredMessage>& messages, const std::string& header_note, provider::ChatMessage& source_document, std::string& error)
{
        const auto auxiliary = summary_provider_config();
        const bool include_reasoning = summary_reasoning_enabled();
        source_document.role = "user";
        source_document.content = header_note;
        for (const StoredMessage& message : messages) {
            const std::string visible_content = message.role == "assistant"
                ? strip_inline_thinking(message.content)
                : message.content;
            std::string bounded = visible_content;
            if (bounded.size() > summary::kMaxMessageContentBytes) {
                bounded = bounded.substr(0, summary::kMaxMessageContentBytes) +
                          "\n[content truncated: " +
                          std::to_string(visible_content.size() -
                                         summary::kMaxMessageContentBytes) +
                          " more bytes omitted]";
            }
            source_document.content +=
                "\n--- source message id: " + std::to_string(message.id) +
                ", role: " + message.role + " ---\n" + bounded + "\n";
            for (const StoredAttachment& attachment :
                 store_->attachments_for_message(message.id)) {
                if (attachment.kind == "image") {
                    if (!auxiliary.vision) {
                        // Text-only auxiliary: reference the image by message
                        // id instead of inlining it, so a small non-vision
                        // model can still summarize image-bearing chats.
                        source_document.content +=
                            "[image attachment in source message " +
                            std::to_string(message.id) + ": " + attachment.name +
                            " - not inlined (auxiliary vision disabled); note "
                            "its message id if the image is material]\n";
                        continue;
                    }
                    std::string mime_type;
                    std::string encoded_payload;
                    if (!parse_image_data_url(attachment.content, mime_type,
                                              encoded_payload)) {
                        error = "invalid image attachment in summary source message " +
                                std::to_string(message.id);
                        return false;
                    }
                    source_document.content +=
                        "[image attachment in source message " +
                        std::to_string(message.id) + ": " + attachment.name + "]\n";
                    source_document.image_data_urls.push_back(attachment.content);
                } else if (attachment.content.size() <=
                           summary::kMaxAttachmentInlineBytes) {
                    source_document.content += "attached file: " + attachment.name +
                                               "\n```\n" + attachment.content + "\n```\n";
                } else {
                    source_document.content +=
                        "attached file: " + attachment.name + " (bounded preview)\n```\n" +
                        attachment.content.substr(0, summary::kMaxAttachmentInlineBytes) +
                        "\n[attachment truncated: " + attachment.name + ", " +
                        std::to_string(attachment.content.size()) + " bytes total]\n```\n";
                }
            }
            if (include_reasoning && message.role == "assistant") {
                const std::string inline_reasoning = extract_inline_thinking(message.content);
                if (!message.reasoning.empty() || !inline_reasoning.empty()) {
                    source_document.content +=
                        "[INTERNAL REASONING SOURCE MATERIAL, NOT AUTHORITATIVE, "
                        "source message id: " + std::to_string(message.id) + "]\n" +
                        message.reasoning + "\n";
                    if (!inline_reasoning.empty()) {
                        source_document.content +=
                            "[INLINE <think> SOURCE MATERIAL, NOT AUTHORITATIVE]\n" +
                            inline_reasoning + "\n";
                    }
                }
            }
        }
        source_document.content += "\nEND QUOTED SOURCE TRANSCRIPT\n";
        return true;
    }

bool Daemon::chunk_source_chain(const std::vector<StoredMessage>& source_chain, long long budget, std::vector<std::vector<StoredMessage>>& chunks, std::string& error)
{
        std::vector<long long> sizes;
        sizes.reserve(source_chain.size());
        for (const StoredMessage& message : source_chain) {
            provider::ChatMessage probe;
            std::string probe_error;
            if (!build_source_segment(std::vector<StoredMessage>{message}, "",
                                      probe, probe_error)) {
                error = probe_error;
                return false;
            }
            sizes.push_back(static_cast<long long>(probe.content.size()));
            if (sizes.back() > budget) {
                error = "summary source budget is smaller than a single message";
                return false;
            }
        }
        chunks.clear();
        std::vector<StoredMessage> current;
        long long current_bytes = 0;
        for (std::size_t i = 0; i < source_chain.size(); ++i) {
            if (!current.empty() && current_bytes + sizes[i] > budget) {
                chunks.push_back(std::move(current));
                current.clear();
                current_bytes = 0;
            }
            current.push_back(source_chain[i]);
            current_bytes += sizes[i];
        }
        if (!current.empty()) chunks.push_back(std::move(current));
        if (chunks.size() > summary::kMaxChunks) {
            error = "source history needs more than " +
                    std::to_string(summary::kMaxChunks) +
                    " summary chunks; use a selected fork";
            return false;
        }
        return true;
    }

void Daemon::build_summary_request(const provider::ChatMessage& source_document, bool chunked, std::vector<provider::ChatMessage>& history)
{
        std::string system = kSummarySystemPrompt;
        if (chunked) {
            system +=
                " The quoted transcript is one segment of a longer conversation; "
                "cite only source message ids present in this segment.";
        }
        history.push_back({"system", system});
        provider::ChatMessage request = source_document;
        request.content +=
            "Now produce the sectioned markdown checkpoint from the quoted "
            "source only.";
        history.push_back(std::move(request));
    }

void Daemon::build_merge_request(const std::vector<std::string>& segment_docs, std::vector<provider::ChatMessage>& history)
{
        history.push_back({"system", kSummaryMergeSystemPrompt});
        provider::ChatMessage request;
        request.role = "user";
        request.content = "BEGIN PRE-VALIDATED COMPACT CONTEXT SEGMENTS\n";
        for (std::size_t i = 0; i < segment_docs.size(); ++i) {
            request.content += "--- segment " + std::to_string(i + 1) + " of " +
                               std::to_string(segment_docs.size()) + " ---\n" +
                               segment_docs[i] + "\n";
        }
        request.content += "END PRE-VALIDATED COMPACT CONTEXT SEGMENTS\n";
        history.push_back(std::move(request));
    }

bool Daemon::stream_summary(const std::shared_ptr<Daemon::Session>& session, bool via_primary, const std::vector<provider::ChatMessage>& history, int chunk, int chunks, CancellationToken cancellation, std::string& out_text, std::string& error)
{
        const std::string sid = session->id;
        const std::string context_run_id = "context-" + sid;
        provider::Config config;
        if (via_primary) {
            config = provider_snapshot();
            config.tools.clear();  // a summary takes no tool calls
            if (config.max_tokens < 16384) config.max_tokens = 16384;
        } else {
            const auto auxiliary = auxiliary_snapshot();
            if (!auxiliary.has_value()) {
                error = "auxiliary provider is not configured";
                return false;
            }
            config = *auxiliary;
        }
        // One-shot auxiliary call: the legacy silence cap (the provider no
        // longer resolves 0 on its own).
        config.stall_timeout_ms = provider::kLegacyStallTimeoutMs;
        return (via_primary ? provider_.get() : auxiliary_provider_.get())->stream(
            config, history,
            [&](const std::string& type, const json::Value& payload) {
                if (cancellation.cancelled()) return;
                if (type == "text.delta") {
                    out_text += payload["text"].as_string();
                } else if (type == "metrics") {
                    publish(store_->append_event(
                        sid, context_run_id, "", "context.metrics",
                        {{"prompt_tokens",
                          std::to_string(payload["prompt_tokens"].as_integer())},
                         {"completion_tokens",
                          std::to_string(payload["completion_tokens"].as_integer())},
                         {"total_tokens",
                          std::to_string(payload["total_tokens"].as_integer())},
                         {"model", via_primary ? "primary" : "auxiliary"},
                         {"chunk", std::to_string(chunk)},
                         {"chunks", std::to_string(chunks)}}));
                }
            },
            cancellation, &error);
    }

bool Daemon::finish_context(const std::shared_ptr<Daemon::Session>& session, const std::string& status, const std::string& summary, const std::string& error)
{
        if (session->deleted.load(std::memory_order_acquire)) return false;
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            if (session->context_mode != "summary" ||
                session->context_status != "preparing") {
                return false;
            }
            session->context_status = status;
            session->context_summary = status == "ready" ? summary : "";
            session->context_error = status == "failed" ? error : "";
            persist_session_locked(session);
        }
        return true;
    }

bool Daemon::validated_summary_call(const std::shared_ptr<Daemon::Session>& session, bool via_primary, std::vector<provider::ChatMessage> history, int chunk, int chunks, const std::set<long long>& valid_ids, const std::set<long long>& required_ids, CancellationToken cancellation, summary::Document& out_doc, std::string& error)
{
        auto attempt = [&](std::string& raw, std::string& attempt_error) -> bool {
            std::string stream_error;
            const bool stream_ok = stream_summary(
                session, via_primary, history, chunk, chunks, cancellation,
                raw, stream_error);
            if (cancellation.cancelled()) {
                attempt_error = "cancelled";
                return false;
            }
            if (!stream_ok) {
                attempt_error = stream_error.empty()
                                   ? (via_primary
                                          ? "primary provider stream failed"
                                          : "auxiliary provider stream failed")
                                   : stream_error;
                return false;
            }
            // Some providers put reasoning in the visible content stream
            // instead of the reasoning field. Never persist or inject that
            // internal block into the compact context.
            summary::Document doc;
            if (!summary::parse_document(strip_inline_thinking(raw), doc,
                                         attempt_error)) {
                return false;
            }
            if (!summary::validate_provenance(doc, valid_ids, attempt_error)) {
                return false;
            }
            if (!required_ids.empty()) {
                std::set<long long> cited;
                collect_cited_ids(doc, cited);
                std::string missing;
                for (const long long id : required_ids) {
                    if (!cited.count(id)) {
                        if (!missing.empty()) missing += ", ";
                        missing += std::to_string(id);
                    }
                }
                if (!missing.empty()) {
                    attempt_error = "dropped required source citations: " + missing;
                    return false;
                }
            }
            out_doc = std::move(doc);
            return true;
        };

        std::string raw;
        std::string attempt_error;
        if (attempt(raw, attempt_error)) return true;
        if (cancellation.cancelled()) {
            error = attempt_error;
            return false;
        }

        // First attempt failed validation. Feed the rejected output back with
        // the specific error for one corrective call.
        history.push_back({"assistant", strip_inline_thinking(raw)});
        history.push_back({"user",
            "Your previous reply was rejected: " + attempt_error + ". Return "
            "the corrected sectioned markdown checkpoint now, with the "
            "sections ## Goal, ## Decisions, ## Constraints, ## Unresolved "
            "Tasks, ## Relevant Artifacts, ## Next Steps. One concise bullet "
            "per item, or (none) for an empty section. Citations as [source "
            "message id: N] are optional. No JSON, no code fence."});
        std::string repair_raw;
        std::string repair_error;
        if (attempt(repair_raw, repair_error)) return true;
        error = "summary rejected after repair: " + repair_error;
        return false;
    }

Daemon::SummaryFallbackChoice Daemon::prompt_summary_fallback( const std::shared_ptr<Daemon::Session>& session, const std::string& reason, CancellationToken cancellation)
{
        const std::string sid = session->id;
        const std::string context_run_id = "context-" + sid;
        const std::string prompt =
            "Auxiliary model unavailable for compaction (" + reason +
            "). Use the primary model? (primary / deterministic / retry)";
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            session->context_prompt = prompt;
            session->context_decision.clear();
        }
        publish(store_->append_event(
            sid, context_run_id, "", "context.summary_prompt",
            {{"prompt", prompt}}));
        std::unique_lock<std::mutex> lock(session->context_mutex);
        session->context_cv.wait_for(
            lock, std::chrono::milliseconds(kSummaryPromptTimeoutMs), [&] {
                return !session->context_decision.empty() ||
                       cancellation.cancelled() ||
                       session->deleted.load(std::memory_order_acquire);
            });
        std::string decision;
        bool by_operator = false;
        {
            decision = session->context_decision;
            by_operator = !decision.empty();
            session->context_prompt.clear();
            session->context_decision.clear();
        }
        if (decision.empty()) decision = "deterministic";
        publish(store_->append_event(
            sid, context_run_id, "", "context.summary_resolved",
            {{"decision", decision},
             {"source", by_operator ? "operator" : "timeout"}}));
        session->context_cv.notify_all();
        if (decision == "primary") return SummaryFallbackChoice::Primary;
        if (decision == "retry") return SummaryFallbackChoice::Retry;
        return SummaryFallbackChoice::Deterministic;
    }

bool Daemon::validated_summary_chain(const std::shared_ptr<Daemon::Session>& session, const std::vector<provider::ChatMessage>& history, int chunk, int chunks, const std::set<long long>& valid_ids, const std::set<long long>& required_ids, CancellationToken cancellation, summary::Document& out_doc, std::string& error)
{
        std::string aux_error;
        if (validated_summary_call(session, false, history, chunk, chunks,
                                   valid_ids, required_ids, cancellation,
                                   out_doc, aux_error)) {
            return true;
        }
        if (cancellation.cancelled()) {
            error = aux_error;
            return false;
        }
        log::warn("auxiliary summary failed session=" + session->id +
                  ": " + aux_error);
        const auto choice =
            prompt_summary_fallback(session, aux_error, cancellation);
        if (choice == SummaryFallbackChoice::Retry) {
            if (validated_summary_call(session, false, history, chunk, chunks,
                                       valid_ids, required_ids, cancellation,
                                       out_doc, aux_error)) {
                return true;
            }
            if (cancellation.cancelled()) {
                error = aux_error;
                return false;
            }
            error = "auxiliary summary failed after retry: " + aux_error;
            return false;
        }
        if (choice == SummaryFallbackChoice::Primary) {
            std::string primary_error;
            if (validated_summary_call(session, true, history, chunk, chunks,
                                       valid_ids, required_ids, cancellation,
                                       out_doc, primary_error)) {
                return true;
            }
            if (cancellation.cancelled()) {
                error = primary_error;
                return false;
            }
            log::warn("primary summary rejected session=" + session->id +
                      ": " + primary_error);
            error = "auxiliary: " + aux_error + "; primary: " + primary_error;
            return false;
        }
        error = aux_error;
        return false;
    }

bool Daemon::deterministic_fallback_document( const std::vector<StoredMessage>& source_chain, long long doc_budget, summary::Document& out, std::string& error)
{
        if (source_chain.empty()) {
            error = "no source messages to summarize";
            return false;
        }
        const long long tail_budget = doc_budget > 0
            ? std::min<long long>(static_cast<long long>(kFallbackTailBytes),
                                  doc_budget)
            : static_cast<long long>(kFallbackTailBytes);
        std::size_t tail_bytes = 0;
        for (auto it = source_chain.rbegin(); it != source_chain.rend(); ++it) {
            if (out.relevant_artifacts.size() >= kFallbackMaxTurns) break;
            const StoredMessage& message = *it;
            const std::string visible = message.role == "assistant"
                ? strip_inline_thinking(message.content)
                : message.content;
            if (visible.empty()) continue;
            std::string text = visible;
            if (text.size() > summary::kMaxItemBytes) {
                text = text.substr(0, summary::kMaxItemBytes) + " [truncated]";
            }
            if (tail_bytes + static_cast<long long>(text.size()) > tail_budget &&
                !out.relevant_artifacts.empty()) {
                break;
            }
            out.relevant_artifacts.push_back(
                "[" + message.role + " | source message id: " +
                std::to_string(message.id) + "]\n" + text);
            tail_bytes += text.size();
        }
        if (out.relevant_artifacts.empty()) {
            error = "no source content to summarize";
            return false;
        }
        for (auto it = source_chain.rbegin(); it != source_chain.rend(); ++it) {
            if (it->role == "user" && !it->content.empty()) {
                const std::string& request = it->content;
                out.latest_user_request = request.size() >
                                                       kMaxLatestUserRequestBytes
                    ? request.substr(0, kMaxLatestUserRequestBytes)
                    : request;
                break;
            }
        }
        return true;
    }

void Daemon::prepare_summary(const std::shared_ptr<Daemon::Session>& session, std::vector<StoredMessage> source_chain, CancellationToken cancellation)
{
        const std::string sid = session->id;
        const std::string context_run_id = "context-" + sid;
        if (cancellation.cancelled()) return;
        log::info("summary preparation started session=" + sid + " messages=" +
                  std::to_string(source_chain.size()));

        auto fail = [&](const std::string& error) {
            log::error("summary failed session=" + sid + ": " + error);
            if (finish_context(session, "failed", "", error)) {
                publish(store_->append_event(
                    sid, context_run_id, "", "context.failed",
                    {{"error", error},
                     {"source_message_count", std::to_string(source_chain.size())}}));
                session->context_cv.notify_all();
            }
        };

        // Deterministic fallback: the auxiliary model is unavailable or
        // rejected its output, so the compact context is built mechanically
        // from the source transcript. The fork still reaches "ready" with no
        // model in the loop; only an empty source still fails.
        auto fallback = [&](const std::string& reason) {
            if (cancellation.cancelled()) return;
            log::warn("summary fallback session=" + sid + ": " + reason);
            summary::Document doc;
            std::string fallback_error;
            if (!deterministic_fallback_document(source_chain,
                                                 mechanical_document_budget(),
                                                 doc, fallback_error)) {
                fail(fallback_error);
                return;
            }
            std::string transcript;
            for (const StoredMessage& message : source_chain) {
                transcript += message.content;
                transcript += "\n";
                for (const StoredAttachment& attachment :
                     store_->attachments_for_message(message.id)) {
                    transcript += attachment.content;
                    transcript += "\n";
                }
            }
            summary::harvest_anchors(transcript, doc.anchors);
            std::string final_document = summary::canonicalize(doc);
            if (finish_context(session, "ready", final_document, "")) {
                publish(store_->append_event(
                    sid, context_run_id, "", "context.fallback",
                    {{"reason", reason},
                     {"summary_length", std::to_string(final_document.size())},
                     {"source_message_count", std::to_string(source_chain.size())}}));
                session->context_cv.notify_all();
            }
        };

        std::string preparation_error;
        provider::ChatMessage full_document;
        if (!build_source_segment(source_chain, "BEGIN QUOTED SOURCE TRANSCRIPT\n",
                                  full_document, preparation_error)) {
            fallback(preparation_error);
            return;
        }
        if (cancellation.cancelled()) return;

        std::set<long long> valid_ids;
        for (const StoredMessage& message : source_chain) valid_ids.insert(message.id);

        std::vector<std::vector<StoredMessage>> chunks;
        if (static_cast<long long>(full_document.content.size()) <=
            summary_budget_bytes()) {
            chunks.push_back(source_chain);
        } else if (!chunk_source_chain(source_chain, summary_budget_bytes(),
                                       chunks, preparation_error)) {
            fallback(preparation_error);
            return;
        }
        const int total_chunks = static_cast<int>(chunks.size());

        std::vector<std::string> segment_docs;
        std::vector<summary::Document> segment_documents;
        segment_docs.reserve(chunks.size());
        segment_documents.reserve(chunks.size());
        summary::Document single_doc;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            if (cancellation.cancelled()) return;
            provider::ChatMessage segment;
            if (!build_source_segment(
                    chunks[i], "BEGIN QUOTED SOURCE TRANSCRIPT SEGMENT\n", segment,
                    preparation_error)) {
                fallback(preparation_error);
                return;
            }
            std::vector<provider::ChatMessage> history;
            build_summary_request(segment, total_chunks > 1, history);
            summary::Document segment_doc;
            log::debug("summary call session=" + sid + " chunk=" +
                       std::to_string(i + 1) + "/" + std::to_string(total_chunks));
            if (!validated_summary_chain(
                    session, history, static_cast<int>(i) + 1,
                    total_chunks, valid_ids, {}, cancellation, segment_doc,
                    preparation_error)) {
                fallback(preparation_error);
                return;
            }
            if (total_chunks == 1) single_doc = segment_doc;
            const std::string segment_document = summary::canonicalize(segment_doc);
            segment_docs.push_back(segment_document);
            segment_documents.push_back(segment_doc);
            if (total_chunks > 1) {
                // Audit trail: every validated segment checkpoint is retained
                // in the event ledger, so a merge can be inspected against
                // its pre-validated inputs (and the mechanical merge's
                // inputs are reconstructable).
                publish(store_->append_event(
                    sid, context_run_id, "", "context.segment",
                    {{"index", std::to_string(i + 1)},
                     {"chunks", std::to_string(total_chunks)},
                     {"length", std::to_string(segment_document.size())},
                     {"document", segment_document}}));
            }
        }

        summary::Document final_doc;
        bool mechanical_merge = false;
        if (total_chunks == 1) {
            final_doc = std::move(single_doc);
        } else {
            // Merge coverage: every citation the pre-validated segments
            // carried must survive the merge. The merge prompt states the
            // contract; this enforces it deterministically.
            std::set<long long> required_ids;
            for (const summary::Document& segment : segment_documents) {
                collect_cited_ids(segment, required_ids);
            }
            std::vector<provider::ChatMessage> history;
            build_merge_request(segment_docs, history);
            log::debug("summary merge call session=" + sid + " chunks=" +
                       std::to_string(total_chunks) + " required_citations=" +
                       std::to_string(required_ids.size()));
            if (validated_summary_chain(
                    session, history, total_chunks + 1,
                    total_chunks + 1, valid_ids, required_ids, cancellation,
                    final_doc, preparation_error)) {
                // model merge covered every segment citation
            } else if (cancellation.cancelled()) {
                return;
            } else {
                // Deterministic merge of the validated segments: nothing is
                // fabricated and no segment's content is lost.
                summary::Document mechanical;
                std::string mechanical_error;
                if (!mechanical_merge_document(segment_documents,
                                               mechanical_document_budget(),
                                               mechanical, mechanical_error)) {
                    fallback("merged summary: " + preparation_error +
                             "; mechanical merge: " + mechanical_error);
                    return;
                }
                log::warn("summary merge fell back to mechanical segment "
                          "merge session=" + sid + ": " + preparation_error);
                final_doc = std::move(mechanical);
                mechanical_merge = true;
            }
        }

        // Deterministic enrichment: provenance the harness can guarantee
        // without trusting the auxiliary model. Anchors are verbatim URLs and
        // paths harvested from the source transcript; latest_user_request is
        // the verbatim most recent user turn.
        summary::harvest_anchors(full_document.content, final_doc.anchors);
        for (auto it = source_chain.rbegin(); it != source_chain.rend(); ++it) {
            if (it->role == "user" && !it->content.empty()) {
                const std::string& request = it->content;
                final_doc.latest_user_request = request.size() >
                                                       kMaxLatestUserRequestBytes
                    ? request.substr(0, kMaxLatestUserRequestBytes)
                    : request;
                break;
            }
        }
        std::string final_document = summary::canonicalize(final_doc);
        log::info("summary ready session=" + sid + " length=" +
                  std::to_string(final_document.size()) + " chunks=" +
                  std::to_string(total_chunks));

        if (finish_context(session, "ready", final_document, "")) {
            publish(store_->append_event(
                sid, context_run_id, "", "context.completed",
                {{"summary_length", std::to_string(final_document.size())},
                 {"source_message_count", std::to_string(source_chain.size())},
                 {"chunks", std::to_string(total_chunks)},
                 {"mechanical_merge", mechanical_merge ? "true" : "false"}}));
            session->context_cv.notify_all();
        }
    }

void Daemon::start_summary_job(const std::shared_ptr<Daemon::Session>& session, const std::vector<StoredMessage>& source_chain)
{
        const CancellationToken cancellation = session->context_cancellation.token();
        std::thread([this, session, source_chain, cancellation]() mutable {
            run_guarded("summary job", [&] { prepare_summary(session, source_chain, cancellation); });
        }).detach();
    }

void Daemon::collect_cited_ids(const summary::Document& doc, std::set<long long>& out)
{
        auto scan = [&out](const std::string& item) {
            static const char* marker = "[source message id: ";
            const std::size_t marker_size = std::strlen(marker);
            std::size_t cursor = 0;
            while ((cursor = item.find(marker, cursor)) != std::string::npos) {
                const std::size_t start = cursor + marker_size;
                const std::size_t end = item.find(']', start);
                if (end == std::string::npos || end == start) {
                    cursor += 1;
                    continue;
                }
                bool digits = true;
                for (std::size_t i = start; i < end; ++i) {
                    if (!std::isdigit(static_cast<unsigned char>(item[i]))) {
                        digits = false;
                        break;
                    }
                }
                if (digits) {
                    try {
                        out.insert(std::stoll(item.substr(start, end - start)));
                    } catch (...) {
                    }
                }
                cursor = end + 1;
            }
        };
        scan(doc.goal);
        for (const std::string& item : doc.decisions) scan(item);
        for (const std::string& item : doc.constraints) scan(item);
        for (const std::string& item : doc.unresolved_tasks) scan(item);
        for (const std::string& item : doc.relevant_artifacts) scan(item);
        for (const std::string& item : doc.next_steps) scan(item);
    }

bool Daemon::build_recompress_request(const summary::Document& checkpoint, const std::vector<StoredMessage>& tail, std::vector<provider::ChatMessage>& history, std::string& error)
{
        history.push_back({"system", kRecompressSystemPrompt});
        provider::ChatMessage request;
        request.role = "user";
        request.content = "BEGIN PRE-VALIDATED CHECKPOINT\n";
        request.content += summary::render_readable(checkpoint);
        request.content += "\nEND PRE-VALIDATED CHECKPOINT\n";
        provider::ChatMessage tail_segment;
        if (!build_source_segment(tail, "BEGIN NEW CONVERSATION TURNS\n",
                                  tail_segment, error)) {
            return false;
        }
        request.content += tail_segment.content;
        request.image_data_urls = std::move(tail_segment.image_data_urls);
        request.content +=
            "Now merge the checkpoint with the new conversation turns and "
            "return the updated sectioned markdown checkpoint.";
        history.push_back(std::move(request));
        return true;
    }

bool Daemon::mechanical_recompress_document(const summary::Document& old_doc, const std::vector<StoredMessage>& tail, long long doc_budget, summary::Document& out, std::string& error)
{
        if (tail.empty()) {
            error = "no new turns to merge";
            return false;
        }
        out = old_doc;
        // The merged document must still fit the schema bounds (and the
        // window, when known); cap the new tail at what the old document
        // leaves free.
        long long cap = static_cast<long long>(summary::kMaxDocumentBytes);
        if (doc_budget > 0) cap = std::min(cap, doc_budget);
        long long headroom =
            cap - static_cast<long long>(summary::canonicalize(old_doc).size());
        if (headroom < 0) headroom = 0;
        const long long tail_budget =
            std::min<long long>(static_cast<long long>(kFallbackTailBytes),
                                headroom);
        std::size_t tail_bytes = 0;
        for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
            if (out.relevant_artifacts.size() >= summary::kMaxItemsPerField) break;
            if (out.relevant_artifacts.size() >= kFallbackMaxTurns) break;
            const StoredMessage& message = *it;
            const std::string visible = message.role == "assistant"
                ? strip_inline_thinking(message.content)
                : message.content;
            if (visible.empty()) continue;
            std::string text = visible;
            if (text.size() > summary::kMaxItemBytes) {
                text = text.substr(0, summary::kMaxItemBytes) + " [truncated]";
            }
            if (tail_bytes + text.size() > tail_budget &&
                !out.relevant_artifacts.empty()) {
                break;
            }
            out.relevant_artifacts.push_back(
                "[" + message.role + " | source message id: " +
                std::to_string(message.id) + "]\n" + text);
            tail_bytes += text.size();
        }
        for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
            if (it->role == "user" && !it->content.empty()) {
                const std::string& request = it->content;
                out.latest_user_request = request.size() >
                                                       kMaxLatestUserRequestBytes
                    ? request.substr(0, kMaxLatestUserRequestBytes)
                    : request;
                break;
            }
        }
        return true;
    }

bool Daemon::mechanical_merge_document(const std::vector<summary::Document>& segments, long long doc_budget, summary::Document& out, std::string& error)
{
        if (segments.empty()) {
            error = "no segments to merge";
            return false;
        }
        out = summary::Document();
        for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
            if (!it->goal.empty()) {
                out.goal = it->goal;
                break;
            }
        }
        auto append_unique = [](std::vector<std::string>& target,
                                const std::string& item) {
            if (target.size() >= summary::kMaxItemsPerField) return;
            if (std::find(target.begin(), target.end(), item) == target.end()) {
                target.push_back(item);
            }
        };
        for (const summary::Document& segment : segments) {
            for (const std::string& item : segment.decisions) append_unique(out.decisions, item);
            for (const std::string& item : segment.constraints) append_unique(out.constraints, item);
            for (const std::string& item : segment.unresolved_tasks) append_unique(out.unresolved_tasks, item);
            for (const std::string& item : segment.relevant_artifacts) append_unique(out.relevant_artifacts, item);
            for (const std::string& item : segment.next_steps) append_unique(out.next_steps, item);
        }
        long long cap = static_cast<long long>(summary::kMaxDocumentBytes);
        if (doc_budget > 0) cap = std::min(cap, doc_budget);
        std::string canonical = summary::canonicalize(out);
        if (canonical.size() > cap) {
            // Drop the oldest artifacts (front of the list) until the merged
            // document fits the bound; a single item always fits.
            while (canonical.size() > cap &&
                   out.relevant_artifacts.size() > 1) {
                out.relevant_artifacts.erase(out.relevant_artifacts.begin());
                canonical = summary::canonicalize(out);
            }
            if (canonical.size() > cap) {
                error = "mechanical merge exceeds the document bound";
                return false;
            }
        }
        return true;
    }

bool Daemon::finish_recompression(const std::shared_ptr<Daemon::Session>& session, const std::string& summary, long long watermark)
{
        if (session->deleted.load(std::memory_order_acquire)) return false;
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            if (session->context_mode != "summary" ||
                session->context_status != "preparing") {
                return false;
            }
            session->context_status = "ready";
            session->context_summary = summary;
            session->context_error.clear();
            session->context_watermark = watermark;
            persist_session_locked(session);
        }
        return true;
    }

void Daemon::prepare_recompression(const std::shared_ptr<Daemon::Session>& session, std::vector<StoredMessage> tail, CancellationToken cancellation)
{
        const std::string sid = session->id;
        const std::string context_run_id = "context-" + sid;
        if (cancellation.cancelled()) return;
        log::info("recompression started session=" + sid +
                  " tail_messages=" + std::to_string(tail.size()));

        // A failed re-compression must not degrade the session: the previous
        // checkpoint still covers the conversation up to the old watermark, so
        // it stays in place and the session returns to ready.
        auto abort = [&](const std::string& error) {
            log::error("recompression failed session=" + sid + ": " + error);
            {
                std::lock_guard<std::mutex> lock(session->context_mutex);
                if (session->context_mode != "summary" ||
                    session->context_status != "preparing") {
                    return;
                }
                session->context_status = "ready";
                session->context_error.clear();
                persist_session_locked(session);
            }
            publish(store_->append_event(
                sid, context_run_id, "", "context.recompress_failed",
                {{"error", error}}));
            session->context_cv.notify_all();
        };

        summary::Document old_doc;
        std::string parse_error;
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            if (!summary::parse_document(session->context_summary, old_doc,
                                         parse_error)) {
                abort("stored checkpoint does not parse: " + parse_error);
                return;
            }
        }
        if (cancellation.cancelled()) return;

        // Valid provenance: everything the checkpoint already cites stays
        // valid, plus the new turns the model may cite. The checkpoint's own
        // citations are required: the re-compression contract preserves them.
        std::set<long long> valid_ids;
        std::set<long long> required_ids;
        collect_cited_ids(old_doc, required_ids);
        valid_ids = required_ids;
        for (const StoredMessage& message : tail) valid_ids.insert(message.id);

        std::vector<provider::ChatMessage> history;
        std::string preparation_error;
        summary::Document merged;
        std::string auxiliary_error;
        bool auxiliary_ok = false;
        if (build_recompress_request(old_doc, tail, history, preparation_error)) {
            auxiliary_ok = validated_summary_chain(
                session, history, 1, 1, valid_ids, required_ids,
                cancellation, merged, auxiliary_error);
        } else {
            // The auxiliary is unavailable (not configured): the mechanical
            // merge below does not need it, so fall through instead of
            // aborting. Mirrors prepare_summary's deterministic fallback, so
            // a re-compression never hard-fails when the aux model is down.
            auxiliary_error = preparation_error;
        }

        std::string transcript;
        for (const StoredMessage& message : tail) {
            transcript += message.content;
            transcript += "\n";
        }

        if (!auxiliary_ok) {
            if (cancellation.cancelled()) return;
            // Mechanical merge fallback: keep the checkpoint's semantic
            // fields, slide the new turns in verbatim, no model in the loop.
            summary::Document mechanical;
            std::string mechanical_error;
            if (!mechanical_recompress_document(old_doc, tail,
                                                mechanical_document_budget(),
                                                mechanical, mechanical_error)) {
                abort(mechanical_error);
                return;
            }
            summary::harvest_anchors(transcript, mechanical.anchors);
            std::string final_document = summary::canonicalize(mechanical);
            if (finish_recompression(session, final_document, tail.back().id)) {
                publish(store_->append_event(
                    sid, context_run_id, "", "context.fallback",
                    {{"reason", auxiliary_error},
                     {"recompressed", "true"},
                     {"summary_length", std::to_string(final_document.size())},
                     {"tail_message_count",
                      std::to_string(tail.size())}}));
                // Story checkpoint: the session just crossed a re-compression
                // boundary - a natural place to update its journey (durable
                // job, deduped per coverage window).
                if (memory_custom_enabled()) enqueue_journey_job(sid);
                session->context_cv.notify_all();
            }
            return;
        }

        // Deterministic enrichment from the new turns, mirroring the initial
        // summary path: verbatim anchors and the latest user request.
        summary::harvest_anchors(transcript, merged.anchors);
        for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
            if (it->role == "user" && !it->content.empty()) {
                const std::string& request = it->content;
                merged.latest_user_request = request.size() >
                                                           kMaxLatestUserRequestBytes
                    ? request.substr(0, kMaxLatestUserRequestBytes)
                    : request;
                break;
            }
        }
        std::string final_document = summary::canonicalize(merged);
        log::info("recompression ready session=" + sid +
                  " length=" + std::to_string(final_document.size()) +
                  " tail_messages=" + std::to_string(tail.size()));
        if (finish_recompression(session, final_document, tail.back().id)) {
            publish(store_->append_event(
                sid, context_run_id, "", "context.recompressed",
                {{"summary_length", std::to_string(final_document.size())},
                 {"tail_message_count", std::to_string(tail.size())}}));
            // Story checkpoint: the session just crossed a re-compression
            // boundary - a natural place to update its journey (durable job,
            // deduped per coverage window).
            if (memory_custom_enabled()) enqueue_journey_job(sid);
            session->context_cv.notify_all();
        }
    }

void Daemon::start_recompression_job(const std::shared_ptr<Daemon::Session>& session, std::vector<StoredMessage> tail)
{
        const CancellationToken cancellation = session->context_cancellation.token();
        std::thread([this, session, tail, cancellation]() mutable {
            run_guarded("recompression job", [&] { prepare_recompression(session, std::move(tail), cancellation); });
        }).detach();
    }

void Daemon::manual_recompress(const std::shared_ptr<Daemon::Session>& session, http::Response& response)
{
        const std::string sid = session->id;
        std::string mode, status, summary;
        long long watermark = 0;
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            mode = session->context_mode;
            status = session->context_status;
            summary = session->context_summary;
            watermark = session->context_watermark;
        }
        if (mode != "summary") {
            response.status = 409;
            response.reason = "Conflict";
            response.body =
                "{\"error\":\"recompress requires summary context mode\"}";
            return;
        }
        if (status == "preparing") {
            response.status = 409;
            response.reason = "Conflict";
            response.body = "{\"error\":\"context is already preparing\"}";
            return;
        }
        if (status != "ready" || summary.empty()) {
            response.status = 409;
            response.reason = "Conflict";
            response.body =
                "{\"error\":\"no ready summary to re-compress\"}";
            return;
        }
        // The delta is the active path after the coverage watermark.
        std::vector<StoredMessage> tail;
        long long cursor = store_->active_leaf_id(sid);
        for (int guard = 0; guard < 10000 && cursor > 0; ++guard) {
            const auto message = store_->get_message(cursor);
            if (!message) break;
            if (message->id > watermark) tail.push_back(*message);
            cursor = message->parent_id;
        }
        std::reverse(tail.begin(), tail.end());
        long long tail_bytes = 0;
        for (const StoredMessage& message : tail) {
            tail_bytes += static_cast<long long>(message.content.size());
        }
        const bool below_threshold =
            tail.size() < kRecompressMinTailMessages ||
            tail_bytes < kRecompressMinTailBytes;
        json::Value result(json::Value::Type::Object);
        result["ok"] = json::Value(true);
        result["watermark"] = json::Value(watermark);
        result["tail_messages"] = json::Value(static_cast<long long>(tail.size()));
        result["tail_bytes"] = json::Value(tail_bytes);
        if (below_threshold) {
            // Cooldown: the delta is too small to be worth an auxiliary call;
            // reuse the existing checkpoint untouched.
            publish(store_->append_event(
                sid, "context-" + sid, "", "context.reused",
                {{"tail_message_count", std::to_string(tail.size())},
                 {"tail_bytes", std::to_string(tail_bytes)}}));
            result["status"] = json::Value("reused");
            result["context_status"] = json::Value("ready");
            response.body = json::stringify(result);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            session->context_status = "preparing";
            persist_session_locked(session);
        }
        publish(store_->append_event(
            sid, "context-" + sid, "", "context.recompressing",
            {{"tail_message_count", std::to_string(tail.size())},
             {"tail_bytes", std::to_string(tail_bytes)}}));
        result["status"] = json::Value("recompressing");
        result["context_status"] = json::Value("preparing");
        response.body = json::stringify(result);
        start_recompression_job(session, std::move(tail));
    }

http::Response Daemon::recompress_context(const http::Request& request)
{
        http::Response response;
        response.headers["content-type"] = "application/json";
        if (!authorized(bearer_token(request))) {
            response.status = 401;
            response.reason = "Unauthorized";
            response.body = "{\"error\":\"unauthorized\"}";
            return response;
        }
        const std::string sid = session_id_from_path(request.path);
        auto session = find_session(sid);
        if (!session) {
            response.status = 404;
            response.reason = "Not Found";
            response.body = "{\"error\":\"session not found\"}";
            return response;
        }
        manual_recompress(session, response);
        return response;
    }

http::Response Daemon::compact_context(const http::Request& request)
{
        http::Response response;
        response.headers["content-type"] = "application/json";
        if (!authorized(bearer_token(request))) {
            response.status = 401;
            response.reason = "Unauthorized";
            response.body = "{\"error\":\"unauthorized\"}";
            return response;
        }
        const std::string sid = session_id_from_path(request.path);
        auto session = find_session(sid);
        if (!session) {
            response.status = 404;
            response.reason = "Not Found";
            response.body = "{\"error\":\"session not found\"}";
            return response;
        }
        {
            auto slot = session_slot(sid);
            std::lock_guard<std::mutex> lock(slot->mutex);
            if (slot->active != 0 || !slot->waiters.empty()) {
                response.status = 409;
                response.reason = "Conflict";
                response.body =
                    "{\"error\":\"a run is in progress; wait for it to finish\"}";
                return response;
            }
        }
        std::string mode;
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            mode = session->context_mode;
        }
        if (mode == "summary") {
            manual_recompress(session, response);
            return response;
        }
        // Selected mode: compact the stored chain's head into a checkpoint.
        std::vector<StoredMessage> chain;
        long long cursor = store_->active_leaf_id(sid);
        for (int guard = 0; guard < 10000 && cursor > 0; ++guard) {
            const auto message = store_->get_message(cursor);
            if (!message) break;
            chain.push_back(*message);
            cursor = message->parent_id;
        }
        std::reverse(chain.begin(), chain.end());
        long long chain_bytes = 0;
        for (const StoredMessage& message : chain) {
            chain_bytes += static_cast<long long>(message.content.size());
        }
        // Conservative 2 bytes/token (the estimator's fallback rate): the
        // value only labels the ledger event.
        const long long estimated = (chain_bytes + 1) / 2;
        std::string detail;
        auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
        const bool compacted = compact_chain_to_checkpoint(
            session, provider_snapshot(), std::move(chain), estimated,
            CancellationToken(cancel_flag), detail);
        json::Value result(json::Value::Type::Object);
        result["ok"] = json::Value(true);
        result["status"] = json::Value(compacted ? "compacted" : "skipped");
        if (compacted) {
            result["context_mode"] = json::Value("summary");
        } else {
            result["detail"] = json::Value(detail);
        }
        response.body = json::stringify(result);
        return response;
    }

http::Response Daemon::context_decision_route(const http::Request& request)
{
        http::Response response;
        response.headers["content-type"] = "application/json";
        if (!authorized(bearer_token(request))) {
            response.status = 401;
            response.reason = "Unauthorized";
            response.body = "{\"error\":\"unauthorized\"}";
            return response;
        }
        const std::string sid = session_id_from_path(request.path);
        auto session = find_session(sid);
        if (!session) {
            response.status = 404;
            response.reason = "Not Found";
            response.body = "{\"error\":\"session not found\"}";
            return response;
        }
        json::Value body;
        try {
            body = json::parse(request.body);
        } catch (...) {
            body = json::Value(json::Value::Type::Object);
        }
        const std::string decision = body["decision"].as_string();
        if (decision != "primary" && decision != "deterministic" &&
            decision != "retry") {
            response.status = 400;
            response.reason = "Bad Request";
            response.body =
                "{\"error\":\"decision must be 'primary', 'deterministic', "
                "or 'retry'\"}";
            return response;
        }
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            if (session->context_prompt.empty()) {
                response.status = 409;
                response.reason = "Conflict";
                response.body =
                    "{\"error\":\"no pending compaction prompt\"}";
                return response;
            }
            session->context_decision = decision;
        }
        session->context_cv.notify_all();
        response.body = "{\"ok\":true}";
        return response;
    }

http::Response Daemon::update_context(const http::Request& request)
{
        http::Response response;
        response.headers["content-type"] = "application/json";
        if (!authorized(bearer_token(request))) {
            response.status = 401;
            response.reason = "Unauthorized";
            response.body = "{\"error\":\"unauthorized\"}";
            return response;
        }
        const std::string sid = session_id_from_path(request.path);
        auto session = find_session(sid);
        if (!session) {
            response.status = 404;
            response.reason = "Not Found";
            response.body = "{\"error\":\"session not found\"}";
            return response;
        }
        json::Value body;
        try {
            body = json::parse(request.body);
        } catch (...) {
            body = json::Value(json::Value::Type::Object);
        }
        if (body["mode"].as_string() != "selected") {
            response.status = 400;
            response.reason = "Bad Request";
            response.body = "{\"error\":\"context mode must be 'selected'\"}";
            return response;
        }
        {
            std::lock_guard<std::mutex> lock(session->context_mutex);
            if (session->context_mode == "summary") {
                // Summary forks deliberately copy no transcript. Treating a
                // queued user message as selected context would silently run
                // against the wrong branch, so require a future selected-path
                // recovery operation instead of inventing one here.
                response.status = 409;
                response.reason = "Conflict";
                response.body =
                    "{\"error\":\"summary branch has no copied selected context; "
                    "create a selected fork with a source path\"}";
                return response;
            }
            session->context_mode = "selected";
            session->context_status = "ready";
            session->context_summary.clear();
            session->context_error.clear();
            session->source_session_id.clear();
            persist_session_locked(session);
        }
        session->context_cancellation.cancel();
        session->context_cv.notify_all();
        json::Value result(json::Value::Type::Object);
        result["ok"] = json::Value(true);
        result["context_mode"] = json::Value("selected");
        result["context_status"] = json::Value("ready");
        result["context_error"] = json::Value("");
        response.body = json::stringify(result);
        return response;
    }

}  // namespace nyaxus
