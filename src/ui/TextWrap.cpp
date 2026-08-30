#include "ui/TextWrap.hpp"

#include <algorithm>

namespace mc::ui {

namespace {

// 以 lead 开头的那个 UTF-8 序列有多少字节
// 续接字节与畸形字节一律算作一个字节，坏字符串因此仍能往前走而不会原地打转
// wrapText 只需要码点边界，不需要解码出来的值
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

        // 这个字形会溢出就在它之前断行
        // 但空行上绝不断，超宽的单个字形因此仍能独占一行
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
