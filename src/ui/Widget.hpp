#pragma once

// PX-4: the data-driven menu object model. A menu page is a flat (shape-wise
// nestable) list of Widget VALUES — not a retained OO tree, not a vtable
// hierarchy. Each widget carries its geometry, its label, whether it is enabled,
// and a std::function callback fired on activation. This replaces the old triple
// coupling (a MenuButton enum + a per-page constexpr array + a ~300-line
// switch(MenuButton) dispatch): a page is now built in one place (PageBuilder),
// hit-tested and dispatched generically (MenuInteraction), and drawn generically
// by kind (the renderer's draw backend).
//
// Vulkan-free and GLFW-free: this lives in mc_rebedrock_runtime so the model,
// the hit test and the dispatch are exercised by headless unit tests (build a
// page, click a coordinate, assert the callback fired). The callbacks capture
// whatever Vulkan/save/audio state the renderer needs; ui:: never sees it.
//
// The Widget is designed to be *nestable* (Panel can hold children) and to carry
// simple relative-layout hints, so a future container UI (creative tabs + scroll
// grid + search field) extends the same model instead of a rewrite. PX-4 only
// uses flat pages — the nesting is shape kept, not framework built.

#include "ui/HudLayout.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mc::ui {

enum class WidgetKind : std::uint8_t {
    Button,     // a clickable button (GuiNineSlice + label)
    Slider,     // a horizontal slider with a draggable handle
    ListRow,    // one selectable row in a scrolling list (worlds/languages)
    Label,      // static text, never interactive
    Panel,      // a non-interactive container (holds children; shape only in PX-4)
    Toggle,     // a button whose label reflects an on/off (cycled) option
    TextField,  // an editable text line (create/edit world name)
};

// A slider's data + callbacks. `value()` reports the current normalized-or-raw
// display value the draw backend paints the handle from; `onDrag(fraction)`
// applies a new position (fraction in [0,1] across the track). Keeping the effect
// in the callback (not in the traversal) is the rule: the traversal never carries
// a `if (kind == Slider)` side effect — it just calls onDrag.
struct SliderBind final {
    std::function<float()> value;             // current fraction in [0,1], for drawing
    std::function<void(float)> onDrag;        // apply a new fraction in [0,1]
    std::function<void()> onCommit;           // release: persist / play feedback
};

// A single menu element as a value. Copyable/movable; a page owns a vector of
// these. `onActivate` is the click action (Button/Toggle/ListRow); Sliders use
// `slider` instead. `debugId` is an optional stable identifier (the old
// MenuButton value) kept only for tests/logging — never switched on for
// behaviour.
struct Widget final {
    WidgetKind kind = WidgetKind::Button;
    UiRect rect{};
    std::string label{};
    bool enabled = true;
    std::uint16_t debugId = 0;  // optional test/debug tag; 0 == none

    std::function<void()> onActivate{};  // Button/Toggle/ListRow click
    SliderBind slider{};                 // Slider only

    // Nesting shape for a future container UI. Empty for every PX-4 flat page.
    std::vector<Widget> children{};

    [[nodiscard]] bool interactive() const noexcept {
        return kind != WidgetKind::Label && kind != WidgetKind::Panel;
    }
};

// A page is just a list of widget values, rebuilt each time the page opens. No
// dirty tracking — the menu is a cold path and a rebuild is trivially cheap.
using Page = std::vector<Widget>;

}  // namespace mc::ui
