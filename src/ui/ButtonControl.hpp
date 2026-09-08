#pragma once

#include "ui/HudLayout.hpp"

namespace mc::ui {

enum class ButtonVisualState {
    Disabled,
    Normal,
    Hovered,
    Pressed,
};

// UI-4 / GUI spec §1.4：`focused` 与悬停走同一个视觉。
//
// 26.1 的按钮**没有**单独的焦点描边——`AbstractButton.extractRenderState` 取的是
// `SPRITES.get(active, isHoveredOrFocused())`，也就是键盘焦点与鼠标悬停共用
// highlighted 那张精灵（`AbstractButton.java:46`）。spec §1.4 那句「焦点控件绘制白色
// 描边高亮」说的其实是**列表的选中框**在列表获得焦点时由灰变白
// （`AbstractSelectionList.extractItem:350`），不是按钮的事。
[[nodiscard]] inline ButtonVisualState buttonVisualState(
    const UiRect& bounds,
    float pointerX,
    float pointerY,
    bool enabled,
    bool pressed,
    bool focused = false) {
    if (!enabled) {
        return ButtonVisualState::Disabled;
    }
    const bool hovered = bounds.contains(pointerX, pointerY);
    if (pressed && hovered) {
        return ButtonVisualState::Pressed;
    }
    return (hovered || focused) ? ButtonVisualState::Hovered : ButtonVisualState::Normal;
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
