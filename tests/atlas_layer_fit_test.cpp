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

// RN-8c-E: the ModelPart box-UV net, and which of its two cap rects is the
// world's up face.
//
// vanilla `ModelPart.Cube` lays a (w,h,d) box out at (u0,v0) as
//
//        [ DOWN ][  UP  ]
//   [-X][  -Z  ][  +X  ][  +Z  ]
//
// with DOWN at (u0+d, v0) and UP at (u0+d+w, v0). Which one a face shows in the
// WORLD depends on whether the caller draws through the entity root's Y flip:
// vanilla's EntityRenderer does `scale(-1,-1,1)` for mobs and players, so their
// world-up face reads the DOWN rect — that is why a skin's head top sits at
// (8,0). A block entity has no such flip (ChestRenderer only yaws), so a chest's
// world-up face reads the UP rect.
//
// The two were conflated: the chest reused the player's unpacking and hand-swapped
// only its lid, leaving the base's up face on the wrong rect. The numbers below
// were checked against entity/chest/normal.png pixel by pixel — the rect this
// says is the base's world-up is the one with the transparent chest mouth in it,
// and the one it says is the base's world-down is solid planks. (The texture is a
// vanilla asset and is not in this repo, so the check lives in the values here
// rather than in a fixture.)
void checkCuboidNet() {
    using mc::render::cuboidNetRects;
    using mc::render::NetRect;
    const auto same = [](const NetRect& a, const NetRect& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    };
    constexpr std::size_t kPlusX = 0;
    constexpr std::size_t kMinusX = 1;
    constexpr std::size_t kPlusY = 2;
    constexpr std::size_t kMinusY = 3;
    constexpr std::size_t kPlusZ = 4;
    constexpr std::size_t kMinusZ = 5;

    // A player head: texOffs(0,0), 8x8x8, drawn through the entity Y flip. Its
    // world-up face is the skin's head top, which every skin puts at (8,0) —
    // i.e. the net's DOWN rect.
    {
        const auto head = cuboidNetRects(0, 0, 8, 8, 8, /*entityYFlip=*/true);
        assert(same(head[kPlusY], {8, 0, 8, 8}));
        assert(same(head[kMinusY], {16, 0, 8, 8}));
        assert(same(head[kMinusX], {0, 8, 8, 8}));
        assert(same(head[kMinusZ], {24, 8, 8, 8}));
    }

    // The chest base: texOffs(0,19), 14x10x14, drawn with no flip. Its world-up
    // face — the one you look into when the lid opens — is the net's UP rect at
    // (28,19), the one carrying the chest mouth. (14,19) is solid planks, pixel
    // for pixel the same as the chest's outer top, which is exactly what the
    // wrong one looked like.
    {
        const auto base = cuboidNetRects(0, 19, 14, 10, 14, /*entityYFlip=*/false);
        assert(same(base[kPlusY], {28, 19, 14, 14}));
        assert(same(base[kMinusY], {14, 19, 14, 14}));
        assert(same(base[kPlusZ], {14, 33, 14, 10}));
        assert(same(base[kPlusX], {28, 33, 14, 10}));
    }

    // The chest lid: texOffs(0,0), 14x5x14, no flip. Its world-up is the chest's
    // outer top at (28,0); its world-down is the dark inside of the lid at (14,0).
    {
        const auto lid = cuboidNetRects(0, 0, 14, 5, 14, /*entityYFlip=*/false);
        assert(same(lid[kPlusY], {28, 0, 14, 14}));
        assert(same(lid[kMinusY], {14, 0, 14, 14}));
    }

    // The flip changes the two cap faces and nothing else — if it ever starts
    // moving a side face, the net itself has been rewritten.
    {
        const auto flipped = cuboidNetRects(3, 5, 14, 10, 14, true);
        const auto plain = cuboidNetRects(3, 5, 14, 10, 14, false);
        assert(same(flipped[kPlusY], plain[kMinusY]));
        assert(same(flipped[kMinusY], plain[kPlusY]));
        for (const std::size_t face : {kPlusX, kMinusX, kPlusZ, kMinusZ}) {
            assert(same(flipped[face], plain[face]));
        }
    }
}

int main() {
    checkCuboidNet();
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
