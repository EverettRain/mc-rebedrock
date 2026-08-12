#include "assets/ImageData.hpp"

#include <cassert>
#include <cstddef>

// The missing-texture fallback is what keeps an incomplete resource pack from
// crashing the atlas bake: a texture the pack (and the bundled base) lack loads
// as the vanilla magenta/black marker instead of throwing. These pin the marker
// pattern and that a missing file yields it rather than an exception.
int main() {
    using mc::assets::ImageData;

    const auto pixelIsMagenta = [](const ImageData& image, int x, int y) {
        const std::size_t i =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
             static_cast<std::size_t>(x)) *
            4U;
        return image.rgba[i] == 248U && image.rgba[i + 1U] == 0U && image.rgba[i + 2U] == 248U &&
               image.rgba[i + 3U] == 255U;
    };
    const auto pixelIsBlack = [](const ImageData& image, int x, int y) {
        const std::size_t i =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
             static_cast<std::size_t>(x)) *
            4U;
        return image.rgba[i] == 0U && image.rgba[i + 1U] == 0U && image.rgba[i + 2U] == 0U &&
               image.rgba[i + 3U] == 255U;
    };

    // --- The marker is a diagonal two-quadrant split of black and magenta. ---
    {
        const auto missing = ImageData::missingTexture(16, 16);
        assert(missing.width == 16 && missing.height == 16);
        assert(missing.rgba.size() == 16U * 16U * 4U);
        // (x<8) XOR (y<8): the two diagonal quadrants are magenta, the other two
        // black, so the marker reads the same at any scale.
        assert(pixelIsBlack(missing, 0, 0));    // top-left
        assert(pixelIsMagenta(missing, 8, 0));  // top-right
        assert(pixelIsMagenta(missing, 0, 8));  // bottom-left
        assert(pixelIsBlack(missing, 8, 8));    // bottom-right
        assert(pixelIsBlack(missing, 15, 15));
    }

    // --- It generates at whatever size the atlas layer needs. ---
    {
        const auto missing = ImageData::missingTexture(32, 32);
        assert(missing.width == 32 && missing.height == 32);
        assert(pixelIsBlack(missing, 0, 0));
        assert(pixelIsMagenta(missing, 16, 0));
    }

    // --- A missing file falls back to the marker rather than throwing. ---
    {
        const auto image =
            ImageData::loadRgbaOrMissing("this_texture_does_not_exist_zzz.png", 16, 16);
        assert(image.width == 16 && image.height == 16);
        assert(pixelIsBlack(image, 0, 0) && pixelIsMagenta(image, 8, 0));

        // The fallback size is honoured, so a caller can match the atlas layer.
        const auto sized = ImageData::loadRgbaOrMissing("nope_zzz.png", 8, 8);
        assert(sized.width == 8 && sized.height == 8);
    }

    return 0;
}
