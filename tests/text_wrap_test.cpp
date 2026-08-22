#include "ui/TextWrap.hpp"

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

// wrapText breaks one logical line to a pixel width using an injected measurer,
// so it is testable with no font/Vulkan. The fake measurer here gives every
// ASCII byte a width of 6 (a UTF-8 lead byte is width 6, continuation bytes 0,
// so a multibyte codepoint also measures 6) — a clean grid for the assertions.
namespace {
float sixPerChar(std::string_view piece) {
    float width = 0.0F;
    for (const unsigned char byte : piece) {
        width += (byte & 0xC0U) == 0x80U ? 0.0F : 6.0F;
    }
    return width;
}
} // namespace

int main() {
    using mc::ui::wrapText;

    // Fits: a line within the width stays a single line, verbatim.
    {
        const auto lines = wrapText("hello world", 200.0F, sixPerChar);
        assert(lines.size() == 1U);
        assert(lines[0] == "hello world");
    }

    // Word wrap: breaks on the last space before overflow, and the break space
    // is dropped (not carried onto either line). "alpha beta gamma" at width 66
    // (11 chars) → "alpha beta" (10) then "gamma".
    {
        const auto lines = wrapText("alpha beta gamma", 66.0F, sixPerChar);
        assert(lines.size() == 2U);
        assert(lines[0] == "alpha beta");
        assert(lines[1] == "gamma");
    }

    // Long word with no break opportunity is split at char boundaries, filling
    // each row up to the width. Width 36 = 6 chars per row.
    {
        const auto lines = wrapText("abcdefghijklmn", 36.0F, sixPerChar);
        assert(lines.size() == 3U);
        assert(lines[0] == "abcdef");
        assert(lines[1] == "ghijkl");
        assert(lines[2] == "mn");
    }

    // A single glyph wider than the whole width still gets its own row rather
    // than looping forever (width 3 < one char's 6).
    {
        const auto lines = wrapText("abc", 3.0F, sixPerChar);
        assert(lines.size() == 3U);
        assert(lines[0] == "a");
        assert(lines[1] == "b");
        assert(lines[2] == "c");
    }

    // Multibyte codepoints break on codepoint boundaries, never mid-byte: "€"
    // is 3 UTF-8 bytes measuring 6. Width 6 → one euro sign per row, each row a
    // valid 3-byte sequence.
    {
        const std::string euros = "\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC"; // €€€
        const auto lines = wrapText(euros, 6.0F, sixPerChar);
        assert(lines.size() == 3U);
        for (const auto& line : lines) {
            assert(line == "\xE2\x82\xAC");
        }
    }

    // Empty input yields no lines (the chat renderer draws nothing extra).
    {
        assert(wrapText("", 100.0F, sixPerChar).empty());
    }

    return 0;
}
