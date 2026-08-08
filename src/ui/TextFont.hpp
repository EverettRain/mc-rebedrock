#pragma once

#include "ui/BitmapFontMetrics.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mc::ui {

// Decodes UTF-8 into codepoints. Invalid bytes become U+FFFD so a malformed
// string still renders something instead of shifting every glyph after it.
[[nodiscard]] std::vector<char32_t> decodeUtf8(std::string_view text);

// One glyph resolved against the font texture array.
struct FontGlyph final {
    // Layer in the font texture array: 0 is the ASCII page, the unicode pages
    // follow in the order the renderer uploaded them.
    float layer = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    float uvWidth = 0.0F;
    float uvHeight = 0.0F;
    // Size and advance in GUI pixels, before the GUI scale is applied.
    float pixelWidth = 0.0F;
    float pixelHeight = 0.0F;
    float advance = 4.0F;
    bool visible = false;
};

// The Java 1.16.1 font stack: the 128x128 ascii.png sheet for Latin text plus
// the legacy unicode pages (unicode_page_XX.png driven by glyph_sizes.bin) for
// everything else. Turning on forceUnicode routes ASCII through the unicode
// pages too, which is how vanilla renders CJK languages consistently.
class TextFont final {
  public:
    static constexpr std::size_t kUnicodePageCount = 256U;
    // A unicode page is a 16x16 grid of 16x16 pixel cells, drawn at half size.
    static constexpr float kUnicodePageSize = 256.0F;
    static constexpr float kUnicodeCellSize = 16.0F;
    static constexpr float kUnicodeOversample = 2.0F;

    void setAsciiMetrics(BitmapFontMetrics metrics) { ascii_ = metrics; }
    // glyph_sizes.bin: one byte per BMP codepoint, high nibble is the first
    // used column and low nibble the last.
    void setUnicodeSizes(std::vector<std::uint8_t> sizes);
    // Assigns the texture array layer a page was uploaded to. Pages that were
    // never uploaded keep their -1 and fall back to the ASCII sheet.
    void setUnicodePageLayer(int page, int layer);
    void clearUnicodePages();
    void setForceUnicode(bool force) { forceUnicode_ = force; }

    [[nodiscard]] bool forceUnicode() const { return forceUnicode_; }
    [[nodiscard]] bool hasUnicodePages() const { return !sizes_.empty(); }

    [[nodiscard]] FontGlyph glyph(char32_t codepoint) const;
    [[nodiscard]] float textWidth(std::string_view text, float scale) const;
    // The height a line occupies in GUI pixels, which stays at the vanilla 8
    // regardless of which sheet supplied the glyph.
    [[nodiscard]] static constexpr float lineHeight() { return 8.0F; }

  private:
    [[nodiscard]] bool useUnicodeFor(char32_t codepoint) const;
    [[nodiscard]] FontGlyph asciiGlyph(char32_t codepoint) const;
    [[nodiscard]] FontGlyph unicodeGlyph(char32_t codepoint) const;

    BitmapFontMetrics ascii_;
    std::vector<std::uint8_t> sizes_;
    std::array<int, kUnicodePageCount> pageLayers_{};
    bool forceUnicode_ = false;
    bool pageLayersInitialized_ = false;
};

} // namespace mc::ui
