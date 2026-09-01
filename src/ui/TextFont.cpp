#include "ui/TextFont.hpp"

#include <algorithm>

namespace mc::ui {
namespace {

constexpr char32_t kReplacementCharacter = 0xFFFD;

// 完全没有字形时 vanilla 采用的宽度
constexpr float kSpaceAdvance = 4.0F;
constexpr float kMissingAdvance = 8.0F;

} // namespace

void appendUtf8(std::string& out, char32_t codepoint) {
    const auto value = static_cast<std::uint32_t>(codepoint);
    if (value < 0x80U) {
        out.push_back(static_cast<char>(value));
    } else if (value < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (value >> 6U)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (value >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (value >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    }
}

std::vector<char32_t> decodeUtf8(std::string_view text) {
    std::vector<char32_t> codepoints;
    codepoints.reserve(text.size());
    std::size_t index = 0U;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1U;
        char32_t codepoint = lead;
        if (lead >= 0xF0U) {
            length = 4U;
            codepoint = lead & 0x07U;
        } else if (lead >= 0xE0U) {
            length = 3U;
            codepoint = lead & 0x0FU;
        } else if (lead >= 0xC0U) {
            length = 2U;
            codepoint = lead & 0x1FU;
        } else if (lead >= 0x80U) {
            // 落单的续接字节不能作为一个序列的开头
            codepoints.push_back(kReplacementCharacter);
            ++index;
            continue;
        }
        if (index + length > text.size()) {
            codepoints.push_back(kReplacementCharacter);
            break;
        }
        bool valid = true;
        for (std::size_t offset = 1U; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3FU);
        }
        if (!valid) {
            codepoints.push_back(kReplacementCharacter);
            ++index;
            continue;
        }
        codepoints.push_back(codepoint);
        index += length;
    }
    return codepoints;
}

void TextFont::setUnicodeSizes(std::vector<std::uint8_t> sizes) {
    sizes_ = std::move(sizes);
    if (!pageLayersInitialized_) {
        clearUnicodePages();
    }
}

void TextFont::setUnicodePageLayer(int page, int layer) {
    if (!pageLayersInitialized_) {
        clearUnicodePages();
    }
    if (page < 0 || page >= static_cast<int>(kUnicodePageCount)) {
        return;
    }
    pageLayers_[static_cast<std::size_t>(page)] = layer;
}

void TextFont::clearUnicodePages() {
    pageLayers_.fill(-1);
    pageLayersInitialized_ = true;
}

bool TextFont::useUnicodeFor(char32_t codepoint) const {
    if (sizes_.size() < 0x10000U || codepoint > 0xFFFF) {
        return false;
    }
    if (!pageLayersInitialized_ || pageLayers_[static_cast<std::size_t>(codepoint >> 8U)] < 0) {
        return false;
    }
    // 除非玩家强制 unicode，ASCII 一直用它那张锐利的 128x128 表
    return forceUnicode_ || codepoint > 0x7F;
}

FontGlyph TextFont::asciiGlyph(char32_t codepoint) const {
    auto character = static_cast<unsigned char>(codepoint);
    if (codepoint < 32U || codepoint > 126U) {
        character = static_cast<unsigned char>('?');
    }
    const auto& metrics = ascii_.glyph(character);
    FontGlyph result;
    result.layer = 0.0F;
    result.u = metrics.u;
    result.v = metrics.v;
    result.uvWidth = metrics.uvWidth;
    result.uvHeight = metrics.uvHeight;
    result.pixelWidth = metrics.pixelWidth;
    result.pixelHeight = metrics.pixelHeight;
    result.advance = metrics.advance;
    result.visible = metrics.pixelWidth > 0.0F;
    return result;
}

FontGlyph TextFont::unicodeGlyph(char32_t codepoint) const {
    // 对应 LegacyUnicodeBitmapFontProvider：高半字节是首列，低半字节是末列
    // 字形按其像素尺寸的一半绘制
    const auto packed = sizes_[static_cast<std::size_t>(codepoint)];
    const auto left = static_cast<float>((packed >> 4U) & 0x0FU);
    const auto right = static_cast<float>((packed & 0x0FU) + 1U);
    FontGlyph result;
    result.layer = static_cast<float>(pageLayers_[static_cast<std::size_t>(codepoint >> 8U)]);
    if (packed == 0U) {
        result.advance = codepoint == U' ' ? kSpaceAdvance : kMissingAdvance;
        return result;
    }
    const float column = static_cast<float>((codepoint & 0x0FU));
    const float row = static_cast<float>((codepoint >> 4U) & 0x0FU);
    const float cellX = column * kUnicodeCellSize;
    const float cellY = row * kUnicodeCellSize;
    result.u = (cellX + left) / kUnicodePageSize;
    result.v = cellY / kUnicodePageSize;
    result.uvWidth = (right - left) / kUnicodePageSize;
    result.uvHeight = kUnicodeCellSize / kUnicodePageSize;
    result.pixelWidth = (right - left) / kUnicodeOversample;
    result.pixelHeight = kUnicodeCellSize / kUnicodeOversample;
    // Java 的步进值是 width / 2 + 1，用的是整数除法
    result.advance = static_cast<float>(static_cast<int>(right - left) / 2 + 1);
    result.visible = result.pixelWidth > 0.0F;
    return result;
}

FontGlyph TextFont::glyph(char32_t codepoint) const {
    if (const auto advance = spaceAdvances_.find(codepoint); advance != spaceAdvances_.end()) {
        FontGlyph result;
        result.advance = advance->second;
        return result;
    }
    // 即使畸形的定义漏掉了空格，也保留 vanilla 的默认值
    if (codepoint == U' ') {
        FontGlyph result;
        result.advance = kSpaceAdvance;
        return result;
    }
    if (const auto bitmap = bitmapGlyphs_.find(codepoint); bitmap != bitmapGlyphs_.end()) {
        return bitmap->second;
    }
    if (useUnicodeFor(codepoint)) {
        return unicodeGlyph(codepoint);
    }
    return asciiGlyph(codepoint);
}

float TextFont::textWidth(std::string_view text, float scale) const {
    float width = 0.0F;
    for (const char32_t codepoint : decodeUtf8(text)) {
        width += glyph(codepoint).advance * scale;
    }
    return width;
}

} // namespace mc::ui
