#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace mc::ui {

struct GlyphMetrics final {
    float u = 0.0F;
    float v = 0.0F;
    float uvWidth = 0.0F;
    float uvHeight = 0.0F;
    float pixelWidth = 0.0F;
    float pixelHeight = 0.0F;
    float advance = 4.0F;
};

class BitmapFontMetrics final {
  public:
    BitmapFontMetrics() = default;

    [[nodiscard]] static BitmapFontMetrics fromRgba(
        std::span<const std::uint8_t> rgba,
        int width,
        int height);

    [[nodiscard]] const GlyphMetrics& glyph(unsigned char character) const;
    [[nodiscard]] float textWidth(std::string_view text, float scale) const;

  private:
    std::array<GlyphMetrics, 256> glyphs_{};
};

} // namespace mc::ui
