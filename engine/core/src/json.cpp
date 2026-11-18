// SPDX-License-Identifier: Apache-2.0
#include "victus/core/json.hpp"
#include "victus/core/charconv.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <format>

namespace victus::json {

namespace {
const std::string kEmptyString;
const Array kEmptyArray;
const Object kEmptyObject;
const Value kNull;
} // namespace

Value& Value::operator=(const Value& other) {
    if (this == &other) return *this;
    type_ = other.type_;
    bool_ = other.bool_;
    number_ = other.number_;
    string_ = other.string_;
    array_ = other.array_;
    object_ = other.object_;
    return *this;
}

// Copy-on-write: copies share storage until mutated.
void Value::ensure_unique() {
    if (string_ && string_.use_count() > 1) string_ = std::make_shared<std::string>(*string_);
    if (array_ && array_.use_count() > 1) array_ = std::make_shared<Array>(*array_);
    if (object_ && object_.use_count() > 1) object_ = std::make_shared<Object>(*object_);
}

const std::string& Value::as_string() const noexcept { return is_string() ? *string_ : kEmptyString; }
const Array& Value::as_array() const noexcept { return is_array() ? *array_ : kEmptyArray; }
const Object& Value::as_object() const noexcept { return is_object() ? *object_ : kEmptyObject; }

Array& Value::as_array() {
    if (!is_array()) *this = Value(Array{});
    ensure_unique();
    return *array_;
}

Object& Value::as_object() {
    if (!is_object()) *this = Value(Object{});
    ensure_unique();
    return *object_;
}

const Value* Value::find(std::string_view key) const noexcept {
    if (!is_object()) return nullptr;
    for (const auto& [k, v] : *object_)
        if (k == key) return &v;
    return nullptr;
}

const Value& Value::operator[](std::string_view key) const noexcept {
    const Value* v = find(key);
    return v ? *v : kNull;
}

Value& Value::set(std::string_view key, Value value) {
    Object& obj = as_object();
    for (auto& [k, v] : obj) {
        if (k == key) {
            v = std::move(value);
            return v;
        }
    }
    obj.emplace_back(std::string(key), std::move(value));
    return obj.back().second;
}

const Value& Value::operator[](std::size_t index) const noexcept {
    if (!is_array() || index >= array_->size()) return kNull;
    return (*array_)[index];
}

Value& Value::push(Value value) {
    Array& arr = as_array();
    arr.push_back(std::move(value));
    return arr.back();
}

std::size_t Value::size() const noexcept {
    if (is_array()) return array_->size();
    if (is_object()) return object_->size();
    return 0;
}

bool operator==(const Value& a, const Value& b) {
    if (a.type_ != b.type_) return false;
    switch (a.type_) {
    case Type::Null: return true;
    case Type::Bool: return a.bool_ == b.bool_;
    case Type::Number: return a.number_ == b.number_;
    case Type::String: return *a.string_ == *b.string_;
    case Type::Array: return *a.array_ == *b.array_;
    case Type::Object: return *a.object_ == *b.object_;
    }
    return false;
}

namespace {

class Parser {
public:
    Parser(std::string_view text, const ParseLimits& limits) : text_(text), limits_(limits) {}

    Result<Value> run() {
        if (text_.size() > limits_.max_bytes) return make_error(ErrorCode::LimitExceeded, "JSON input exceeds size limit");
        Value v;
        if (!parse_value(v, 0)) return fail();
        skip_ws();
        if (pos_ != text_.size()) {
            set_error("trailing characters after JSON value");
            return fail();
        }
        return v;
    }

private:
    Error fail() const {
        std::size_t line = 1, col = 1;
        for (std::size_t i = 0; i < error_pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        return make_error(ErrorCode::ParseError, std::format("JSON {}:{}: {}", line, col, error_));
    }
    void set_error(const char* msg) {
        if (error_.empty()) {
            error_ = msg;
            error_pos_ = pos_;
        }
    }
    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
                // Line comments are tolerated in hand-edited project files.
                while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }
    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) {
            set_error("invalid literal");
            return false;
        }
        pos_ += word.size();
        return true;
    }

    bool parse_value(Value& out, std::size_t depth) {
        if (depth > limits_.max_depth) {
            set_error("maximum nesting depth exceeded");
            return false;
        }
        skip_ws();
        if (pos_ >= text_.size()) {
            set_error("unexpected end of input");
            return false;
        }
        const char c = text_[pos_];
        switch (c) {
        case 'n': out = Value(); return literal("null");
        case 't': out = Value(true); return literal("true");
        case 'f': out = Value(false); return literal("false");
        case '"': {
            std::string s;
            if (!parse_string(s)) return false;
            out = Value(std::move(s));
            return true;
        }
        case '[': return parse_array(out, depth);
        case '{': return parse_object(out, depth);
        default: return parse_number(out);
        }
    }

