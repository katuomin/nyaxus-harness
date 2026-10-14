#include "nyaxus/summary.hpp"

#include <cctype>
#include <cstring>
#include <set>

#include "nyaxus/json.hpp"

namespace nyaxus {
namespace summary {
namespace {

using nyaxus::json::Value;
using nyaxus::json::Array;
using nyaxus::json::Object;

std::string trim(const std::string& value) {
    const auto whitespace = [](char character) {
        return character == ' ' || character == '\t' || character == '\r' ||
               character == '\n';
    };
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && whitespace(value[begin])) ++begin;
    while (end > begin && whitespace(value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

// Small-model auxiliary responses often wrap an otherwise useful payload in a
// markdown fence despite the prompt. Remove only that outer presentation
// wrapper; the result is still treated as model output, not trusted state.
std::string strip_outer_fence(std::string value) {
    value = trim(value);
    if (value.rfind("```", 0) != 0) return value;
    const std::size_t first_newline = value.find('\n');
    if (first_newline == std::string::npos) return value;
    const std::size_t closing = value.rfind("```");
    if (closing <= first_newline) return value;
    return trim(value.substr(first_newline + 1, closing - first_newline - 1));
}

// Remove commas whose next significant character closes an object or array.
std::string strip_trailing_commas(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char character = value[i];
        if (character == ',') {
            std::size_t j = i + 1;
            while (j < value.size() &&
                   (value[j] == ' ' || value[j] == '\t' || value[j] == '\n' ||
                    value[j] == '\r')) {
                ++j;
            }
            if (j < value.size() && (value[j] == '}' || value[j] == ']')) {
                continue;
            }
        }
        out.push_back(character);
    }
    return out;
}

struct FieldSpec {
    const char* key;
    const std::vector<std::string>* items;
};

// Normalize one claim element into a canonical claim string. Small auxiliary
// models emit several shapes for the same logical field: a bare string, an
// array of strings, an array wrapping an array, or an array of objects that
// carry the text and a source id as separate keys. Accept all of them so a
// useful summary is not discarded over presentation. When an object carries a
// source id, attach it as an inline citation so provenance is preserved and
// still validated downstream.
bool normalize_element(const Value& element, std::string& claim, std::string& error) {
    if (element.type() == Value::Type::String) {
        claim = element.as_string();
        return true;
    }
    if (element.type() == Value::Type::Object) {
        const Object& obj = element.as_object();
        std::string text;
        for (const char* key : {"text", "claim", "value", "description", "summary"}) {
            if (obj.count(key)) {
                const Value& candidate = obj.at(key);
                if (candidate.type() == Value::Type::String) {
                    text = candidate.as_string();
                    break;
                }
            }
        }
        if (text.empty()) {
            static const char* skip[] = {"role", "type", "id",
                                         "source message id", "source_message_id",
                                         "source_id"};
            for (const auto& pair : obj) {
                bool skipped = false;
                for (const char* name : skip) {
                    if (pair.first == name) { skipped = true; break; }
                }
                if (!skipped && pair.second.type() == Value::Type::String) {
                    text = pair.second.as_string();
                    break;
                }
            }
        }
        if (text.empty()) {
            error = "object claim has no text";
            return false;
        }
        long long source_id = -1;
        bool has_id = false;
        for (const char* key : {"source message id", "source_message_id",
                               "source_id", "id"}) {
            if (!obj.count(key)) continue;
            const Value& candidate = obj.at(key);
            if (candidate.type() == Value::Type::Number) {
                source_id = candidate.as_integer(-1);
                has_id = true;
                break;
            }
            if (candidate.type() == Value::Type::String) {
                const std::string digits = candidate.as_string();
                if (digits.empty()) continue;
                bool all_digits = true;
                for (char character : digits) {
                    if (!std::isdigit(static_cast<unsigned char>(character))) {
                        all_digits = false;
                        break;
                    }
                }
                if (all_digits) {
                    try {
                        source_id = std::stoll(digits);
                    } catch (...) {
                        continue;
                    }
                    has_id = true;
                    break;
                }
            }
        }
        if (has_id && source_id >= 0) {
            claim = text + " [source message id: " + std::to_string(source_id) + "]";
        } else {
            claim = text;
        }
        return true;
    }
    error = "unsupported claim element type";
    return false;
}

bool check_field(const Value& root, const char* key, std::vector<std::string>& out,
                 std::string& error) {
    if (!root.has(key)) {
        error = std::string("missing key '") + key + "'";
        return false;
    }
    const Value& field = root.at(key);
    std::vector<Value> elements;
    if (field.type() == Value::Type::String) {
        // A single string where an array is expected: accept as one claim.
        elements.push_back(field);
    } else if (field.type() == Value::Type::Array) {
        for (const Value& element : field.as_array()) {
            if (element.type() == Value::Type::Array) {
                // The model wrapped the list in an extra array: flatten one
                // level.
                for (const Value& inner : element.as_array()) {
                    elements.push_back(inner);
                }
            } else {
                elements.push_back(element);
            }
        }
    } else {
        error = std::string("key '") + key + "' must be an array or string";
        return false;
    }
    if (elements.size() > kMaxItemsPerField) {
        error = std::string("key '") + key + "' has more than " +
                std::to_string(kMaxItemsPerField) + " items";
        return false;
    }
    out.clear();
    out.reserve(elements.size());
    for (const Value& element : elements) {
        std::string claim;
        if (!normalize_element(element, claim, error)) {
            error = std::string("key '") + key + ": " + error;
            return false;
        }
        claim = trim(claim);
        if (claim.empty()) {
            continue;  // skip empty entries; a field may legitimately be empty
        }
        if (claim.size() > kMaxItemBytes) {
            claim = claim.substr(0, kMaxItemBytes);
        }
        out.push_back(claim);
    }
    return true;
}

// Map a section title to a Document field. Tolerant of case, punctuation,
// and common synonyms.
enum class Section { None, Goal, Decisions, Constraints, Unresolved, Artifacts,
                     NextSteps, LatestUser };

Section resolve_section(const std::string& raw_title) {
    std::string norm;
    for (unsigned char character : raw_title) {
        if (std::isalpha(character)) {
            norm.push_back(static_cast<char>(std::tolower(character)));
        } else if (std::isdigit(character)) {
            norm.push_back(static_cast<char>(character));
        }
    }
    if (norm == "goal" || norm == "intent" || norm == "objective" ||
        norm == "primaryrequestandintent" || norm == "requestandintent" ||
        norm == "userintent" || norm == "summary") {
        return Section::Goal;
    }
    if (norm == "decisions" || norm == "keydecisions" || norm == "decisionsmade") {
        return Section::Decisions;
    }
    if (norm == "constraints" || norm == "constraintsandpreferences" ||
        norm == "preferences") {
        return Section::Constraints;
    }
    if (norm == "unresolvedtasks" || norm == "pendingjobs" || norm == "opentasks" ||
        norm == "todo" || norm == "pendingwork") {
        return Section::Unresolved;
    }
    if (norm == "relevantartifacts" || norm == "artifacts" || norm == "filesandcode" ||
        norm == "files" || norm == "relevantfiles" || norm == "fileschanged") {
        return Section::Artifacts;
    }
    if (norm == "nextsteps" || norm == "nextstep" || norm == "nextactions") {
        return Section::NextSteps;
    }
    if (norm == "latestuserrequest" || norm == "mostrecentrequest" ||
        norm == "currentrequest") {
        return Section::LatestUser;
    }
    return Section::None;
}

// Parse a sectioned markdown checkpoint into a Document. The primary
// auxiliary output contract: headings map to fields, bullets to items.
// Missing sections are simply empty; unrecognized sections are ignored.
bool parse_markdown_checkpoint(const std::string& raw, Document& out,
                               std::string& error) {
    Section current = Section::None;
    bool saw_section = false;
    bool any_content = false;

    std::size_t start = 0;
    while (start <= raw.size()) {
        const std::size_t nl = raw.find('\n', start);
        const std::string line =
            (nl == std::string::npos) ? raw.substr(start) : raw.substr(start, nl - start);
        start = (nl == std::string::npos) ? raw.size() + 1 : nl + 1;

        const std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        std::size_t hash = 0;
        while (hash < trimmed.size() && trimmed[hash] == '#') ++hash;
        const bool is_header =
            hash >= 1 && hash <= 6 &&
            (hash == trimmed.size() || trimmed[hash] == ' ');
        if (is_header) {
            current = resolve_section(trim(trimmed.substr(hash)));
            saw_section = true;
            continue;
        }
        if (!saw_section || current == Section::None) continue;

        std::string item = trimmed;
        if (!item.empty() && (item[0] == '-' || item[0] == '*' || item[0] == '+')) {
            item = trim(item.substr(1));
        } else {
            std::size_t digits = 0;
            while (digits < item.size() &&
                   std::isdigit(static_cast<unsigned char>(item[digits]))) {
                ++digits;
            }
            if (digits > 0 && digits < item.size() &&
                (item[digits] == '.' || item[digits] == ')')) {
                item = trim(item.substr(digits + 1));
            }
        }
        // Small models express "empty section" several ways: (none), None,
        // N/A. Skip them so they do not become phantom claims.
        std::string item_lower = item;
        for (char& character : item_lower) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        if (item.empty() || item_lower == "none" || item_lower == "(none)" ||
            item_lower == "n/a") {
            continue;
        }
        if (item.size() > kMaxItemBytes) item = item.substr(0, kMaxItemBytes);

        switch (current) {
            case Section::Goal:
                if (out.goal.empty()) out.goal = item;
                else out.goal += "; " + item;
                break;
            case Section::Decisions:
                if (out.decisions.size() < kMaxItemsPerField) out.decisions.push_back(item);
                break;
            case Section::Constraints:
                if (out.constraints.size() < kMaxItemsPerField) out.constraints.push_back(item);
                break;
            case Section::Unresolved:
                if (out.unresolved_tasks.size() < kMaxItemsPerField) {
                    out.unresolved_tasks.push_back(item);
                }
                break;
            case Section::Artifacts:
                if (out.relevant_artifacts.size() < kMaxItemsPerField) {
                    out.relevant_artifacts.push_back(item);
                }
                break;
            case Section::NextSteps:
                if (out.next_steps.size() < kMaxItemsPerField) out.next_steps.push_back(item);
                break;
            case Section::LatestUser:
                if (out.latest_user_request.empty()) out.latest_user_request = item;
                break;
            case Section::None:
                break;
        }
        any_content = true;
    }

    if (!saw_section) {
        error = "output has no section headers";
        return false;
    }
    if (!any_content) {
        error = "checkpoint sections are all empty";
        return false;
    }
    return true;
}

bool check_json_schema(const Value& root, Document& out, std::string& error) {
    if (root.type() != Value::Type::Object) {
        error = "JSON output is not an object";
        return false;
    }
    // All fields optional; accept recognized keys, ignore unknown ones. The
    // document is valid as long as at least one field ends up non-empty.
    if (root.has("goal")) {
        const Value& value = root.at("goal");
        if (value.type() == Value::Type::String) {
            out.goal = trim(value.as_string());
        } else if (value.type() == Value::Type::Array) {
            std::string joined;
            for (const Value& element : value.as_array()) {
                std::string claim;
                if (normalize_element(element, claim, error)) {
                    claim = trim(claim);
                    if (!claim.empty()) {
                        if (!joined.empty()) joined += "; ";
                        joined += claim;
                    }
                }
            }
            out.goal = joined;
        }
    }
    if (root.has("decisions") &&
        !check_field(root, "decisions", out.decisions, error)) {
        return false;
    }
    if (root.has("constraints") &&
        !check_field(root, "constraints", out.constraints, error)) {
        return false;
    }
    if (root.has("unresolved_tasks") &&
        !check_field(root, "unresolved_tasks", out.unresolved_tasks, error)) {
        return false;
    }
    if (root.has("relevant_artifacts") &&
        !check_field(root, "relevant_artifacts", out.relevant_artifacts, error)) {
        return false;
    }
    if (root.has("next_steps") &&
        !check_field(root, "next_steps", out.next_steps, error)) {
        return false;
    }
    if (root.has("latest_user_request")) {
        const Value& value = root.at("latest_user_request");
        if (value.type() == Value::Type::String) {
            out.latest_user_request = trim(value.as_string());
        }
    }
    if (root.has("anchors") && !check_field(root, "anchors", out.anchors, error)) {
        return false;
    }
    return true;
}

bool document_has_content(const Document& doc) {
    return !doc.goal.empty() || !doc.decisions.empty() || !doc.constraints.empty() ||
           !doc.unresolved_tasks.empty() || !doc.relevant_artifacts.empty() ||
           !doc.next_steps.empty() || !doc.latest_user_request.empty() ||
           !doc.anchors.empty();
}

bool extract_citations(const std::string& item, std::vector<long long>& ids,
                       std::string& error) {
    static const char* marker = "[source message id: ";
    const std::size_t marker_size = std::strlen(marker);
    std::size_t cursor = 0;
    while ((cursor = item.find(marker, cursor)) != std::string::npos) {
        const std::size_t start = cursor + marker_size;
        const std::size_t end = item.find(']', start);
        if (end == std::string::npos || end == start) {
            error = "claim has a malformed citation";
            return false;
        }
        for (std::size_t i = start; i < end; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(item[i]))) {
                error = "claim has a malformed citation";
                return false;
            }
        }
        try {
            ids.push_back(std::stoll(item.substr(start, end - start)));
        } catch (...) {
            error = "claim has a malformed citation";
            return false;
        }
        cursor = end + 1;
    }
    // Citations are optional: small auxiliary models frequently omit them. A
    // claim with no citation is accepted; a claim with a citation must still
    // reference a real source message, which validate_provenance enforces.
    return true;
}

}  // namespace

bool parse_document(const std::string& raw, Document& out, std::string& error) {
    const std::string candidate = strip_outer_fence(raw);
    if (candidate.empty()) {
        error = "auxiliary output is empty";
        return false;
    }

    // Primary: a sectioned markdown checkpoint.
    Document markdown_doc;
    std::string markdown_error;
    if (parse_markdown_checkpoint(candidate, markdown_doc, markdown_error) &&
        document_has_content(markdown_doc)) {
        out = markdown_doc;
        if (canonicalize(out).size() > kMaxDocumentBytes) {
            error = "summary document exceeds the " + std::to_string(kMaxDocumentBytes) +
                    " byte limit";
            return false;
        }
        return true;
    }

    // Fallback: a JSON object (tolerant element normalizer).
    Value root(Value::Type::Null);
    const auto try_parse = [&](const std::string& text) -> bool {
        try {
            root = nyaxus::json::parse(text);
            return true;
        } catch (...) {
            return false;
        }
    };
    std::string json_candidate = candidate;
    if (!try_parse(json_candidate)) {
        // Repair pass: take the outermost {...} span, which drops prose the
        // model added before or after the object.
        const std::size_t first = candidate.find('{');
        const std::size_t last = candidate.rfind('}');
        if (first == std::string::npos || last == std::string::npos || last <= first) {
            error = "output is neither a sectioned checkpoint nor a JSON object";
            return false;
        }
        json_candidate = trim(candidate.substr(first, last - first + 1));
        if (!try_parse(json_candidate)) {
            // Repair pass: trailing commas before a closing bracket.
            if (!try_parse(strip_trailing_commas(json_candidate))) {
                error = "output is neither a sectioned checkpoint nor valid JSON";
                return false;
            }
        }
    }

    Document json_doc;
    if (!check_json_schema(root, json_doc, error)) return false;
    if (!document_has_content(json_doc)) {
        error = "JSON document has no recognized fields";
        return false;
    }
    out = json_doc;
    if (canonicalize(out).size() > kMaxDocumentBytes) {
        error = "summary document exceeds the " + std::to_string(kMaxDocumentBytes) +
                " byte limit";
        return false;
    }
    return true;
}

bool validate_provenance(const Document& doc, const std::set<long long>& valid_ids,
                         std::string& error) {
    const FieldSpec fields[] = {
        {"decisions", &doc.decisions},
        {"constraints", &doc.constraints},
        {"unresolved_tasks", &doc.unresolved_tasks},
        {"relevant_artifacts", &doc.relevant_artifacts},
        {"next_steps", &doc.next_steps},
    };
    auto check_one = [&](const char* name, const std::string& item) -> bool {
        std::vector<long long> ids;
        std::string field_error;
        if (!extract_citations(item, ids, field_error)) {
            error = std::string(name) + ": " + field_error;
            return false;
        }
        for (const long long id : ids) {
            if (!valid_ids.count(id)) {
                error = std::string(name) +
                        ": cites unknown source message id " + std::to_string(id);
                return false;
            }
        }
        return true;
    };
    if (!doc.goal.empty() && !check_one("goal", doc.goal)) return false;
    for (const FieldSpec& field : fields) {
        for (const std::string& item : *field.items) {
            if (!check_one(field.key, item)) return false;
        }
    }
    // anchors and latest_user_request are harness-populated (verbatim from the
    // source), so they are inherently grounded and not citation-checked.
    return true;
}

std::string canonicalize(const Document& doc) {
    const auto to_array = [](const std::vector<std::string>& items) {
        Array array;
        array.reserve(items.size());
        for (const std::string& item : items) array.emplace_back(item);
        return Value(std::move(array));
    };
    Object object;
    object["goal"] = Value(doc.goal);
    object["decisions"] = to_array(doc.decisions);
    object["constraints"] = to_array(doc.constraints);
    object["unresolved_tasks"] = to_array(doc.unresolved_tasks);
    object["relevant_artifacts"] = to_array(doc.relevant_artifacts);
    object["next_steps"] = to_array(doc.next_steps);
    object["latest_user_request"] = Value(doc.latest_user_request);
    object["anchors"] = to_array(doc.anchors);
    return nyaxus::json::stringify(Value(std::move(object)));
}

std::string render_readable(const Document& doc) {
    std::string out;
    const auto append_list = [&](const char* heading,
                                 const std::vector<std::string>& items) {
        if (items.empty()) return;
        out += "## " + std::string(heading) + "\n";
        for (const std::string& item : items) out += "- " + item + "\n";
        out += "\n";
    };
    if (!doc.goal.empty()) {
        out += "## Goal\n" + doc.goal + "\n\n";
    }
    append_list("Decisions", doc.decisions);
    append_list("Constraints", doc.constraints);
    append_list("Unresolved Tasks", doc.unresolved_tasks);
    append_list("Relevant Artifacts", doc.relevant_artifacts);
    append_list("Next Steps", doc.next_steps);
    if (!doc.latest_user_request.empty()) {
        out += "## Latest User Request\n" + doc.latest_user_request + "\n\n";
    }
    if (!doc.anchors.empty()) {
        out += "## Anchors (verbatim from source)\n";
        for (const std::string& anchor : doc.anchors) out += "- " + anchor + "\n";
        out += "\n";
    }
    return out;
}

void harvest_anchors(const std::string& source, std::vector<std::string>& out) {
    std::set<std::string> seen;
    auto add = [&](const std::string& raw_value) {
        std::string value = trim(raw_value);
        while (!value.empty() && (value.back() == '.' || value.back() == ',' ||
                                  value.back() == ';' || value.back() == ':' ||
                                  value.back() == ')' || value.back() == ']' ||
                                  value.back() == '}' || value.back() == '"' ||
                                  value.back() == '\'')) {
            value.pop_back();
        }
        if (value.size() < 4 || value.size() > 512) return;
        if (!seen.count(value)) {
            seen.insert(value);
            out.push_back(value);
        }
    };

    // URLs: http(s)://...
    std::size_t cursor = 0;
    while (true) {
        const std::size_t pos = source.find("http", cursor);
        if (pos == std::string::npos) break;
        std::size_t scheme_end = pos + 4;  // after "http"
        if (scheme_end < source.size() && source[scheme_end] == 's') ++scheme_end;
        if (scheme_end + 2 <= source.size() && source[scheme_end] == ':' &&
            source[scheme_end + 1] == '/') {
            std::size_t end = scheme_end + 2;
            while (end < source.size()) {
                const char c = source[end];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' ||
                    c == '\'' || c == ')' || c == ']' || c == '}' || c == ',' ||
                    c == ';' || c == '\\') {
                    // A backslash ends the URL: in JSON exports the newline
                    // escape "\n" is two literal characters, and letting the
                    // scanner run past it captures the escape as part of the
                    // anchor.
                    break;
                }
                ++end;
            }
            add(source.substr(pos, end - pos));
            cursor = end;
        } else {
            cursor = pos + 1;
        }
    }

    // File paths: a token of path characters containing a slash, a dot after
    // the last slash (an extension), and not a URL (already captured above).
    std::size_t i = 0;
    while (i < source.size()) {
        const char c = source[i];
        const bool start_ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                              c == '_' || c == '/' || c == '~';
        if (!start_ok) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < source.size()) {
            const char d = source[i];
            if (std::isalnum(static_cast<unsigned char>(d)) || d == '.' || d == '_' ||
                d == '/' || d == '-' || d == '~') {
                ++i;
            } else {
                break;
            }
        }
        const std::string token = source.substr(start, i - start);
        if (token.find('/') != std::string::npos &&
            token.find("http") == std::string::npos) {
            const std::size_t last_slash = token.rfind('/');
            const std::size_t last_dot = token.rfind('.');
            // A real path has an extension dot after the last slash. Without
            // the npos guard, slash-words like "fork/branch" (no dot at all)
            // slip through, because npos compares greater than any index.
            if (last_dot != std::string::npos && last_dot > last_slash &&
                last_dot != token.size() - 1) {
                add(token);
            }
        }
    }
}

}  // namespace summary
}  // namespace nyaxus
