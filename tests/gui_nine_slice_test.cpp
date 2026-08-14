#include "ui/GuiNineSlice.hpp"

#include <cassert>
#include <cmath>
#include <vector>

// The geometry behind 26.1's gui.scaling. Nine-slicing is easy to get subtly
// wrong in ways that only show as a soft edge on someone's screen, so the
// properties that make it *look* right are asserted here instead: border bands
// are never scaled, the destination is partitioned with no seam or overlap, and
// the source never leaves the sprite's atlas rectangle.
namespace {

using mc::ui::GuiSpriteQuad;
using mc::ui::UiRect;

[[nodiscard]] std::vector<GuiSpriteQuad> quadsOf(const UiRect& destination, const UiRect& source,
                                                 const mc::assets::GuiSpriteScaling& scaling,
                                                 float scale) {
    std::vector<GuiSpriteQuad> quads;
    mc::ui::forEachGuiSpriteQuad(destination, source, scaling, scale,
                                 [&](const GuiSpriteQuad& quad) { quads.push_back(quad); });
    return quads;
}

[[nodiscard]] bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 1.0e-3F;
}

// The 26.1 button: 200x20 art with a 3px frame, sitting at (0,66) in the
// compatibility atlas.
[[nodiscard]] mc::assets::GuiSpriteScaling buttonScaling() {
    return mc::assets::GuiSpriteScaling::parse(R"({
        "gui": {"scaling": {"type": "nine_slice", "width": 200, "height": 20, "border": 3}}
    })");
}
const UiRect kButtonRegion{0.0F, 66.0F, 200.0F, 20.0F};

// Every quad must sample inside the sprite's own atlas rectangle. Bleeding
// outside picks up a neighbouring sprite — the failure that makes a stray row
// of hotbar pixels appear along a button edge.
void assertSourceInsideRegion(const std::vector<GuiSpriteQuad>& quads, const UiRect& region) {
    for (const auto& quad : quads) {
        assert(quad.source.x >= region.x - 1.0e-3F);
        assert(quad.source.y >= region.y - 1.0e-3F);
        assert(quad.source.x + quad.source.width <= region.x + region.width + 1.0e-3F);
        assert(quad.source.y + quad.source.height <= region.y + region.height + 1.0e-3F);
        assert(quad.source.width > 0.0F && quad.source.height > 0.0F);
        assert(quad.destination.width > 0.0F && quad.destination.height > 0.0F);
    }
}

// The destination quads must cover the target rectangle exactly: their areas
// sum to its area (no overlap given they also stay inside it), and their union
// reaches every edge. A gap shows as a transparent seam through the widget.
void assertCoversDestination(const std::vector<GuiSpriteQuad>& quads, const UiRect& destination) {
    float area = 0.0F;
    float right = destination.x;
    float bottom = destination.y;
    for (const auto& quad : quads) {
        assert(quad.destination.x >= destination.x - 1.0e-3F);
        assert(quad.destination.y >= destination.y - 1.0e-3F);
        assert(quad.destination.x + quad.destination.width <=
               destination.x + destination.width + 1.0e-3F);
        assert(quad.destination.y + quad.destination.height <=
               destination.y + destination.height + 1.0e-3F);
        area += quad.destination.width * quad.destination.height;
        right = std::max(right, quad.destination.x + quad.destination.width);
        bottom = std::max(bottom, quad.destination.y + quad.destination.height);
    }
    assert(near(area, destination.width * destination.height));
    assert(near(right, destination.x + destination.width));
    assert(near(bottom, destination.y + destination.height));
}

// The property the whole feature exists for: a band drawn from the sprite's
// border must map one source pixel to exactly `scale` destination pixels. Any
// other ratio is the blur this replaces.
void assertBordersUnscaled(const std::vector<GuiSpriteQuad>& quads, float scale, float border) {
    int borderQuads = 0;
    for (const auto& quad : quads) {
        const bool leadingColumn = near(quad.source.width, border);
        const bool leadingRow = near(quad.source.height, border);
        if (!leadingColumn && !leadingRow) {
            continue;
        }
        if (leadingColumn) {
            assert(near(quad.destination.width, quad.source.width * scale));
        }
        if (leadingRow) {
            assert(near(quad.destination.height, quad.source.height * scale));
        }
        ++borderQuads;
    }
    // Four corners plus four edges all touch a border band.
    assert(borderQuads >= 8);
}

} // namespace

