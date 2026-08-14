#pragma once

// Turns one "draw this sprite into that rectangle" into the quads that honour
// the sprite's 26.1 `gui.scaling`. Kept free of Vulkan (and of the renderer
// generally) so the geometry — which is where nine-slicing is easy to get
// subtly wrong — can be asserted headlessly instead of only by eye.
//
// Everything below works in *sprite pixels*: the units the mcmeta's borders and
// reference size are written in. Two conversions happen once, on the way out —
// `scale` (the GUI scale) to framebuffer pixels for the destination, and the
// art's own resolution to atlas pixels for the source. A pack that ships the
// art at 2x therefore keeps a 3-pixel border reading as 3 sprite pixels.

#include "assets/GuiSpriteScaling.hpp"
#include "ui/HudLayout.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace mc::ui {

// One sub-rectangle draw: `destination` in framebuffer pixels, `source` in
// atlas pixels.
struct GuiSpriteQuad final {
    UiRect destination;
    UiRect source;
};

// A sprite whose repeating region is a pixel or two across would need thousands
// of quads to cover a large window. Past this many repeats on an axis, that
// axis stretches instead of flooding the command buffer.
inline constexpr int kMaximumGuiTilesPerAxis = 64;

namespace detail {

// One of the three bands along an axis: leading border, middle, trailing
// border. Offsets and lengths are sprite pixels relative to the rectangle's
// leading edge.
struct GuiSpan final {
    float destinationOffset = 0.0F;
    float destinationLength = 0.0F;
    float sourceOffset = 0.0F;
    float sourceLength = 0.0F;
    // The band that grows with the destination, rather than a fixed-size
    // border. Only these tile or stretch.
    bool inner = false;
};

// Splits one axis. Both borders clamp to half the destination, matching
// vanilla: a widget drawn narrower than its own frame shows two half-borders
// and no middle, rather than slices that overlap or invert. The source uses the
// clamped insets too, so a border band is always sampled 1:1 in sprite space
// and stays sharp.
[[nodiscard]] inline std::array<GuiSpan, 3> guiSliceAxis(float destinationLength,
                                                         float referenceLength, int leadingBorder,
                                                         int trailingBorder) {
    const float half = destinationLength * 0.5F;
    const float leading = std::min(static_cast<float>(std::max(leadingBorder, 0)), half);
    const float trailing = std::min(static_cast<float>(std::max(trailingBorder, 0)), half);
    const float middle = std::max(destinationLength - leading - trailing, 0.0F);
    const float sourceMiddle = std::max(referenceLength - leading - trailing, 0.0F);
    return {
        GuiSpan{0.0F, leading, 0.0F, leading, false},
        GuiSpan{leading, middle, leading, sourceMiddle, true},
        GuiSpan{leading + middle, trailing, referenceLength - trailing, trailing, false},
    };
}

// How many times a band repeats, and how long one repeat is along the
// destination. A non-repeating band is one step covering the whole band, which
// makes it a plain stretch.
struct GuiRepeat final {
    int count = 1;
    float step = 0.0F;
};

[[nodiscard]] inline GuiRepeat guiRepeat(bool repeat, float destinationLength,
                                         float sourceLength) {
    if (!repeat || sourceLength <= 0.0F) {
        return GuiRepeat{1, destinationLength};
    }
    // The epsilon keeps an exact fit (a 194px middle over a 194px band) at one
    // repeat instead of adding a zero-width second one.
    const int count =
        std::max(static_cast<int>(std::ceil(destinationLength / sourceLength - 1.0e-4F)), 1);
    if (count > kMaximumGuiTilesPerAxis) {
        return GuiRepeat{1, destinationLength};
    }
    return GuiRepeat{count, sourceLength};
}

} // namespace detail

// Emits the quads that draw `source` (atlas pixels) into `destination`
// (framebuffer pixels) under `scaling`, at GUI scale `scale`. `emit` is called
// with one GuiSpriteQuad per quad, in row-major order; the destination quads
// partition `destination` exactly, leaving no seams.
template <typename Emit>
void forEachGuiSpriteQuad(const UiRect& destination, const UiRect& source,
                          const assets::GuiSpriteScaling& scaling, float scale, Emit&& emit) {
    if (destination.width <= 0.0F || destination.height <= 0.0F || source.width <= 0.0F ||
        source.height <= 0.0F || scale <= 0.0F) {
        return;
    }
    if (scaling.type == assets::GuiSpriteScalingType::Stretch) {
        emit(GuiSpriteQuad{destination, source});
        return;
    }
    // A sidecar may omit the reference size; the art's own size is then what
    // the borders are measured against.
    const float referenceWidth =
        scaling.width > 0 ? static_cast<float>(scaling.width) : source.width;
    const float referenceHeight =
        scaling.height > 0 ? static_cast<float>(scaling.height) : source.height;
    const float atlasPerSpriteX = source.width / referenceWidth;
    const float atlasPerSpriteY = source.height / referenceHeight;

    // `tile` is the borderless case of the same split: no frame, one middle
    // band that repeats the whole sprite.
    const bool nineSlice = scaling.type == assets::GuiSpriteScalingType::NineSlice;
    const auto columns = detail::guiSliceAxis(destination.width / scale, referenceWidth,
                                              nineSlice ? scaling.border.left : 0,
                                              nineSlice ? scaling.border.right : 0);
    const auto rows = detail::guiSliceAxis(destination.height / scale, referenceHeight,
                                           nineSlice ? scaling.border.top : 0,
                                           nineSlice ? scaling.border.bottom : 0);
    // 26.1 tiles a nine-slice's middle bands unless the sprite asks for them to
    // be stretched; a `tile` sprite always repeats.
    const bool repeatInner = !nineSlice || !scaling.stretchInner;

    for (const auto& row : rows) {
        if (row.destinationLength <= 0.0F || row.sourceLength <= 0.0F) {
            continue;
        }
        const auto vertical =
            detail::guiRepeat(repeatInner && row.inner, row.destinationLength, row.sourceLength);
        for (const auto& column : columns) {
            if (column.destinationLength <= 0.0F || column.sourceLength <= 0.0F) {
                continue;
            }
            const auto horizontal = detail::guiRepeat(repeatInner && column.inner,
                                                      column.destinationLength, column.sourceLength);
            for (int tileY = 0; tileY < vertical.count; ++tileY) {
                const float y = row.destinationOffset + static_cast<float>(tileY) * vertical.step;
                const float height =
                    std::min(vertical.step, row.destinationOffset + row.destinationLength - y);
                if (height <= 0.0F) {
                    break;
                }
                for (int tileX = 0; tileX < horizontal.count; ++tileX) {
                    const float x =
                        column.destinationOffset + static_cast<float>(tileX) * horizontal.step;
                    const float width = std::min(
                        horizontal.step, column.destinationOffset + column.destinationLength - x);
                    if (width <= 0.0F) {
                        break;
                    }
                    // A repeat running past the end is clipped: it samples only
                    // the leading part of the source instead of squashing all
                    // of it into the remaining space.
                    emit(GuiSpriteQuad{
                        UiRect{destination.x + x * scale, destination.y + y * scale, width * scale,
                               height * scale},
                        UiRect{source.x + column.sourceOffset * atlasPerSpriteX,
                               source.y + row.sourceOffset * atlasPerSpriteY,
                               column.sourceLength * (width / horizontal.step) * atlasPerSpriteX,
                               row.sourceLength * (height / vertical.step) * atlasPerSpriteY},
                    });
                }
            }
        }
    }
}

} // namespace mc::ui
