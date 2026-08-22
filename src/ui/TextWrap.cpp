#include "ui/TextWrap.hpp"

#include <algorithm>

namespace mc::ui {

namespace {

// Byte length of the UTF-8 sequence beginning at `lead`. Continuation and
// malformed bytes count as one byte so a bad string still advances instead of
// looping; wrapText only needs codepoint boundaries, not decoded values.
std::size_t utf8SequenceLength(unsigned char lead) {
    if (lead < 0x80U) return 1U;
    if ((lead & 0xE0U) == 0xC0U) return 2U;
    if ((lead & 0xF0U) == 0xE0U) return 3U;
    if ((lead & 0xF8U) == 0xF0U) return 4U;
    return 1U;
}

} // namespace

std::vector<std::string> wrapText(std::string_view text, float maxWidth,
                                  const std::function<float(std::string_view)>& measure) {
    std::vector<std::string> lines;
    if (text.empty()) {
        return lines;
    }

    std::size_t lineStart = 0;       // byte offset where the current line begins
    float lineWidth = 0.0F;          // measured width of text[lineStart, cursor)
    std::size_t lastSpace = std::string_view::npos; // byte offset of last ' ' on the line
    float widthThroughSpace = 0.0F;  // lineWidth captured just after that space

    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t length =
            std::min(utf8SequenceLength(static_cast<unsigned char>(text[cursor])),
                     text.size() - cursor);
        const std::string_view glyph = text.substr(cursor, length);
        const float glyphWidth = measure(glyph);

        // Break before this glyph when it would overflow — but never on an empty
        // line, so an oversized single glyph still gets its own row.
        if (cursor > lineStart && lineWidth + glyphWidth > maxWidth) {
            if (lastSpace != std::string_view::npos) {
                lines.emplace_back(text.substr(lineStart, lastSpace - lineStart));
                lineStart = lastSpace + 1U; // skip the single break space
                lineWidth -= widthThroughSpace;
                lastSpace = std::string_view::npos;
            } else {
                lines.emplace_back(text.substr(lineStart, cursor - lineStart));
                lineStart = cursor;
                lineWidth = 0.0F;
            }
        }

        lineWidth += glyphWidth;
        if (length == 1U && text[cursor] == ' ') {
            lastSpace = cursor;
            widthThroughSpace = lineWidth;
        }
        cursor += length;
    }

    lines.emplace_back(text.substr(lineStart));
    return lines;
}

} // namespace mc::ui
