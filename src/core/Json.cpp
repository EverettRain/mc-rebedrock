#include "core/Json.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace mc::core {
namespace {

const Json& nullValue() {
    static const Json kNull{};
    return kNull;
}

// Process-wide parse tally, read by parseCount(). Relaxed because it is a pure
// diagnostic — nothing orders memory against it — but atomic so a headless test
// that parses on more than one thread still gets a coherent count.
std::atomic<std::uint64_t> g_parseCount{0};

void escapeInto(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20U) {
                    // A bare control character is not legal JSON text; spell it as
                    // a \u escape. Everything printable (including UTF-8 multibyte
                    // sequences) passes straight through.
                    std::array<char, 7> buffer{};
                    std::snprintf(buffer.data(), buffer.size(), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out.append(buffer.data());
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void numberInto(std::string& out, double value) {
    // A number that is exactly an integer prints without a decimal point, so a
    // baked count of 4 dumps as `4` and reads back as 4 — the round-trip the codec
    // relies on to carry integer fields. Anything fractional (or huge) uses a
    // precision that reproduces the double exactly on re-parse.
    if (std::isfinite(value) && value == std::floor(value) &&
        std::abs(value) < 9007199254740992.0 /* 2^53: integers are exact below this */) {
        std::array<char, 32> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%lld",
                      static_cast<long long>(value));
        out.append(buffer.data());
        return;
    }
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.17g", value);
    out.append(buffer.data());
}

void dumpInto(std::string& out, const Json& value) {
    switch (value.type()) {
        case Json::Type::Null:
            out.append("null");
            return;
        case Json::Type::Boolean:
            out.append(value.asBool() ? "true" : "false");
            return;
        case Json::Type::Number:
            numberInto(out, value.asNumber());
            return;
        case Json::Type::String:
            escapeInto(out, value.asString());
            return;
        case Json::Type::Array: {
            out.push_back('[');
            const auto& array = value.asArray();
            for (std::size_t index = 0; index < array.size(); ++index) {
                if (index != 0U) out.push_back(',');
                dumpInto(out, array[index]);
            }
            out.push_back(']');
            return;
        }
        case Json::Type::Object: {
            out.push_back('{');
            const auto& members = value.asObject();
            for (std::size_t index = 0; index < members.size(); ++index) {
                if (index != 0U) out.push_back(',');
                escapeInto(out, members[index].first);
                out.push_back(':');
                dumpInto(out, members[index].second);
            }
            out.push_back('}');
            return;
        }
    }
}

class Parser final {
  public:
    explicit Parser(std::string_view text) : text_(text) {}

    Json parse() {
        skipWhitespace();
        Json value = parseValue();
        skipWhitespace();
        if (position_ != text_.size()) {
            fail("trailing characters after JSON document");
        }
        return value;
    }

  private:
    Json parseValue() {
        skipWhitespace();
        if (position_ >= text_.size()) {
            fail("unexpected end of input");
        }
        const char c = text_[position_];
        switch (c) {
            case '{':
                return parseObject();
            case '[':
                return parseArray();
            case '"':
                return Json{parseString()};
            case 't':
            case 'f':
                return parseBoolean();
            case 'n':
                return parseNull();
            default:
                return parseNumber();
        }
    }

    Json parseObject() {
        expect('{');
        Json::Object members;
        skipWhitespace();
        if (peek() == '}') {
            ++position_;
            return Json{std::move(members)};
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                fail("expected string key in object");
            }
            std::string key = parseString();
            skipWhitespace();
            expect(':');
            members.emplace_back(std::move(key), parseValue());
            skipWhitespace();
            const char next = peek();
            if (next == ',') {
                ++position_;
                continue;
            }
            if (next == '}') {
                ++position_;
                break;
            }
            fail("expected ',' or '}' in object");
        }
        return Json{std::move(members)};
    }

    Json parseArray() {
        expect('[');
        Json::Array elements;
        skipWhitespace();
        if (peek() == ']') {
            ++position_;
            return Json{std::move(elements)};
        }
        while (true) {
            elements.push_back(parseValue());
            skipWhitespace();
            const char next = peek();
            if (next == ',') {
                ++position_;
                continue;
            }
            if (next == ']') {
                ++position_;
                break;
            }
            fail("expected ',' or ']' in array");
        }
        return Json{std::move(elements)};
    }

    std::string parseString() {
        expect('"');
        std::string result;
        while (true) {
            if (position_ >= text_.size()) {
                fail("unterminated string");
            }
            const char c = text_[position_++];
            if (c == '"') {
                break;
            }
            if (c == '\\') {
                if (position_ >= text_.size()) {
                    fail("unterminated escape sequence");
                }
                const char escape = text_[position_++];
                switch (escape) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    case 'u': appendUnicode(result); break;
                    default: fail("invalid escape sequence");
                }
            } else {
                result.push_back(c);
            }
        }
        return result;
    }

