#pragma once

#include "ui/BitmapFontMetrics.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
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
    float offsetX = 0.0F;
    float offsetY = 0.0F;
    float advance = 4.0F;
    bool visible = false;
};

// The Java 26.1 provider stack: bitmap sheets supply their declared codepoints,
// space providers supply advances, and unihex providers populate BMP pages from
// embedded .hex archives. Turning on forceUnicode selects font/uniform.json.
class TextFont final {
  public:
    static constexpr std::size_t kUnicodePageCount = 256U;
    // A unicode page is a 16x16 grid of 16x16 pixel cells, drawn at half size.
    static constexpr float kUnicodePageSize = 256.0F;
    static constexpr float kUnicodeCellSize = 16.0F;
    static constexpr float kUnicodeOversample = 2.0F;

    void setAsciiMetrics(BitmapFontMetrics metrics) { ascii_ = metrics; }
    // Compact unihex bounds: high nibble is the first used column and low
    // nibble the last.
    void setUnicodeSizes(std::vector<std::uint8_t> sizes);
    // Assigns the texture array layer a page was uploaded to. Pages that were
    // never uploaded keep their -1 and fall back to the ASCII sheet.
    void setUnicodePageLayer(int page, int layer);
    void clearUnicodePages();
    void clearBitmapGlyphs() { bitmapGlyphs_.clear(); }
    // Providers are visited in font JSON order. The first provider offering a
    // codepoint wins, matching FontSet's provider precedence.
    void addBitmapGlyph(char32_t codepoint, FontGlyph glyph) {
        bitmapGlyphs_.try_emplace(codepoint, glyph);
    }
    void clearSpaceAdvances() { spaceAdvances_.clear(); }
    void setSpaceAdvance(char32_t codepoint, float advance) {
        spaceAdvances_.insert_or_assign(codepoint, advance);
    }
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
    std::unordered_map<char32_t, FontGlyph> bitmapGlyphs_;
    std::unordered_map<char32_t, float> spaceAdvances_;
    bool forceUnicode_ = false;
    bool pageLayersInitialized_ = false;
};

} // namespace mc::ui
