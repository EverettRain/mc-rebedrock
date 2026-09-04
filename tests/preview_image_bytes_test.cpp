// RN-15b: the readback's byte transform.
//
// The export path needs a GPU and cannot run here, so this covers the piece of it
// that is pure arithmetic — and the piece where a bug is hardest to see, because
// a red/blue swap yields a perfectly plausible picture that is simply the wrong
// colour, and a zeroed alpha yields a PNG that looks like an empty file.

#include "render/PreviewImageBytes.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

int main() {
    using mc::render::kFormatB8G8R8A8Srgb;
    using mc::render::kFormatB8G8R8A8Unorm;
    using mc::render::normalizePreviewPixels;
    using mc::render::readbackNeedsRedBlueSwap;

    // Which formats need the swap. VK_FORMAT_R8G8B8A8_UNORM is 37 and
    // _SRGB is 43; neither may be swapped, or every RGBA-surface export comes out
    // with red and blue exchanged.
    assert(readbackNeedsRedBlueSwap(kFormatB8G8R8A8Unorm));
    assert(readbackNeedsRedBlueSwap(kFormatB8G8R8A8Srgb));
    assert(!readbackNeedsRedBlueSwap(37U));
    assert(!readbackNeedsRedBlueSwap(43U));
    assert(!readbackNeedsRedBlueSwap(0U));

    // BGRA in, RGBA out. Two pixels, so an off-by-one stride shows up.
    {
        std::array<std::uint8_t, 8> pixels{
            10U, 20U, 30U, 0U,   // B G R A
            40U, 50U, 60U, 7U,
        };
        normalizePreviewPixels(std::span{pixels}, /*swapRedAndBlue=*/true);
        const std::array<std::uint8_t, 8> expected{30U, 20U, 10U, 255U, 60U, 50U, 40U, 255U};
        assert(pixels == expected);
    }

    // RGBA in: the colour bytes are untouched, only alpha is forced.
    {
        std::array<std::uint8_t, 4> pixels{10U, 20U, 30U, 0U};
        normalizePreviewPixels(std::span{pixels}, /*swapRedAndBlue=*/false);
        const std::array<std::uint8_t, 4> expected{10U, 20U, 30U, 255U};
        assert(pixels == expected);
    }

    // The green channel never moves under the swap, whatever the values.
    {
        std::vector<std::uint8_t> pixels;
        for (int value = 0; value < 256; ++value) {
            pixels.push_back(static_cast<std::uint8_t>(value));
            pixels.push_back(static_cast<std::uint8_t>(255 - value));
            pixels.push_back(static_cast<std::uint8_t>((value * 7) % 256));
            pixels.push_back(0U);
        }
        const std::vector<std::uint8_t> before = pixels;
        normalizePreviewPixels(std::span{pixels}, /*swapRedAndBlue=*/true);
        for (std::size_t index = 0; index + 4U <= pixels.size(); index += 4U) {
            assert(pixels[index] == before[index + 2U]);
            assert(pixels[index + 1U] == before[index + 1U]);
            assert(pixels[index + 2U] == before[index]);
            assert(pixels[index + 3U] == 255U);
        }
        // Swapping twice is the identity apart from the alpha, so a transform
        // that "helpfully" also touched a colour channel would not survive here.
        std::vector<std::uint8_t> twice = pixels;
        normalizePreviewPixels(std::span{twice}, /*swapRedAndBlue=*/true);
        normalizePreviewPixels(std::span{twice}, /*swapRedAndBlue=*/true);
        assert(twice == pixels);
    }

    // A trailing partial pixel is left alone rather than read past. A readback
    // buffer is always a whole number of pixels, but a loop that reads one byte
    // beyond its span is a bug whether or not today's caller can reach it.
    {
        std::array<std::uint8_t, 7> pixels{1U, 2U, 3U, 4U, 5U, 6U, 7U};
        normalizePreviewPixels(std::span{pixels}, /*swapRedAndBlue=*/true);
        const std::array<std::uint8_t, 7> expected{3U, 2U, 1U, 255U, 5U, 6U, 7U};
        assert(pixels == expected);
    }

    // An empty buffer is not a crash.
    {
        std::vector<std::uint8_t> pixels;
        normalizePreviewPixels(std::span{pixels}, true);
        assert(pixels.empty());
    }

    return 0;
}
