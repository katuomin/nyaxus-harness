// daemon_http_util.cpp - the daemon's HTTP/web helpers (split from
// daemon.cpp; see docs/daemon-split-analysis.md). Declarations live in
// include/nyaxus/daemon.hpp; this file carries only the definitions. The
// tuning constants and run_guarded moved to the header.
#include <cctype>
#include <cstring>
#include <exception>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "nyaxus/daemon.hpp"
#include "nyaxus/http.hpp"
#include "nyaxus/log.hpp"

namespace nyaxus {

namespace {
// Private helpers for the HTML/URL/base64 parsers below (grouped here;
// they kept their original order and bodies). lower_ascii and trim_ascii
// are shared with daemon.cpp, so they live inline in daemon.hpp.
int base64_value(char character) {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+') return 62;
    if (character == '/') return 63;
    return -1;
}

// Decode the HTML entities that actually appear in page text. Two passes so
// double-escaped sequences (&amp;lt;) resolve too.
void decode_entities(std::string& text) {
    static const std::pair<const char*, const char*> kEntities[] = {
        {"&amp;", "&"},
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&quot;", "\""},
        {"&#39;", "'"},
        {"&apos;", "'"},
        {"&nbsp;", " "},
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& [entity, replacement] : kEntities) {
            const std::size_t entity_len = std::strlen(entity);
            const std::size_t replacement_len = std::strlen(replacement);
            std::size_t pos = 0;
            while ((pos = text.find(entity, pos)) != std::string::npos) {
                text.replace(pos, entity_len, replacement);
                pos += replacement_len;
            }
        }
    }
}

// True when text[pos] starts the tag <name ...> (a real tag boundary, not a
// prefix of a longer tag name like <head> vs <header>).
bool html_tag_at(const std::string& text, std::size_t pos, const char* tag) {
    if (text.compare(pos, 1, "<") != 0) return false;
    const std::size_t len = std::strlen(tag);
    if (text.compare(pos + 1, len, tag) != 0) return false;
    const char next = text[pos + 1 + len];
    return next == '>' || next == ' ' || next == '\t' || next == '\n' ||
           next == '\r' || next == '/';
}

// Remove every <name ...> ... </name> block. A manual scan rather than a
// regex with a back-reference: MSVC's std::regex does not support them.
void strip_tag_block(std::string& text, const char* tag, const char* close_tag) {
    const std::string needle = std::string("<") + tag;
    const std::string close = std::string("</") + close_tag + ">";
    std::size_t search_from = 0;
    while (search_from < text.size()) {
        const std::size_t pos = text.find(needle, search_from);
        if (pos == std::string::npos) return;
        if (!html_tag_at(text, pos, tag)) {
            search_from = pos + 1;
            continue;
        }
        const std::size_t end = text.find(close, pos + needle.size());
        if (end == std::string::npos) {
            search_from = pos + needle.size();
            continue;
        }
        text.erase(pos, (end - pos) + close.size());
        // Continue scanning from the erase point (the following content slid
        // back to `pos`).
    }
}

}  // namespace

std::string bearer_token(const http::Request& request) {
    const auto it = request.headers.find("authorization");
    if (it == request.headers.end()) return "";
    const std::string& value = it->second;
    if (value.rfind("Bearer ", 0) == 0) return value.substr(7);
    return "";
}

bool decode_base64(const std::string& encoded, std::string& decoded) {
    if (encoded.empty() || encoded.size() % 4 == 1) return false;
    const std::size_t first_padding = encoded.find('=');
    if (first_padding != std::string::npos) {
        if (encoded.size() % 4 != 0 || encoded.size() - first_padding > 2) return false;
        for (std::size_t i = first_padding; i < encoded.size(); ++i) {
            if (encoded[i] != '=') return false;
        }
    }

    decoded.clear();
    decoded.reserve((encoded.size() * 3) / 4);
    int accumulator = 0;
    int bits = 0;
    for (char character : encoded) {
        if (character == '=') break;
        const int value = base64_value(character);
        if (value < 0) return false;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((accumulator >> bits) & 0xff));
            accumulator = bits == 0 ? 0 : accumulator & ((1 << bits) - 1);
        }
    }
    return true;
}

bool supported_image_mime(const std::string& mime_type) {
    const std::string normalized = lower_ascii(mime_type);
    return normalized == "image/jpeg" || normalized == "image/png" ||
           normalized == "image/webp" || normalized == "image/gif";
}

// ── Web research helpers ────────────────────────────────────────────────────

// Percent-encode for use in a URL query component.
std::string url_encode(const std::string& value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const char raw : value) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// The page's <title> text (first occurrence), entity-decoded and trimmed.
std::string html_title(const std::string& html) {
    const std::size_t open = html.find("<title");
    if (open == std::string::npos) return "";
    const std::size_t gt = html.find('>', open);
    if (gt == std::string::npos) return "";
    const std::size_t close = html.find("</title", gt + 1);
    if (close == std::string::npos) return "";
    std::string title = html.substr(gt + 1, close - gt - 1);
    decode_entities(title);
    return trim_ascii(title);
}

