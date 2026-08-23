#pragma once

// Pure, dependency-free helpers that keep the block atlas robust against an
// off-spec resource pack. Every layer of the block/entity/effect texture array
// is a fixed reference-sized tile (grass_block_top's dimensions); the layers
// are concatenated raw, so a layer of any other size would corrupt the atlas.
// A pack is free to ship a texture at a different resolution (an HD pack, a
// stray animation strip, or the magenta missing-texture placeholder standing
// in at 16x16 for an HD atlas). Rather than aborting the launch, the off-size
// layer is nearest-neighbour scaled to the reference size — the same "never
// crash on an off-spec pack" contract the fluid-animation fitter uses. Kept in
// their own header (free of Vulkan and the block/item registries) so they can
// be unit-tested headless.

#include "assets/ImageData.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string_view>

namespace mc::render {

// Nearest-neighbour scale of an image region into a target rectangle. The
// square-target convenience form below covers the entity/GUI unwraps that scale
// into square atlas tiles.
[[nodiscard]] inline assets::ImageData resizedRegion(const assets::ImageData& image, int sourceX,
                                                     int sourceY, int sourceWidth, int sourceHeight,
                                                     int targetWidth, int targetHeight) {
    assets::ImageData result;
    result.width = targetWidth;
    result.height = targetHeight;
    result.rgba.resize(static_cast<std::size_t>(targetWidth * targetHeight * 4));
    for (int y = 0; y < targetHeight; ++y) {
        for (int x = 0; x < targetWidth; ++x) {
            const int sx = sourceX + x * sourceWidth / targetWidth;
            const int sy = sourceY + y * sourceHeight / targetHeight;
            const std::size_t source = static_cast<std::size_t>((sy * image.width + sx) * 4);
            const std::size_t target = static_cast<std::size_t>((y * targetWidth + x) * 4);
            std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(source), 4,
                        result.rgba.begin() + static_cast<std::ptrdiff_t>(target));
        }
    }
    return result;
}

[[nodiscard]] inline assets::ImageData resizedRegion(const assets::ImageData& image, int sourceX,
                                                     int sourceY, int sourceWidth, int sourceHeight,
                                                     int targetSize) {
    return resizedRegion(image, sourceX, sourceY, sourceWidth, sourceHeight, targetSize,
                         targetSize);
}

// Returns a copy of `layer` guaranteed to match `reference`'s dimensions, so it
// can be concatenated into the fixed-size atlas without corrupting it. A layer
// that already matches is returned untouched; an off-size one is nearest-
// neighbour scaled (and a single line logged); a degenerate empty image falls
// back to the magenta missing-texture placeholder so the bake still cannot
// crash.
[[nodiscard]] inline assets::ImageData conformToAtlasLayer(const assets::ImageData& reference,
                                                           const assets::ImageData& layer,
                                                           std::string_view name) {
    if (layer.width == reference.width && layer.height == reference.height) {
        return layer;
    }
    std::cerr << "[texture-atlas] " << name << " resized " << layer.width << 'x' << layer.height
              << " -> " << reference.width << 'x' << reference.height << " to fit the atlas.\n";
    if (layer.width <= 0 || layer.height <= 0) {
        return assets::ImageData::missingTexture(reference.width, reference.height);
    }
    return resizedRegion(layer, 0, 0, layer.width, layer.height, reference.width, reference.height);
}

// A block texture streamed through the dynamic (registry-driven) path may be a
// vertical animation strip — magma, sea lantern, prismarine — whose height is a
// whole multiple of its width (one square frame stacked per row). The atlas's
// animated section is hardwired to the fluids (water/lava), so a non-fluid
// animated block cannot stream frames; baking its FIRST frame gives a correct
// static tile instead of vertically squashing the whole strip into one blurred
// layer (which is what conformToAtlasLayer alone would do). A texture that is
// not such a strip is conformed as usual.
[[nodiscard]] inline assets::ImageData conformBlockLayer(const assets::ImageData& reference,
                                                         const assets::ImageData& layer,
                                                         std::string_view name) {
    if (layer.width > 0 && layer.height > layer.width && layer.height % layer.width == 0) {
        std::cerr << "[texture-atlas] " << name << " is a " << (layer.height / layer.width)
                  << "-frame strip; baking frame 0 (block animation unsupported).\n";
        return resizedRegion(layer, 0, 0, layer.width, layer.width, reference.width,
                             reference.height);
    }
    return conformToAtlasLayer(reference, layer, name);
}

} // namespace mc::render
