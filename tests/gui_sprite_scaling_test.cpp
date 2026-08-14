#include "assets/GuiSpriteScaling.hpp"

#include <cassert>

// 26.1 declares how each GUI sprite fills a destination in its `.png.mcmeta`
// `gui.scaling` block. These pin the two border spellings vanilla actually
// ships (scalar for the button, per-side for the slider handle), the reference
// size the borders are measured against, and that everything a pack can get
// wrong — no mcmeta, no gui block, an unknown type, malformed JSON — falls back
// to the plain stretch the renderer used before nine-slice existed.
int main() {
    using namespace mc::assets;

    // --- widget/button.png.mcmeta: scalar border applies to all four sides. ---
    {
        const auto scaling = GuiSpriteScaling::parse(R"({
            "gui": {"scaling": {"type": "nine_slice", "width": 200, "height": 20, "border": 3}}
        })");
        assert(scaling.type == GuiSpriteScalingType::NineSlice);
        assert(scaling.width == 200 && scaling.height == 20);
        assert(scaling.border.left == 3 && scaling.border.top == 3);
        assert(scaling.border.right == 3 && scaling.border.bottom == 3);
        assert(!scaling.stretchInner); // tiled inner is the default
    }

    // --- widget/slider_handle.png.mcmeta: per-side border object. A uniform
    // read would put the handle's 3px bottom lip at 2px and shift its inner
    // region up one row. ---
    {
        const auto scaling = GuiSpriteScaling::parse(R"({
            "gui": {"scaling": {"type": "nine_slice", "width": 8, "height": 20,
                                "border": {"left": 2, "top": 2, "right": 2, "bottom": 3}}}
        })");
        assert(scaling.type == GuiSpriteScalingType::NineSlice);
        assert(scaling.width == 8 && scaling.height == 20);
        assert(scaling.border.left == 2 && scaling.border.top == 2);
        assert(scaling.border.right == 2 && scaling.border.bottom == 3);
    }

    // --- toast/system.png.mcmeta gives all four sides different values: the
    // 17px left gutter holds the toast icon and the 30px top holds its title
    // bar. Nothing here is symmetric, so a side read from the wrong key shows
    // up immediately. ---
    {
        const auto scaling = GuiSpriteScaling::parse(R"({
            "gui": {"scaling": {"type": "nine_slice", "width": 160, "height": 64,
                                "border": {"left": 17, "top": 30, "right": 4, "bottom": 5}}}
        })");
        assert(scaling.border.left == 17);
        assert(scaling.border.top == 30);
        assert(scaling.border.right == 4);
        assert(scaling.border.bottom == 5);
    }

    // --- tooltip/frame.png.mcmeta is the one vanilla sprite that stretches its
    // inner regions instead of tiling them. ---
    {
        const auto scaling = GuiSpriteScaling::parse(R"({
            "gui": {"scaling": {"type": "nine_slice", "width": 100, "height": 100,
                                "border": 10, "stretch_inner": true}}
        })");
        assert(scaling.type == GuiSpriteScalingType::NineSlice);
        assert(scaling.stretchInner);
        assert(scaling.border.left == 10 && scaling.border.bottom == 10);
    }

    // --- `tile` keeps its reference size (the tile), and carries no border. ---
    {
        const auto scaling =
            GuiSpriteScaling::parse(R"({"gui": {"scaling": {"type": "tile", "width": 16,
                                                            "height": 16}}})");
        assert(scaling.type == GuiSpriteScalingType::Tile);
        assert(scaling.width == 16 && scaling.height == 16);
        assert(scaling.border.left == 0 && scaling.border.right == 0);
    }

    // --- An explicit `stretch` is the default, and carries no reference size:
    // it scales whatever the sprite's real pixels are. ---
    {
        const auto scaling = GuiSpriteScaling::parse(R"({"gui": {"scaling": {"type": "stretch"}}})");
        assert(scaling.type == GuiSpriteScalingType::Stretch);
        assert(scaling.width == 0 && scaling.height == 0);
    }

    // --- Everything a pack can get wrong reads as stretch, never as a
    // nine-slice with a zero border (which would drop the sprite's frame). ---
    {
        // A .mcmeta that only carries an animation block (hud/air is one).
        assert(GuiSpriteScaling::parse(R"({"animation": {"frametime": 2}})").type ==
               GuiSpriteScalingType::Stretch);
        // An unrecognised type from a newer pack format.
        assert(GuiSpriteScaling::parse(R"({"gui": {"scaling": {"type": "twelve_slice"}}})").type ==
               GuiSpriteScalingType::Stretch);
        // A `gui` block with no `scaling` object at all.
        assert(GuiSpriteScaling::parse(R"({"gui": {}})").type == GuiSpriteScalingType::Stretch);
        // Malformed JSON must not throw out of the texture upload path.
        assert(GuiSpriteScaling::parse("{ not json").type == GuiSpriteScalingType::Stretch);
        // The empty sidecar an editor can leave behind.
        assert(GuiSpriteScaling::parse("").type == GuiSpriteScalingType::Stretch);
    }

    // --- A negative border would produce inverted slice rectangles; it is
    // clamped to 0 rather than trusted. ---
    {
        const auto scaling = GuiSpriteScaling::parse(R"({
            "gui": {"scaling": {"type": "nine_slice", "width": 20, "height": 20, "border": -4}}
        })");
        assert(scaling.type == GuiSpriteScalingType::NineSlice);
        assert(scaling.border.left == 0 && scaling.border.right == 0);
    }

    // --- A missing width/height stays 0 so the caller substitutes the sprite's
    // real pixel size instead of scaling against a bogus reference. ---
    {
        const auto scaling =
            GuiSpriteScaling::parse(R"({"gui": {"scaling": {"type": "nine_slice",
                                                            "border": 2}}})");
        assert(scaling.width == 0 && scaling.height == 0);
        assert(scaling.border.top == 2);
    }

    return 0;
}