int main() {
    using namespace mc::assets;

    // --- A stretch sprite is one quad: exactly what every fixed-size HUD
    // element did before nine-slicing existed, unchanged. ---
    {
        const UiRect destination{10.0F, 20.0F, 64.0F, 48.0F};
        const auto quads = quadsOf(destination, kButtonRegion, GuiSpriteScaling{}, 2.0F);
        assert(quads.size() == 1U);
        assert(near(quads[0].destination.width, 64.0F) && near(quads[0].destination.height, 48.0F));
        assert(near(quads[0].source.width, 200.0F) && near(quads[0].source.height, 20.0F));
    }

    // --- Drawn at its own reference size, a nine-slice must reproduce the
    // sprite 1:1. This is the no-regression case: the 200x20 menu button on the
    // title screen has to keep landing on exactly its own pixels. ---
    {
        const float scale = 2.0F;
        const UiRect destination{100.0F, 50.0F, 200.0F * scale, 20.0F * scale};
        const auto quads = quadsOf(destination, kButtonRegion, buttonScaling(), scale);
        assertSourceInsideRegion(quads, kButtonRegion);
        assertCoversDestination(quads, destination);
        assertBordersUnscaled(quads, scale, 3.0F);
        // 3 + 194 + 3 on each axis, and the middle fits in one repeat: a full
        // 3x3 grid and nothing more.
        assert(quads.size() == 9U);
        for (const auto& quad : quads) {
            // Every quad, corner and centre alike, samples at exactly the GUI
            // scale — the sprite is reproduced, not resampled.
            assert(near(quad.destination.width, quad.source.width * scale));
            assert(near(quad.destination.height, quad.source.height * scale));
        }
    }

    // --- A button narrower than its art (the two-column video settings page).
    // The frame keeps its 3px; the middle is *clipped*, not squashed. ---
    {
        const float scale = 1.0F;
        const UiRect destination{0.0F, 0.0F, 150.0F, 20.0F};
        const auto quads = quadsOf(destination, kButtonRegion, buttonScaling(), scale);
        assertSourceInsideRegion(quads, kButtonRegion);
        assertCoversDestination(quads, destination);
        assertBordersUnscaled(quads, scale, 3.0F);
        assert(quads.size() == 9U);
        for (const auto& quad : quads) {
            assert(near(quad.destination.width, quad.source.width * scale));
            assert(near(quad.destination.height, quad.source.height * scale));
        }
        // The right border still comes from the sprite's right edge (197..200),
        // not from wherever a stretch would have landed.
        bool sawRightEdge = false;
        for (const auto& quad : quads) {
            if (near(quad.source.x, 197.0F)) {
                sawRightEdge = true;
                assert(near(quad.destination.x, 147.0F));
                assert(near(quad.source.width, 3.0F));
            }
        }
        assert(sawRightEdge);
    }

    // --- A button wider than its art. The middle repeats rather than
    // stretching, so the last repeat is clipped and every quad still samples at
    // the GUI scale. ---
    {
        const float scale = 1.0F;
        const UiRect destination{0.0F, 0.0F, 400.0F, 20.0F};
        const auto quads = quadsOf(destination, kButtonRegion, buttonScaling(), scale);
        assertSourceInsideRegion(quads, kButtonRegion);
        assertCoversDestination(quads, destination);
        assertBordersUnscaled(quads, scale, 3.0F);
        for (const auto& quad : quads) {
            assert(near(quad.destination.width, quad.source.width * scale));
            assert(near(quad.destination.height, quad.source.height * scale));
        }
        // 394px of middle over a 194px inner band: three repeats, the last one
        // 6px wide. A stretching implementation would emit one 394px quad.
        int middleColumnQuads = 0;
        for (const auto& quad : quads) {
            if (quad.source.x >= 3.0F - 1.0e-3F && quad.source.x < 197.0F - 1.0e-3F &&
                near(quad.source.y, 66.0F)) {
                ++middleColumnQuads;
            }
        }
        assert(middleColumnQuads == 3);
    }

    // --- `stretch_inner` (26.1's tooltip/frame) scales the middle bands
    // instead: one centre quad, and the frame still unscaled. ---
    {
        const auto scaling = GuiSpriteScaling::parse(R"({
            "gui": {"scaling": {"type": "nine_slice", "width": 100, "height": 100,
                                "border": 10, "stretch_inner": true}}
        })");
        const UiRect region{0.0F, 0.0F, 100.0F, 100.0F};
        const UiRect destination{0.0F, 0.0F, 400.0F, 300.0F};
        const auto quads = quadsOf(destination, region, scaling, 1.0F);
        assertSourceInsideRegion(quads, region);
        assertCoversDestination(quads, destination);
        assert(quads.size() == 9U); // no repeats at all
        bool sawStretchedCentre = false;
        for (const auto& quad : quads) {
            if (near(quad.source.x, 10.0F) && near(quad.source.y, 10.0F)) {
                sawStretchedCentre = true;
                assert(near(quad.source.width, 80.0F) && near(quad.source.height, 80.0F));
                assert(near(quad.destination.width, 380.0F));
                assert(near(quad.destination.height, 280.0F));
            }
        }
        assert(sawStretchedCentre);
    }

    // --- The slider handle: 8x20 with an asymmetric border, drawn at exactly
    // its native size. The 3px bottom lip must come from the sprite's bottom,
    // the 2px top from its top — a uniform border would shift one of them. ---
    {
        const auto scaling = GuiSpriteScaling::parse(R"({
            "gui": {"scaling": {"type": "nine_slice", "width": 8, "height": 20,
                                "border": {"left": 2, "top": 2, "right": 2, "bottom": 3}}}
        })");
        const UiRect region{0.0F, 146.0F, 8.0F, 20.0F};
        const float scale = 3.0F;
        const UiRect destination{7.0F, 11.0F, 8.0F * scale, 20.0F * scale};
        const auto quads = quadsOf(destination, region, scaling, scale);
        assertSourceInsideRegion(quads, region);
        assertCoversDestination(quads, destination);
        bool sawBottomLip = false;
        for (const auto& quad : quads) {
            assert(near(quad.destination.width, quad.source.width * scale));
            assert(near(quad.destination.height, quad.source.height * scale));
            if (near(quad.source.y, 146.0F + 20.0F - 3.0F)) {
                sawBottomLip = true;
                assert(near(quad.source.height, 3.0F));
            }
        }
        assert(sawBottomLip);
    }

    // --- A widget drawn narrower than its own frame. Vanilla clamps each inset
    // to half the destination; without that the slices overlap and the source
    // rectangles invert. ---
    {
        const UiRect destination{0.0F, 0.0F, 4.0F, 20.0F};
        const auto quads = quadsOf(destination, kButtonRegion, buttonScaling(), 1.0F);
        assertSourceInsideRegion(quads, kButtonRegion);
        assertCoversDestination(quads, destination);
        // Two half-borders across, no middle column.
        for (const auto& quad : quads) {
            assert(near(quad.destination.width, 2.0F));
        }
    }

    // --- `tile` repeats the whole sprite with no frame. ---
    {
        const auto scaling =
            GuiSpriteScaling::parse(R"({"gui": {"scaling": {"type": "tile", "width": 16,
                                                            "height": 16}}})");
        const UiRect region{0.0F, 0.0F, 16.0F, 16.0F};
        const UiRect destination{0.0F, 0.0F, 40.0F, 16.0F};
        const auto quads = quadsOf(destination, region, scaling, 1.0F);
        assertSourceInsideRegion(quads, region);
        assertCoversDestination(quads, destination);
        assert(quads.size() == 3U); // 16 + 16 + 8 clipped
    }

    // --- A one-pixel repeating band over a large destination would need
    // thousands of quads; that axis stretches instead of flooding the command
    // buffer, and still covers the destination. ---
    {
        const auto scaling = GuiSpriteScaling::parse(R"({
            "gui": {"scaling": {"type": "nine_slice", "width": 3, "height": 3, "border": 1}}
        })");
        const UiRect region{0.0F, 0.0F, 3.0F, 3.0F};
        const UiRect destination{0.0F, 0.0F, 4000.0F, 40.0F};
        const auto quads = quadsOf(destination, region, scaling, 1.0F);
        assertSourceInsideRegion(quads, region);
        assertCoversDestination(quads, destination);
        assert(quads.size() < 256U);
    }

    // --- Degenerate inputs emit nothing rather than inverted quads. ---
    {
        assert(quadsOf({0.0F, 0.0F, 0.0F, 20.0F}, kButtonRegion, buttonScaling(), 1.0F).empty());
        assert(quadsOf({0.0F, 0.0F, 20.0F, -4.0F}, kButtonRegion, buttonScaling(), 1.0F).empty());
        assert(quadsOf({0.0F, 0.0F, 20.0F, 20.0F}, {0.0F, 0.0F, 0.0F, 0.0F}, buttonScaling(), 1.0F)
                   .empty());
        // A zero GUI scale would divide the destination by nothing.
        assert(quadsOf({0.0F, 0.0F, 20.0F, 20.0F}, kButtonRegion, buttonScaling(), 0.0F).empty());
    }

    return 0;
}
