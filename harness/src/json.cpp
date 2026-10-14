#include "nyaxus/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace nyaxus {
namespace json {

Value::Value(bool value) : type_(Type::Bool), bool_(value) {}
Value::Value(int value) : type_(Type::Number), number_(static_cast<double>(value)) {}
Value::Value(long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
Value::Value(long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
Value::Value(double value) : type_(Type::Number), number_(value) {}
Value::Value(const char* value) : type_(Type::String), string_(value ? value : "") {}
Value::Value(std::string value) : type_(Type::String), string_(std::move(value)) {}
Value::Value(Array value)
    : type_(Type::Array), array_(std::make_shared<Array>(std::move(value))) {}
Value::Value(Object value)
    : type_(Type::Object), object_(std::make_shared<Object>(std::move(value))) {}

bool Value::as_bool(bool fallback) const {
    return type_ == Type::Bool ? bool_ : fallback;
}

double Value::as_number(double fallback) const {
    return type_ == Type::Number ? number_ : fallback;
}

long long Value::as_integer(long long fallback) const {
    return type_ == Type::Number ? static_cast<long long>(number_) : fallback;
}

const std::string& Value::as_string() const {
    return string_;
}

std::string Value::as_string(const std::string& fallback) const {
    return type_ == Type::String ? string_ : fallback;
}

const Array& Value::as_array() const {
    static const Array kEmpty;
    return array_ ? *array_ : kEmpty;
}

const Object& Value::as_object() const {
    static const Object kEmpty;
    return object_ ? *object_ : kEmpty;
}

Array& Value::mut_array() {
    if (!array_) {
        array_ = std::make_shared<Array>();
        type_ = Type::Array;
    }
    return *array_;
}

Object& Value::mut_object() {
    if (!object_) {
        object_ = std::make_shared<Object>();
        type_ = Type::Object;
    }
    return *object_;
}

bool Value::has(const std::string& key) const {
    return object_ && object_->count(key) != 0;
}

const Value& Value::at(const std::string& key) const {
    if (!object_ || object_->count(key) == 0) {
        static const Value kNull;
        return kNull;
    }
    return object_->at(key);
}

Value& Value::at(const std::string& key) {
    return mut_object()[key];
}

const Value& Value::operator[](const std::string& key) const {
    return at(key);
}

Value& Value::operator[](const std::string& key) {
    return mut_object()[key];
}

bool Value::contains(const std::string& key) const {
    return has(key);
}

namespace {

class Parser {
   public:
    explicit Parser(const std::string& text) : text_(text) {}

    Value parse() {
        skip_ws();
        Value result = parse_value();
        skip_ws();
        if (pos_ != text_.size()) throw std::runtime_error("json: trailing data");
        return result;
    }

   private:
    const std::string& text_;
    std::size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    char next() {
        if (pos_ >= text_.size()) throw std::runtime_error("json: unexpected end");
        return text_[pos_++];
    }

    Value parse_value() {
        const char c = peek();
        switch (c) {
            case '{':
                return parse_object();
            case '[':
                return parse_array();
            case '"':
                return Value(parse_string());
            case 't':
                expect_literal("true");
                return Value(true);
            case 'f':
                expect_literal("false");
                return Value(false);
            case 'n':
                expect_literal("null");
                return Value();
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
                throw std::runtime_error("json: unexpected token");
        }
    }

    void expect_literal(const char* literal) {
        for (const char* p = literal; *p; ++p) {
            if (next() != *p) throw std::runtime_error("json: bad literal");
        }
    }

    std::string parse_string() {
        if (next() != '"') throw std::runtime_error("json: expected string");
        std::string out;
        while (true) {
            char c = next();
            if (c == '"') break;
            if (c == '\\') {
                char e = next();
                switch (e) {
                    case '"':
                        out.push_back('"');
                        break;
                    case '\\':
                        out.push_back('\\');
                        break;
                    case '/':
                        out.push_back('/');
                        break;
                    case 'b':
                        out.push_back('\b');
                        break;
                    case 'f':
                        out.push_back('\f');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'u': {
                        unsigned int code = parse_hex4();
                        if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
                            text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            const unsigned int low = parse_hex4();
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        }
                        append_utf8(out, code);
                        break;
                    }
                    default:
                        out.push_back(e);
                        break;
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    unsigned int parse_hex4() {
        unsigned int code = 0;
        for (int i = 0; i < 4; ++i) {
            char c = next();
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<unsigned int>(c - 'A' + 10);
            else throw std::runtime_error("json: bad hex");
        }
        return code;
    }

    static void append_utf8(std::string& out, unsigned int code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    Value parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        const std::string token = text_.substr(start, pos_ - start);
        return Value(std::strtod(token.c_str(), nullptr));
    }

    Value parse_array() {
        next();  // [
        Array array;
        skip_ws();
        if (peek() == ']') {
            next();
            return Value(array);
        }
        while (true) {
            skip_ws();
            array.push_back(parse_value());
            skip_ws();
            char c = next();
            if (c == ']') break;
            if (c != ',') throw std::runtime_error("json: expected comma in array");
        }
        return Value(std::move(array));
    }

    Value parse_object() {
        next();  // {
        Object object;
        skip_ws();
        if (peek() == '}') {
            next();
            return Value(object);
        }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (next() != ':') throw std::runtime_error("json: expected colon");
            skip_ws();
            object.emplace(std::move(key), parse_value());
            skip_ws();
            char c = next();
            if (c == '}') break;
            if (c != ',') throw std::runtime_error("json: expected comma in object");
        }
        return Value(std::move(object));
    }
};

void escape_into(std::string& out, const std::string& value) {
    out.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (character < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", character);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    out.push_back('"');
}

void stringify_into(const Value& value, std::string& out, bool pretty, int depth) {
    switch (value.type()) {
        case Value::Type::Null:
            out += "null";
            break;
        case Value::Type::Bool:
            out += value.as_bool() ? "true" : "false";
            break;
        case Value::Type::Number: {
            double number = value.as_number();
            if (std::isfinite(number) && number == std::floor(number) &&
                std::abs(number) < 1e15) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.0f", number);
                out += buf;
            } else {
                // Shortest decimal that round-trips to the same double, so 0.95
                // stays 0.95 instead of 0.94999999999999996. The value is still
                // exact: we only emit fewer digits once they suffice.
                char buf[40];
                bool done = false;
                for (int precision = 1; precision <= 17 && !done; ++precision) {
                    std::snprintf(buf, sizeof(buf), "%.*g", precision, number);
                    try {
                        if (std::stod(buf) == number) {
                            out += buf;
                            done = true;
                        }
                    } catch (...) {
                        // Non-finite (inf/nan): fall through to the 17-digit form.
                    }
                }
                if (!done) {
                    std::snprintf(buf, sizeof(buf), "%.17g", number);
                    out += buf;
                }
            }
            break;
        }
        case Value::Type::String:
            escape_into(out, value.as_string());
            break;
        case Value::Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const Value& item : value.as_array()) {
                if (!first) out.push_back(',');
                first = false;
                if (pretty) out += "\n" + std::string(depth + 1, ' ');
                stringify_into(item, out, pretty, depth + 1);
            }
            if (pretty && !value.as_array().empty()) out += "\n" + std::string(depth, ' ');
            out.push_back(']');
            break;
        }
        case Value::Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, item] : value.as_object()) {
                if (!first) out.push_back(',');
                first = false;
                if (pretty) out += "\n" + std::string(depth + 1, ' ');
                escape_into(out, key);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                stringify_into(item, out, pretty, depth + 1);
            }
            if (pretty && !value.as_object().empty()) out += "\n" + std::string(depth, ' ');
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

Value parse(const std::string& text) {
    Parser parser(text);
    return parser.parse();
}

std::string stringify(const Value& value, bool pretty) {
    std::string out;
    stringify_into(value, out, pretty, 0);
    return out;
}

}  // namespace json
}  // namespace nyaxus