#pragma once

// PX-5: display names for the Key Binds screen. Vulkan-free and GLFW-free — the
// Controls page lists one row per rebindable InputAction, showing the action's
// label and the physical control currently bound to it. The renderer draws these
// strings; nothing here touches Vulkan, so the naming + the ordering of the
// rebindable rows is headless-testable.
//
// These are English identifiers; the i18n layer can map them to translation keys
// later. Movement/attack/use/inventory/hotbar/drop/chat/etc. are rebindable;
// Pause (Escape) is intentionally NOT offered for rebinding (vanilla keeps the
// menu key fixed), which is why keyBindRows() lists a curated set.

#include "input/InputAction.hpp"
#include "input/InputBinding.hpp"

#include <array>
#include <string>
#include <string_view>

namespace mc::input {

[[nodiscard]] inline std::string_view actionDisplayName(InputAction action) noexcept {
    switch (action) {
        case InputAction::MoveForward: return "Forward";
        case InputAction::MoveBack: return "Back";
        case InputAction::MoveLeft: return "Left";
        case InputAction::MoveRight: return "Right";
        case InputAction::Jump: return "Jump";
        case InputAction::Sneak: return "Sneak";
        case InputAction::Sprint: return "Sprint";
        case InputAction::Attack: return "Attack / Destroy";
        case InputAction::Use: return "Use Item / Place";
        case InputAction::Inventory: return "Inventory";
        case InputAction::Hotbar1: return "Hotbar Slot 1";
        case InputAction::Hotbar2: return "Hotbar Slot 2";
        case InputAction::Hotbar3: return "Hotbar Slot 3";
        case InputAction::Hotbar4: return "Hotbar Slot 4";
        case InputAction::Hotbar5: return "Hotbar Slot 5";
        case InputAction::Hotbar6: return "Hotbar Slot 6";
        case InputAction::Hotbar7: return "Hotbar Slot 7";
        case InputAction::Hotbar8: return "Hotbar Slot 8";
        case InputAction::Hotbar9: return "Hotbar Slot 9";
        case InputAction::DropItem: return "Drop Item";
        case InputAction::Chat: return "Open Chat";
        case InputAction::Command: return "Open Command";
        case InputAction::Perspective: return "Toggle Perspective";
        case InputAction::Debug: return "Debug Info";
        case InputAction::Pause: return "Pause / Menu";
        case InputAction::Count: break;
    }
    return "?";
}

[[nodiscard]] inline std::string_view keyName(Key key) noexcept {
    switch (key) {
        case Key::W: return "W";
        case Key::A: return "A";
        case Key::S: return "S";
        case Key::D: return "D";
        case Key::Q: return "Q";
        case Key::E: return "E";
        case Key::T: return "T";
        case Key::Space: return "Space";
        case Key::LeftShift: return "Left Shift";
        case Key::LeftControl: return "Left Ctrl";
        case Key::Escape: return "Escape";
        case Key::Enter: return "Enter";
        case Key::Tab: return "Tab";
        case Key::Backspace: return "Backspace";
        case Key::Slash: return "/";
        case Key::F3: return "F3";
        case Key::F5: return "F5";
        case Key::Digit1: return "1";
        case Key::Digit2: return "2";
        case Key::Digit3: return "3";
        case Key::Digit4: return "4";
        case Key::Digit5: return "5";
        case Key::Digit6: return "6";
        case Key::Digit7: return "7";
        case Key::Digit8: return "8";
        case Key::Digit9: return "9";
        case Key::Unknown: break;
    }
    return "Not Bound";
}

[[nodiscard]] inline std::string_view mouseName(MouseButton button) noexcept {
    switch (button) {
        case MouseButton::Left: return "Left Button";
        case MouseButton::Right: return "Right Button";
        case MouseButton::Middle: return "Middle Button";
        case MouseButton::Unknown: break;
    }
    return "Not Bound";
}

// The physical control a binding currently points at, as a label for the row.
[[nodiscard]] inline std::string bindingDisplayName(const InputBinding& binding) {
    switch (binding.device) {
        case InputDevice::Keyboard:
            return std::string{keyName(static_cast<Key>(binding.code))};
        case InputDevice::Mouse:
            return std::string{mouseName(static_cast<MouseButton>(binding.code))};
        case InputDevice::GamepadButton:
            return "Gamepad";
        case InputDevice::None:
            break;
    }
    return "Not Bound";
}

// The rebindable actions, in the order the Controls screen lists them. Pause is
// deliberately excluded (the menu/escape key is fixed, matching vanilla). This
// curated order is the single source the page builder iterates.
[[nodiscard]] inline constexpr std::array<InputAction, 24> keyBindRows() noexcept {
    return {
        InputAction::MoveForward, InputAction::MoveBack,  InputAction::MoveLeft,
        InputAction::MoveRight,   InputAction::Jump,      InputAction::Sneak,
        InputAction::Sprint,      InputAction::Attack,    InputAction::Use,
        InputAction::Inventory,   InputAction::DropItem,  InputAction::Chat,
        InputAction::Command,     InputAction::Perspective, InputAction::Debug,
        InputAction::Hotbar1,     InputAction::Hotbar2,   InputAction::Hotbar3,
        InputAction::Hotbar4,     InputAction::Hotbar5,   InputAction::Hotbar6,
        InputAction::Hotbar7,     InputAction::Hotbar8,   InputAction::Hotbar9,
    };
}

}  // namespace mc::input
