#include "ui/BitmapFontMetrics.hpp"

#include <algorithm>
#include <stdexcept>

namespace mc::ui {

BitmapFontMetrics BitmapFontMetrics::fromRgba(
    std::span<const std::uint8_t> rgba,
    int width,
    int height) {
    if (width <= 0 || height <= 0 || width % 16 != 0 || height % 16 != 0 ||
        rgba.size() != static_cast<std::size_t>(width * height * 4)) {
        throw std::invalid_argument("Bitmap font must be a 16x16 RGBA glyph grid");
    }
    const int cellWidth = width / 16;
    const int cellHeight = height / 16;
    BitmapFontMetrics result;
    for (int character = 0; character < 256; ++character) {
        const int cellX = (character % 16) * cellWidth;
        const int cellY = (character / 16) * cellHeight;
        int minimumX = cellWidth;
        int maximumX = -1;
        for (int y = 0; y < cellHeight; ++y) {
            for (int x = 0; x < cellWidth; ++x) {
                const auto pixelIndex = static_cast<std::size_t>(
                    ((cellY + y) * width + cellX + x) * 4 + 3);
                if (rgba[pixelIndex] == 0U) {
                    continue;
                }
                minimumX = std::min(minimumX, x);
                maximumX = std::max(maximumX, x);
            }
        }

        GlyphMetrics metrics;
        metrics.v = static_cast<float>(cellY) / static_cast<float>(height);
        metrics.uvHeight = static_cast<float>(cellHeight) / static_cast<float>(height);
        metrics.pixelHeight = static_cast<float>(cellHeight);
        if (maximumX >= minimumX) {
            const int glyphWidth = maximumX - minimumX + 1;
            metrics.u = static_cast<float>(cellX + minimumX) /
                        static_cast<float>(width);
            metrics.uvWidth = static_cast<float>(glyphWidth) /
                              static_cast<float>(width);
            metrics.pixelWidth = static_cast<float>(glyphWidth);
            metrics.advance = static_cast<float>(glyphWidth + 1);
        } else {
            metrics.u = static_cast<float>(cellX) / static_cast<float>(width);
            metrics.advance = character == ' ' ? 4.0F : 3.0F;
        }
        result.glyphs_[static_cast<std::size_t>(character)] = metrics;
    }
    return result;
}

const GlyphMetrics& BitmapFontMetrics::glyph(unsigned char character) const {
    return glyphs_[character];
}

float BitmapFontMetrics::textWidth(std::string_view text, float scale) const {
    float width = 0.0F;
    for (const char characterValue : text) {
        unsigned char character = static_cast<unsigned char>(characterValue);
        if (character < 32U || character > 126U) {
            character = static_cast<unsigned char>('?');
        }
        width += glyph(character).advance * scale;
    }
    return width;
}

} // namespace mc::ui