    bool parse_number(Value& out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size() || !(text_[pos_] >= '0' && text_[pos_] <= '9')) {
            set_error("invalid value");
            return false;
        }
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-')
                ++pos_;
            else
                break;
        }
        double d = 0.0;
        const auto res = victus::from_chars_double(text_.data() + start, text_.data() + pos_, d);
        if (res.ec != std::errc() || res.ptr != text_.data() + pos_ || !std::isfinite(d)) {
            pos_ = start;
            set_error("invalid number");
            return false;
        }
        out = Value(d);
        return true;
    }

    static void append_utf8(std::string& s, std::uint32_t cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parse_hex4(std::uint32_t& cp) {
        if (pos_ + 4 > text_.size()) {
            set_error("truncated unicode escape");
            return false;
        }
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
            else {
                set_error("invalid unicode escape");
                return false;
            }
        }
        return true;
    }

    bool parse_string(std::string& out) {
        ++pos_; // opening quote
        while (true) {
            if (pos_ >= text_.size()) {
                set_error("unterminated string");
                return false;
            }
            if (out.size() > limits_.max_string_bytes) {
                set_error("string exceeds size limit");
                return false;
            }
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) {
                set_error("control character in string");
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                set_error("unterminated escape");
                return false;
            }
            const char e = text_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!parse_hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    std::uint32_t lo = 0;
                    if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                        set_error("unpaired surrogate");
                        return false;
                    }
                    pos_ += 2;
                    if (!parse_hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
                        set_error("invalid surrogate pair");
                        return false;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                append_utf8(out, cp);
                break;
            }
            default: set_error("invalid escape"); return false;
            }
        }
    }

    bool parse_array(Value& out, std::size_t depth) {
        ++pos_;
        Array arr;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            out = Value(std::move(arr));
            return true;
        }
        while (true) {
            Value v;
            if (!parse_value(v, depth + 1)) return false;
            arr.push_back(std::move(v));
            skip_ws();
            if (pos_ >= text_.size()) {
                set_error("unterminated array");
                return false;
            }
            const char c = text_[pos_++];
            if (c == ']') break;
            if (c != ',') {
                --pos_;
                set_error("expected ',' or ']'");
                return false;
            }
        }
        out = Value(std::move(arr));
        return true;
    }

    bool parse_object(Value& out, std::size_t depth) {
        ++pos_;
        Object obj;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out = Value(std::move(obj));
            return true;
        }
        while (true) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                set_error("expected object key");
                return false;
            }
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                set_error("expected ':'");
                return false;
            }
            ++pos_;
            Value v;
            if (!parse_value(v, depth + 1)) return false;
            obj.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (pos_ >= text_.size()) {
                set_error("unterminated object");
                return false;
            }
            const char c = text_[pos_++];
            if (c == '}') break;
            if (c != ',') {
                --pos_;
                set_error("expected ',' or '}'");
                return false;
            }
        }
        out = Value(std::move(obj));
        return true;
    }

    std::string_view text_;
    ParseLimits limits_;
    std::size_t pos_{0};
    std::string error_;
    std::size_t error_pos_{0};
};

void write_string(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void write_number(std::string& out, double d) {
    if (std::floor(d) == d && std::fabs(d) < 9.007199254740992e15) {
        out += std::format("{}", static_cast<std::int64_t>(d));
    } else {
        // Shortest round-trip representation.
        char buf[64];
        const auto res = std::to_chars(buf, buf + sizeof buf, d);
        out.append(buf, res.ptr);
    }
}

void write_value(std::string& out, const Value& v, bool pretty, int indent) {
    auto newline = [&](int level) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<std::size_t>(level) * 2, ' ');
    };
    switch (v.type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += v.as_bool() ? "true" : "false"; break;
    case Type::Number: write_number(out, v.as_number()); break;
    case Type::String: write_string(out, v.as_string()); break;
    case Type::Array: {
        const Array& arr = v.as_array();
        if (arr.empty()) {
            out += "[]";
            break;
        }
        // Short numeric arrays (vectors, colors, matrices) stay on one line.
        bool compact = arr.size() <= 16;
        for (const auto& e : arr) compact = compact && e.is_number();
        out += '[';
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (i) out += compact && pretty ? ", " : ",";
            if (!compact) newline(indent + 1);
            write_value(out, arr[i], pretty, indent + 1);
        }
        if (!compact) newline(indent);
        out += ']';
        break;
    }
    case Type::Object: {
        const Object& obj = v.as_object();
        if (obj.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        for (std::size_t i = 0; i < obj.size(); ++i) {
            if (i) out += ',';
            newline(indent + 1);
            write_string(out, obj[i].first);
            out += pretty ? ": " : ":";
            write_value(out, obj[i].second, pretty, indent + 1);
        }
        newline(indent);
        out += '}';
        break;
    }
    }
}

} // namespace

Result<Value> parse(std::string_view text, const ParseLimits& limits) { return Parser(text, limits).run(); }

std::string stringify(const Value& value, bool pretty) {
    std::string out;
    write_value(out, value, pretty, 0);
    if (pretty) out += '\n';
    return out;
}

std::function<Status(std::string_view)> document_validator(std::string format) {
    return [format = std::move(format)](std::string_view text) -> Status {
        auto doc = parse(text);
        if (!doc) return std::move(doc).error();
        if (!doc->is_object()) return make_error(ErrorCode::ParseError, "not a JSON object");
        if (!format.empty() && (*doc)["format"].as_string_or("") != format)
            return make_error(ErrorCode::ParseError, "not a " + format + " document");
        return {};
    };
}

} // namespace victus::json
