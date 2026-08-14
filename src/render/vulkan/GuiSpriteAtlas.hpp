#pragma once

// Where the runtime-stretched 26.1 widget sprites landed in the compatibility
// GUI atlas, and how each one fills a destination.
//
// TextureManager reassembles 26.1's `gui/sprites/*` into the single 256x256
// layers this renderer samples with fixed pixel rectangles. That is exact for
// every fixed-size element (hearts, hotbar, container backgrounds), but menu
// buttons and slider tracks are drawn at widths that change with the window and
// the GUI scale. 26.1 answers that with per-sprite `gui.scaling` metadata, so
// the atlas rectangle alone is not enough — the HUD also needs the sprite's
// reference size and border to nine-slice it. This header is the small shared
// vocabulary for that, kept out of TextureManager so HudRenderer does not have
// to depend on the whole texture-upload subsystem.

#include "assets/GuiSpriteScaling.hpp"
#include "ui/HudLayout.hpp"

#include <array>
#include <cstddef>

namespace mc::render {

// One 26.1 sprite as it sits in the compatibility atlas: `region` is its pixel
// rectangle inside the 256x256 layer, `scaling` its parsed `.png.mcmeta`.
// ui::forEachGuiSpriteQuad turns the pair into draws.
struct GuiAtlasSprite final {
    ui::UiRect region{};
    assets::GuiSpriteScaling scaling{};
};

// The widget sprites the front-end draws at sizes it decides at runtime. Every
// other GUI sprite keeps its hard-coded atlas rectangle: it is drawn at its
// native size, where nine-slicing and stretching are the same thing.
enum class GuiWidgetSprite : std::size_t {
    Button,
    ButtonHighlighted,
    ButtonDisabled,
    Slider,
    SliderHandle,
    SliderHandleHighlighted,
    Count,
};

using GuiWidgetSpriteTable =
    std::array<GuiAtlasSprite, static_cast<std::size_t>(GuiWidgetSprite::Count)>;

[[nodiscard]] inline const GuiAtlasSprite& guiWidgetSprite(const GuiWidgetSpriteTable& table,
                                                           GuiWidgetSprite sprite) {
    return table[static_cast<std::size_t>(sprite)];
}

} // namespace mc::render
