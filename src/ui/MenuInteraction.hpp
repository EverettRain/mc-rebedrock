#pragma once

// PX-4: generic page interaction — hit test + click/drag dispatch. This is the
// single replacement for VulkanRenderer::handleMenuButtonPress/Release's
// switch(MenuButton): a click resolves to a widget by geometry and fires that
// widget's own callback. No per-button branching, no page knowledge here.
//
// Vulkan-free: operates purely on the ui::Page value model and pointer
// coordinates, so a headless test can build a page, "click" a coordinate, and
// assert the right callback ran (and that a disabled widget runs nothing).
// Reuses ui::buttonActivated / buttonVisualState for the exact hover/press/enable
// semantics the old per-button path used.

#include "ui/ButtonControl.hpp"
#include "ui/Widget.hpp"

#include <cstddef>

namespace mc::ui {

// The index of the topmost interactive widget whose rect contains the pointer,
// or npos if none. Later widgets win ties (drawn last == on top), matching the
// draw order. Non-interactive widgets (Label/Panel) are skipped.
inline constexpr std::size_t kNoWidget = static_cast<std::size_t>(-1);

[[nodiscard]] inline std::size_t hitTest(const Page& page, float pointerX,
                                         float pointerY) noexcept {
    std::size_t hit = kNoWidget;
    for (std::size_t i = 0; i < page.size(); ++i) {
        const Widget& widget = page[i];
        if (!widget.interactive()) {
            continue;
        }
        if (widget.rect.contains(pointerX, pointerY)) {
            hit = i;  // keep scanning so the last (topmost) match wins
        }
    }
    return hit;
}

// Fire the activation for a press/release pair over the same widget. `pressed` is
// the widget the press landed on (from a prior pressAt); the activation only runs
// if the release is over that same widget and it is enabled — exactly
// ButtonControl::buttonActivated's contract, now applied to the whole page.
// Returns the activated widget index, or kNoWidget if nothing fired.
//
// A Slider is a drag control, not a click: it never activates here (its effect
// ran through the drag callbacks). ListRow/Button/Toggle fire onActivate.
[[nodiscard]] inline std::size_t dispatchActivate(const Page& page, std::size_t pressedIndex,
                                                  float releaseX, float releaseY) {
    if (pressedIndex >= page.size()) {
        return kNoWidget;
    }
    const Widget& widget = page[pressedIndex];
    if (widget.kind == WidgetKind::Slider) {
        return kNoWidget;  // drags act through onDrag/onCommit, not activation
    }
    if (!buttonActivated(widget.rect, releaseX, releaseY, /*wasPressed=*/true, widget.enabled)) {
        return kNoWidget;
    }
    if (widget.onActivate) {
        widget.onActivate();
    }
    return pressedIndex;
}

// Convenience for the headless test path (and any caller that presses+releases in
// one spot): hit-test the coordinate and, if it landed on an enabled interactive
// widget, activate it. Returns the activated index or kNoWidget.
[[nodiscard]] inline std::size_t clickAt(const Page& page, float pointerX, float pointerY) {
    const std::size_t index = hitTest(page, pointerX, pointerY);
    if (index == kNoWidget || !page[index].enabled) {
        return kNoWidget;
    }
    return dispatchActivate(page, index, pointerX, pointerY);
}

// Begin a drag on the widget under the pointer if it is an enabled Slider. The
// caller supplies the track fraction it computed from the layout; the slider's
// onDrag applies it. Returns the slider index if a drag started, else kNoWidget.
[[nodiscard]] inline std::size_t beginSliderDrag(const Page& page, float pointerX, float pointerY,
                                                 float trackFraction) {
    const std::size_t index = hitTest(page, pointerX, pointerY);
    if (index == kNoWidget) {
        return kNoWidget;
    }
    const Widget& widget = page[index];
    if (widget.kind != WidgetKind::Slider || !widget.enabled || !widget.slider.onDrag) {
        return kNoWidget;
    }
    widget.slider.onDrag(trackFraction);
    return index;
}

}  // namespace mc::ui
