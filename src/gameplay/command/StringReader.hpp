#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace mc::gameplay::command {

// 一个基于游标、在一行命令输入上向前滑动的分词器，对应 1.16.1 的
// com.mojang.brigadier.StringReader。命令按空白分词，但以引号开头的 token
// 会按带反斜杠转义的引号字符串读取，因此单个参数可以携带空格。reader 从不
// 复制输入，只推进游标并对输入返回视图；只有引号解析需要一个 std::string
// 用来拼装转义结果。
class StringReader final {
  public:
    explicit constexpr StringReader(std::string_view input) : input_(input) {}

    // 原始输入与当前游标。
    [[nodiscard]] constexpr std::string_view input() const { return input_; }
    [[nodiscard]] constexpr std::size_t cursor() const { return cursor_; }
    constexpr void setCursor(std::size_t cursor) { cursor_ = cursor; }

    // 从游标到输入末尾的剩余部分，以及自 `start` 起已消费的区间（补全用它
    // 计算一条建议相对整行的偏移）。
    [[nodiscard]] constexpr std::string_view remaining() const { return input_.substr(cursor_); }
    [[nodiscard]] constexpr std::size_t remainingLength() const {
        return input_.size() - cursor_;
    }
    [[nodiscard]] constexpr std::string_view readFrom(std::size_t start) const {
        return input_.substr(start, cursor_ - start);
    }

    [[nodiscard]] constexpr bool canRead() const { return cursor_ < input_.size(); }
    [[nodiscard]] constexpr bool canRead(std::size_t offset) const {
        return cursor_ + offset < input_.size();
    }

    [[nodiscard]] constexpr char peek() const { return input_[cursor_]; }
    [[nodiscard]] constexpr char peek(std::size_t offset) const {
        return input_[cursor_ + offset];
    }
    [[nodiscard]] constexpr char read() { return input_[cursor_++]; }
    constexpr void skip() {
        if (canRead()) {
            ++cursor_;
        }
    }

    // 越过空格与水平制表符。
    constexpr void skipWhitespace() {
        while (canRead() && isWhitespace(input_[cursor_])) {
            ++cursor_;
        }
    }

    // 读取一个 token。若游标停在引号处，按引号字符串读取（处理反斜杠转义）；
    // 否则读到下一个空白。引号字符串未闭合或含非法转义时返回 nullopt，并把
    // 游标停在该字符上（对应 Brigadier 的 setCursor(cursor - 1)）。
    [[nodiscard]] std::optional<std::string> readString() {
        if (!canRead()) {
            return std::string{};
        }
        if (isQuotedStringStart(peek())) {
            const char quote = peek();
            ++cursor_;
            return readStringUntil(quote);
        }
        return readUnquotedString();
    }

    // 读到下一个空白或超出允许字符集为止。输入结束时返回空字符串。
    [[nodiscard]] std::string readUnquotedString() {
        const std::size_t start = cursor_;
        while (canRead() && isAllowedInUnquotedString(peek())) {
            ++cursor_;
        }
        return std::string{input_.substr(start, cursor_ - start)};
    }

    // 消费到输入末尾并返回剩余部分——greedy 字符串参数（`/say <message>`）
    // 用它一次读完行尾（含内部空格）。与只读不进的 remaining() 不同。
    [[nodiscard]] std::string readToEnd() {
        const std::size_t start = cursor_;
        cursor_ = input_.size();
        return std::string{input_.substr(start)};
    }

    // 读取一个坐标 token，对应 1.16.1 Vec3Argument 的 readRelativeDouble：
    // 可选 `~` 相对前缀，后接有符号十进制数。`~` 刻意不在无引号字符串的
    // 允许字符集里，因此坐标需要自己的读取器。输入不以坐标开头时返回空串。
    [[nodiscard]] std::string readCoordinate() {
        const std::size_t start = cursor_;
        if (canRead() && peek() == '~') {
            ++cursor_;
        }
        while (canRead()) {
            const char character = peek();
            if ((character >= '0' && character <= '9') || character == '-' ||
                character == '.') {
                ++cursor_;
            } else {
                break;
            }
        }
        return std::string{input_.substr(start, cursor_ - start)};
    }

    // 读取一个引号字符串，要求游标停在引号处。
    [[nodiscard]] std::optional<std::string> readQuotedString() {
        if (!canRead() || !isQuotedStringStart(peek())) {
            return std::nullopt;
        }
        const char quote = peek();
        ++cursor_;
        return readStringUntil(quote);
    }

    // 无引号 token 内允许的字符，与 1.16.1 StringReader#isAllowedInUnquotedString
    // 的字符集一致（因此标识符、数字与命名空间都能在一个 token 里）。
    [[nodiscard]] static constexpr bool isAllowedInUnquotedString(char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') || character == '.' ||
               character == '-' || character == '_' || character == '+' ||
               character == ';' || character == ':' || character == '@';
    }

    [[nodiscard]] static constexpr bool isQuotedStringStart(char character) {
        return character == '"' || character == '\'';
    }

    [[nodiscard]] static constexpr bool isWhitespace(char character) {
        return character == ' ' || character == '\t';
    }

  private:
    // 从开引号之后读取到匹配的闭引号。反斜杠只转义闭引号与反斜杠本身，
    // 其余都是非法转义（与 Brigadier 的 readStringUntil 一致）。
    [[nodiscard]] std::optional<std::string> readStringUntil(char terminator) {
        std::string result;
        bool escaped = false;
        while (canRead()) {
            const char character = read();
            if (escaped) {
                if (character == terminator || character == '\\') {
                    result.push_back(character);
                    escaped = false;
                } else {
                    setCursor(cursor() - 1U);
                    return std::nullopt;
                }
            } else if (character == '\\') {
                escaped = true;
            } else if (character == terminator) {
                return result;
            } else {
                result.push_back(character);
            }
        }
        // 未闭合字符串：游标停在输入末尾。
        return std::nullopt;
    }

    std::string_view input_;
    std::size_t cursor_ = 0;
};

} // namespace mc::gameplay::command
