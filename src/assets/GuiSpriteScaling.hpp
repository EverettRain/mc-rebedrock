#pragma once

#include "assets/ResourceLocation.hpp"

#include <string_view>

namespace mc::assets {

class ResourceProvider;

// How a GUI sprite fills a destination larger or smaller than its own art.
// 26.1 declares this per sprite in `<sprite>.png.mcmeta`; 1.16.1 had no such
// concept, which is why widget art used to be stretched (or split into halves
// by hand) and its 1px borders blurred at odd sizes.
enum class GuiSpriteScalingType {
    // Linear stretch of the whole image. The default when no mcmeta declares
    // otherwise, and what every fixed-size HUD element uses.
    Stretch,
    // Corners drawn 1:1, edges stretched/tiled along one axis, centre along
    // both — so a 3px button border stays 3px at any width.
    NineSlice,
    // The whole image repeated to fill the destination.
    Tile,
};

// Inset of the non-scaling frame, in sprite pixels. A scalar `border` in the
// mcmeta sets all four sides; an object sets them independently (26.1's
// slider_handle uses bottom 3, the rest 2).
struct GuiSpriteBorder final {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

// A parsed `gui.scaling` block. `width`/`height` are the sprite's reference
// size, which the borders are measured against; they are 0 when the mcmeta
// omits them, and a caller then falls back to the sprite's real pixel size.
struct GuiSpriteScaling final {
    GuiSpriteScalingType type = GuiSpriteScalingType::Stretch;
    int width = 0;
    int height = 0;
    GuiSpriteBorder border{};
    // nine_slice only: stretch the edge/centre regions instead of tiling them.
    // 26.1's tooltip/frame is the one vanilla sprite that sets it.
    bool stretchInner = false;

    // Parses the text of a .mcmeta. A file with no `gui.scaling` object, an
    // unknown `type`, or malformed JSON all read as the plain `stretch`
    // default — a bad pack must not change how the rest of the GUI draws.
    [[nodiscard]] static GuiSpriteScaling parse(std::string_view mcmeta);

    // Reads `<pngLocation>.mcmeta` through the provider. An absent sidecar is
    // the common case (most sprites are fixed-size) and yields `stretch`.
    [[nodiscard]] static GuiSpriteScaling load(const ResourceProvider& resources,
                                               const ResourceLocation& pngLocation);
};

} // namespace mc::assets