// Reduce HTML to readable plain text: drop scripts/styles/comments, turn
// block boundaries into newlines, remove the remaining tags, decode entities,
// and collapse whitespace. Bounded at `max_chars` (0 = no bound); the bound

// is applied without splitting a UTF-8 sequence.
// Reduce HTML to readable plain text: content-root slice (a <main> holds
// the readable region by HTML semantics - portals nest their listings there
// while shell furniture lives at the body level), drop scripts/styles/comments,
// turn block boundaries into newlines, remove the remaining tags, decode
// entities, prune link-dense furniture lines, and collapse whitespace.
// Bounded at `max_chars` (0 = no bound); the bound is applied without
// splitting a UTF-8 sequence.
std::string html_to_text(const std::string& html, long long max_chars) {
    // Anchor markers for the link-density pass: <a ...> bodies are wrapped
    // so the per-line scan can see how much of each line came from links
    // (hrefs are dropped by extraction; the link-density rule works on the
    // wrapped original).
    constexpr char kLinkOpen = '\x01';
    constexpr char kLinkClose = '\x02';

    std::string text = html;

    // True when text[at] sits on the tag name `name` (lowercased compare,
    // exact tag boundary: the next char must be a tag terminator).
    auto tag_here = [&](std::size_t at, const char* name) {
        for (std::size_t k = 0; name[k]; ++k) {
            char c = text[at + k];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != name[k]) return false;
        }
        char after = at + 1 + std::strlen(name) < text.size()
                         ? text[at + 1 + std::strlen(name)] : '\0';
        return after == '>' || after == ' ' || after == '\t' ||
               after == '\n' || after == '\r' || after == '/';
    };

    // Content root: slice to <main> ... </main> (nested mains counted; an
    // unterminated or missing <main> keeps the full body).
    {
        for (std::size_t i = 0; i + 7 < text.size(); ++i) {
            if (text[i] != '<' || !tag_here(i, "main")) continue;
            std::size_t content_start = text.find('>', i);
            if (content_start == std::string::npos) break;
            ++content_start;
            std::size_t depth = 1;
            std::size_t scan = content_start;
            while (scan + 7 < text.size()) {
                if (text[scan] != '<') {
                    ++scan;
                    continue;
                }
                if (tag_here(scan, "main")) {
                    ++depth;
                    scan += 6;
                    continue;
                }
                if (tag_here(scan, "/main")) {
                    --depth;
                    if (depth == 0) {
                        if (scan > content_start) {
                            text = text.substr(content_start, scan - content_start);
                        }
                        break;
                    }
                    scan += 7;
                    continue;
                }
                ++scan;
            }
            break;  // the first <main> decides; an unterminated one keeps the body
        }
    }

    strip_tag_block(text, "script", "script");
    strip_tag_block(text, "style", "style");
    strip_tag_block(text, "noscript", "noscript");
    strip_tag_block(text, "template", "template");
    strip_tag_block(text, "svg", "svg");
    strip_tag_block(text, "head", "head");

    // Anchor markers (before the block/tag passes; the markers are control
    // bytes that survive entity decode and whitespace normalization).
    {
        std::string marked;
        marked.reserve(text.size() + 16);
        std::size_t i = 0;
        while (i < text.size()) {
            if (text[i] == '<' && i + 3 < text.size()) {
                const char c1 = static_cast<char>(text[i + 1] | 0x20);
                const char c2 = text[i + 2];
                if (c1 == 'a' && (c2 == ' ' || c2 == '\t' || c2 == '\n' || c2 == '\r' || c2 == '>')) {
                    const std::size_t gt = text.find('>', i);
                    if (gt != std::string::npos) {
                        marked.push_back(kLinkOpen);
                        i = gt + 1;
                        continue;
                    }
                }
                if (text[i + 1] == '/' && c1 == 'a') {
                    const std::size_t gt = text.find('>', i);
                    if (gt != std::string::npos) {
                        marked.push_back(kLinkClose);
                        i = gt + 1;
                        continue;
                    }
                }
            }
            marked.push_back(text[i]);
            ++i;
        }
        text = std::move(marked);
    }

    // HTML comments.
    std::size_t comment_start = 0;
    while ((comment_start = text.find("<!--", comment_start)) != std::string::npos) {
        const std::size_t comment_end = text.find("-->", comment_start + 4);
        if (comment_end == std::string::npos) {
            text.erase(comment_start);
            break;
        }
        text.erase(comment_start, comment_end + 3 - comment_start);
    }
    static const std::regex kBreak(
        "</?(p|div|li|ul|ol|table|tr|td|th|h[1-6]|section|article|header|"\
        "footer|blockquote|pre|br|figure|figcaption)[^>]*>",
        std::regex::icase);
    text = std::regex_replace(text, kBreak, "\n");
    static const std::regex kTag("<[^>]+>");
    text = std::regex_replace(text, kTag, " ");
    decode_entities(text);

    // Per-line link-density prune (BoilerplateTrimmer): a line dominated by
    // anchor text (≥3 anchors, ≥60% of its visible chars) is navigation
    // furniture - extraction keeps anchor text but drops hrefs, so nav
    // menus survive as word-lines that bury the listing. The keep-share
    // guard reverts the whole prune when it stripped >45% of the extracted
    // bytes (over-trimming hides real content and is worse than furniture).
    {
        std::string pruned;
        std::size_t original_bytes = 0;
        std::size_t kept_bytes = 0;
        std::size_t cursor = 0;
        while (cursor <= text.size()) {
            const std::size_t line_end = text.find('\n', cursor);
            const std::string_view line(
                text.data() + cursor,
                (line_end == std::string::npos ? text.size() : line_end) - cursor);
            std::size_t total = 0;
            std::size_t link_chars = 0;
            std::size_t anchors = 0;
            bool in_link = false;
            for (const char c : line) {
                if (c == kLinkOpen) {
                    in_link = true;
                    ++anchors;
                } else if (c == kLinkClose) {
                    in_link = false;
                } else if (c != ' ' && c != '\t' && c != '\r') {
                    ++total;
                    if (in_link) ++link_chars;
                }
            }
            const bool furniture = anchors >= 3 && link_chars * 2 >= total && total >= 60;
            if (!furniture) {
                // Copy the line minus the markers.
                for (const char c : line) {
                    if (c != kLinkOpen && c != kLinkClose) pruned.push_back(c);
                }
                kept_bytes += total;
            }
            original_bytes += total;
            if (line_end == std::string::npos) break;
            if (!furniture) pruned.push_back('\n');
            cursor = line_end + 1;
        }
        if (original_bytes > 0 && kept_bytes * 100 >= original_bytes * 55) {
            text = std::move(pruned);
        }
        // Marker cleanup on the unpruned path too.
        if (!text.empty() &&
            (text.find(kLinkOpen) != std::string::npos ||
             text.find(kLinkClose) != std::string::npos)) {
            std::string cleaned;
            cleaned.reserve(text.size());
            for (const char c : text) {
                if (c != kLinkOpen && c != kLinkClose) cleaned.push_back(c);
            }
            text = std::move(cleaned);
        }
    }

    std::string out;
    out.reserve(text.size());
    bool last_space = true;  // skip leading blanks
    for (const char c : text) {
        if (c == '\n') {
            out.push_back('\n');
            last_space = true;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            if (!last_space) out.push_back(' ');
            last_space = true;
        } else {
            out.push_back(c);
            last_space = false;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) {
        out.pop_back();
    }
    // Collapse blank-line runs to a single blank line.
    std::string collapsed;
    collapsed.reserve(out.size());
    int blanks = 0;
    for (const char c : out) {
        if (c == '\n') {
            if (blanks < 1) collapsed.push_back('\n');
            ++blanks;
        } else {
            collapsed.push_back(c);
            blanks = 0;
        }
    }
    if (max_chars > 0 && collapsed.size() > static_cast<std::size_t>(max_chars)) {
        collapsed.resize(static_cast<std::size_t>(max_chars));
        while (!collapsed.empty() &&
               (static_cast<unsigned char>(collapsed.back()) & 0xC0) == 0x80) {
            collapsed.pop_back();
        }
        collapsed += "…[truncated]";
    }
    return collapsed;
}

bool parse_image_data_url(const std::string& data_url,
                          std::string& mime_type,
                          std::string& encoded_payload,
                          std::string* decoded_bytes) {
    if (data_url.size() < 6 || lower_ascii(data_url.substr(0, 5)) != "data:") {
        return false;
    }
    const std::size_t comma = data_url.find(',');
    if (comma == std::string::npos || comma <= 5 || comma + 1 >= data_url.size()) {
        return false;
    }
    const std::string metadata = data_url.substr(5, comma - 5);
    const std::size_t separator = metadata.find(';');
    if (separator == std::string::npos) return false;
    mime_type = metadata.substr(0, separator);
    if (!supported_image_mime(mime_type)) return false;
    for (const unsigned char character : mime_type) {
        if (std::isspace(character) || std::iscntrl(character)) return false;
    }
    if (lower_ascii(metadata.substr(separator + 1)) != "base64") return false;
    encoded_payload = data_url.substr(comma + 1);
    std::string decoded;
    if (!decode_base64(encoded_payload, decoded)) return false;
    if (decoded_bytes) *decoded_bytes = std::move(decoded);
    mime_type = lower_ascii(mime_type);
    return true;
}

}  // namespace nyaxus
