#include "gameplay/command/StringReader.hpp"

#include <cassert>
#include <string>

using mc::gameplay::command::StringReader;

int main() {
    // 游标原语：peek/read/canRead 与 cursor 推进。
    StringReader basic{"hello world"};
    assert(basic.canRead());
    assert(basic.peek() == 'h');
    assert(basic.read() == 'h');
    assert(basic.cursor() == 1U);
    assert(basic.remaining() == "ello world");
    basic.skipWhitespace(); // 当前停在 'e'，不是空白
    assert(basic.cursor() == 1U);

    // skipWhitespace 越过空格与制表符。
    StringReader spaces{"  \t"};
    spaces.skipWhitespace();
    assert(!spaces.canRead());

    // readUnquotedString 读到空白或字符集边界为止。
    StringReader unquoted{"one two"};
    assert(unquoted.readUnquotedString() == "one");
    assert(unquoted.remaining() == " two");
    unquoted.skipWhitespace();
    assert(unquoted.readUnquotedString() == "two");
    assert(!unquoted.canRead());

    // 命名空间标识符整体是一个 token（冒号、下划线都在允许集内）。
    StringReader identifier{"minecraft:acacia_planks"};
    assert(identifier.readUnquotedString() == "minecraft:acacia_planks");

    // readString：无引号时退化为 readUnquotedString。
    StringReader plain{"gamemode survival"};
    assert(plain.readString() == "gamemode");
    plain.skipWhitespace();
    assert(plain.readString() == "survival");
    assert(!plain.canRead());

    // 双引号字符串：参数可以携带空格。
    StringReader quoted{"\"hello world\" tail"};
    assert(quoted.readString() == "hello world");
    quoted.skipWhitespace();
    assert(quoted.readString() == "tail");

    // 单引号字符串与双引号等价。
    StringReader single{"'hello world'"};
    assert(single.readString() == "hello world");

    // 反斜杠转义引号本身：输入 "say \"hi\"" → say "hi"。
    StringReader escaped{R"("say \"hi\"")"};
    assert(escaped.readString() == "say \"hi\"");
    assert(!escaped.canRead());

    // 反斜杠转义反斜杠：输入 "back\\slash" → back\slash。
    StringReader backslash{R"("back\\slash")"};
    assert(backslash.readString() == R"(back\slash)");

    // 未闭合字符串返回 nullopt，游标停在输入末尾。
    StringReader unclosed{"\"unclosed"};
    assert(!unclosed.readString().has_value());
    assert(unclosed.cursor() == unclosed.input().size());

    // 非法转义（\q）返回 nullopt，游标停回非法字符上。
    StringReader invalidEscape{R"("bad\q")"};
    assert(!invalidEscape.readString().has_value());
    assert(invalidEscape.peek() == 'q');

    // readQuotedString 要求停在引号处。
    StringReader mustQuote{"\"quoted\""};
    assert(mustQuote.readQuotedString() == "quoted");
    StringReader notQuote{"plain"};
    assert(!notQuote.readQuotedString().has_value());

    // 空输入：readString 返回空字符串，不失败。
    StringReader empty{""};
    assert(!empty.canRead());
    assert(empty.readString().has_value() && empty.readString()->empty());

    // readCoordinate：坐标 token 的专用读取器（`~` 不在无引号字符集里）。
    StringReader coords{"~5.5 64 -100 ~"};
    assert(coords.readCoordinate() == "~5.5");
    coords.skipWhitespace();
    assert(coords.readCoordinate() == "64");
    coords.skipWhitespace();
    assert(coords.readCoordinate() == "-100");
    coords.skipWhitespace();
    assert(coords.readCoordinate() == "~");
    assert(!coords.canRead());
    StringReader notCoordinate{"pig 5"};
    assert(notCoordinate.readCoordinate().empty()); // 字母不是坐标开头
    assert(notCoordinate.cursor() == 0U);           // 未消费输入
    assert(notCoordinate.readString() == "pig");

    // readToEnd：消费到行尾并返回剩余（greedy 字符串参数用）。
    StringReader greedy{"hello world 123"};
    assert(greedy.readToEnd() == "hello world 123");
    assert(!greedy.canRead());
    StringReader trailing{"  spaced  "};
    trailing.skipWhitespace();
    assert(trailing.readToEnd() == "spaced  "); // 保留内部与尾部空格

    // vanilla isAllowedInUnquotedString 的允许/拒绝字符集。
    for (const char allowed : std::string{"abcdefghijklmnopqrstuvwxyz"
                                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                          "0123456789._-+;:@"}) {
        assert(StringReader::isAllowedInUnquotedString(allowed));
    }
    for (const char denied : std::string{" \t\"'#/?!,*()[]{}="}) {
        assert(!StringReader::isAllowedInUnquotedString(denied));
    }
    assert(StringReader::isQuotedStringStart('"'));
    assert(StringReader::isQuotedStringStart('\''));
    assert(!StringReader::isQuotedStringStart('x'));
    return 0;
}