    void appendUnicode(std::string& result) {
        const std::uint32_t code = parseHex4();
        std::uint32_t codepoint = code;
        // Surrogate pair handling for characters outside the BMP.
        if (code >= 0xD800U && code <= 0xDBFFU) {
            if (position_ + 1U < text_.size() && text_[position_] == '\\' &&
                text_[position_ + 1U] == 'u') {
                position_ += 2U;
                const std::uint32_t low = parseHex4();
                codepoint = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
            }
        }
        // Encode as UTF-8.
        if (codepoint <= 0x7FU) {
            result.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            result.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            result.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            result.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            result.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    std::uint32_t parseHex4() {
        std::uint32_t value = 0U;
        for (int i = 0; i < 4; ++i) {
            if (position_ >= text_.size()) {
                fail("unterminated unicode escape");
            }
            const char c = text_[position_++];
            value <<= 4U;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                fail("invalid hex digit in unicode escape");
            }
        }
        return value;
    }

    Json parseNumber() {
        const std::size_t start = position_;
        if (peek() == '-') {
            ++position_;
        }
        while (position_ < text_.size()) {
            const char c = text_[position_];
            const bool numeric = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                                 c == '+' || c == '-';
            if (!numeric) {
                break;
            }
            ++position_;
        }
        if (position_ == start) {
            fail("invalid number");
        }
        const std::string token{text_.substr(start, position_ - start)};
        try {
            return Json{std::stod(token)};
        } catch (const std::exception&) {
            fail("invalid number literal");
        }
        return Json{}; // unreachable
    }

    Json parseBoolean() {
        if (matchLiteral("true")) {
            return Json{true};
        }
        if (matchLiteral("false")) {
            return Json{false};
        }
        fail("invalid literal");
        return Json{}; // unreachable
    }

    Json parseNull() {
        if (matchLiteral("null")) {
            return Json{};
        }
        fail("invalid literal");
        return Json{}; // unreachable
    }

    bool matchLiteral(std::string_view literal) {
        if (text_.substr(position_, literal.size()) == literal) {
            position_ += literal.size();
            return true;
        }
        return false;
    }

    void skipWhitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++position_;
            } else if (c == '/' && position_ + 1U < text_.size() && text_[position_ + 1U] == '/') {
                // Bedrock geometry files sometimes carry `//` comments.
                position_ += 2U;
                while (position_ < text_.size() && text_[position_] != '\n') {
                    ++position_;
                }
            } else {
                break;
            }
        }
    }

    [[nodiscard]] char peek() const {
        return position_ < text_.size() ? text_[position_] : '\0';
    }

    void expect(char c) {
        if (peek() != c) {
            fail(std::string{"expected '"} + c + "'");
        }
        ++position_;
    }

    [[noreturn]] void fail(const std::string& message) const {
        std::size_t line = 1U;
        std::size_t column = 1U;
        for (std::size_t i = 0U; i < position_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1U;
            } else {
                ++column;
            }
        }
        throw std::runtime_error("JSON parse error at line " + std::to_string(line) + ", column " +
                                 std::to_string(column) + ": " + message);
    }

    std::string_view text_;
    std::size_t position_ = 0U;
};

} // namespace

const Json& Json::operator[](std::string_view key) const {
    if (type_ != Type::Object) {
        return nullValue();
    }
    for (const auto& [name, value] : object_) {
        if (name == key) {
            return value;
        }
    }
    return nullValue();
}

bool Json::contains(std::string_view key) const {
    if (type_ != Type::Object) {
        return false;
    }
    for (const auto& [name, value] : object_) {
        if (name == key) {
            return true;
        }
    }
    return false;
}

const Json& Json::operator[](std::size_t index) const {
    if (type_ != Type::Array || index >= array_.size()) {
        return nullValue();
    }
    return array_[index];
}

std::size_t Json::size() const {
    if (type_ == Type::Array) {
        return array_.size();
    }
    if (type_ == Type::Object) {
        return object_.size();
    }
    return 0U;
}

Json Json::parse(std::string_view text) {
    g_parseCount.fetch_add(1U, std::memory_order_relaxed);
    Parser parser{text};
    return parser.parse();
}

std::uint64_t Json::parseCount() {
    return g_parseCount.load(std::memory_order_relaxed);
}

std::string Json::dump() const {
    std::string out;
    dumpInto(out, *this);
    return out;
}

} // namespace mc::core
