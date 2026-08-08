#pragma once

#include "ui/HudLayout.hpp"

namespace mc::ui {

enum class ButtonVisualState {
    Disabled,
    Normal,
    Hovered,
    Pressed,
};

[[nodiscard]] inline ButtonVisualState buttonVisualState(
    const UiRect& bounds,
    float pointerX,
    float pointerY,
    bool enabled,
    bool pressed) {
    if (!enabled) {
        return ButtonVisualState::Disabled;
    }
    const bool hovered = bounds.contains(pointerX, pointerY);
    if (pressed && hovered) {
        return ButtonVisualState::Pressed;
    }
    return hovered ? ButtonVisualState::Hovered : ButtonVisualState::Normal;
}

[[nodiscard]] inline bool buttonActivated(
    const UiRect& bounds,
    float pointerX,
    float pointerY,
    bool wasPressed,
    bool enabled = true) {
    return enabled && wasPressed && bounds.contains(pointerX, pointerY);
}

} // namespace mc::ui
