#include "render/vulkan/AtlasLayerFit.hpp"

#include "assets/ImageData.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

// The block atlas is a fixed-size texture array: every layer must be exactly
// grass_block_top's dimensions, and the layers are concatenated raw. An
// off-spec resource pack that ships a block texture at a different resolution
// used to abort the entire launch with "Grass block texture layers must have
// identical dimensions". These pin the robust replacement: a layer of any size
// is conformed to the reference so the bake never crashes.
namespace {

using mc::assets::ImageData;
using mc::render::conformToAtlasLayer;

// A solid image filled with one RGBA colour, so a nearest-neighbour resize can
// be verified by sampling any pixel.
ImageData solid(int width, int height, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                std::uint8_t a) {
    ImageData image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    for (std::size_t i = 0; i + 3U < image.rgba.size(); i += 4U) {
        image.rgba[i] = r;
        image.rgba[i + 1U] = g;
        image.rgba[i + 2U] = b;
        image.rgba[i + 3U] = a;
    }
    return image;
}

bool matchesReference(const ImageData& fitted, const ImageData& reference) {
    return fitted.width == reference.width && fitted.height == reference.height &&
           fitted.rgba.size() == reference.rgba.size();
}

bool pixelIs(const ImageData& image, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const std::size_t i =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
         static_cast<std::size_t>(x)) *
        4U;
    return image.rgba[i] == r && image.rgba[i + 1U] == g && image.rgba[i + 2U] == b;
}

} // namespace

int main() {
    const auto reference = solid(16, 16, 10, 20, 30, 255);

    // --- Same size: returned untouched, byte-for-byte. ---
    {
        const auto layer = solid(16, 16, 99, 88, 77, 255);
        const auto fitted = conformToAtlasLayer(reference, layer, "same");
        assert(matchesReference(fitted, reference));
        assert(fitted.rgba == layer.rgba);
    }

    // --- Sabotage 1: an HD pack layer twice the reference resolution. The old
    //     requireSameSize threw here; now it is downscaled to fit. ---
    {
        const auto hd = solid(32, 32, 200, 100, 50, 255);
        const auto fitted = conformToAtlasLayer(reference, hd, "hd_double");
        assert(matchesReference(fitted, reference));
        // Nearest-neighbour of a solid image keeps the colour.
        assert(pixelIs(fitted, 0, 0, 200, 100, 50));
        assert(pixelIs(fitted, 15, 15, 200, 100, 50));
    }

    // --- Sabotage 2: a non-square layer (e.g. an animation strip mistaken for a
    //     single tile). Different width AND height still conforms. ---
    {
        const auto strip = solid(16, 64, 5, 6, 7, 255);
        const auto fitted = conformToAtlasLayer(reference, strip, "strip_16x64");
        assert(matchesReference(fitted, reference));
        assert(pixelIs(fitted, 8, 8, 5, 6, 7));
    }

    // --- Sabotage 3: a degenerate zero-size layer. Rather than dividing by zero
    //     or emitting an empty layer that corrupts the atlas, it falls back to
    //     the magenta missing-texture placeholder at the reference size. ---
    {
        ImageData empty;
        empty.width = 0;
        empty.height = 0;
        const auto fitted = conformToAtlasLayer(reference, empty, "empty");
        assert(matchesReference(fitted, reference));
        // The placeholder is the vanilla magenta/black marker.
        const auto marker = ImageData::missingTexture(reference.width, reference.height);
        assert(fitted.rgba == marker.rgba);
    }

    // --- A smaller-than-reference layer upscales the same way. ---
    {
        const auto small = solid(8, 8, 1, 2, 3, 255);
        const auto fitted = conformToAtlasLayer(reference, small, "small_8x8");
        assert(matchesReference(fitted, reference));
        assert(pixelIs(fitted, 15, 15, 1, 2, 3));
    }

    // RN-4b removed conformBlockLayer's "bake frame 0" cap: a vertical animation
    // strip (magma 16x48, prismarine, …) is now baked frame-by-frame into a
    // contiguous atlas run by bakeBlockAtlas, which records a BlockTextureAnimation
    // so the terrain shader cycles it. That path needs a full resource provider, so
    // it is exercised at runtime (Mac-gated) rather than here; this file keeps its
    // conformToAtlasLayer robustness coverage above.

    return 0;
}
