#pragma once

// PX-5: display names for the Key Binds screen. Vulkan-free and GLFW-free — the
// Controls page lists one row per rebindable InputAction, showing the action's
// label and the physical control currently bound to it. The renderer draws these
// strings; nothing here touches Vulkan, so the naming + the ordering of the
// rebindable rows is headless-testable.
//
// The `*DisplayName` functions are English identifiers and stay that way: they are
// the FALLBACK when a translation is missing, and they are what the headless tests
// assert against. The `*TranslationKey` functions beside them are the i18n layer
// the header note above used to promise — 26.1's own keys (`key.forward`,
// `key.keyboard.space`, `key.mouse.left`), resolved by the UI through the loaded
// language table.
//
// ★ 26.1 does NOT translate the plain printable keys. `InputConstants.Type.KEYSYM`
// asks GLFW for the system key name first and only falls back to
// `Component.translatable(name)` when GLFW has none:
//     systemName != null ? literal(systemName.toUpperCase()) : translatable(name)
// which is why vanilla's lang files carry `key.keyboard.space` but no
// `key.keyboard.w`. This header has no GLFW, so it hands out the translation key
// for every control and lets the missing entry fall back to the English name —
// the same "W" on screen, without dragging a window library into a headless
// header. The one difference: a resource pack that DID ship `key.keyboard.w`
// would be honoured here and ignored by vanilla. Registered, not fixed.
//
// Movement/attack/use/inventory/hotbar/drop/chat/etc. are rebindable;
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

// 26.1 的 KeyMapping 名字，逐条对应 `Options.java` 里那批 `new KeyMapping("key.…")`。
// 本作有而 vanilla 没有的可绑定动作（Debug）走本项目自有的 `key.rebedrock.*`
// 命名空间（`lang/rebedrock/`），因为它不在任何 vanilla 语言表里。
[[nodiscard]] inline std::string_view actionTranslationKey(InputAction action) noexcept {
    switch (action) {
        case InputAction::MoveForward: return "key.forward";
        case InputAction::MoveBack: return "key.back";
        case InputAction::MoveLeft: return "key.left";
        case InputAction::MoveRight: return "key.right";
        case InputAction::Jump: return "key.jump";
        case InputAction::Sneak: return "key.sneak";
        case InputAction::Sprint: return "key.sprint";
        case InputAction::Attack: return "key.attack";
        case InputAction::Use: return "key.use";
        case InputAction::Inventory: return "key.inventory";
        case InputAction::Hotbar1: return "key.hotbar.1";
        case InputAction::Hotbar2: return "key.hotbar.2";
        case InputAction::Hotbar3: return "key.hotbar.3";
        case InputAction::Hotbar4: return "key.hotbar.4";
        case InputAction::Hotbar5: return "key.hotbar.5";
        case InputAction::Hotbar6: return "key.hotbar.6";
        case InputAction::Hotbar7: return "key.hotbar.7";
        case InputAction::Hotbar8: return "key.hotbar.8";
        case InputAction::Hotbar9: return "key.hotbar.9";
        case InputAction::DropItem: return "key.drop";
        case InputAction::Chat: return "key.chat";
        case InputAction::Command: return "key.command";
        case InputAction::Perspective: return "key.togglePerspective";
        // vanilla 的 F3 是固定键、不可重绑，所以没有对应的 KeyMapping 名字
        case InputAction::Debug: return "key.rebedrock.debug";
        case InputAction::Pause: return "key.rebedrock.pause";
        case InputAction::Count: break;
    }
    return "";
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

// `InputConstants.Key` 的名字：`key.keyboard.<名>`。左右修饰键中间还有一段
// （`key.keyboard.left.shift`），数字键是裸数字（`key.keyboard.1`）。
[[nodiscard]] inline std::string_view keyTranslationKey(Key key) noexcept {
    switch (key) {
        case Key::W: return "key.keyboard.w";
        case Key::A: return "key.keyboard.a";
        case Key::S: return "key.keyboard.s";
        case Key::D: return "key.keyboard.d";
        case Key::Q: return "key.keyboard.q";
        case Key::E: return "key.keyboard.e";
        case Key::T: return "key.keyboard.t";
        case Key::Space: return "key.keyboard.space";
        case Key::LeftShift: return "key.keyboard.left.shift";
        case Key::LeftControl: return "key.keyboard.left.control";
        case Key::Escape: return "key.keyboard.escape";
        case Key::Enter: return "key.keyboard.enter";
        case Key::Tab: return "key.keyboard.tab";
        case Key::Backspace: return "key.keyboard.backspace";
        case Key::Slash: return "key.keyboard.slash";
        case Key::F3: return "key.keyboard.f3";
        case Key::F5: return "key.keyboard.f5";
        case Key::Digit1: return "key.keyboard.1";
        case Key::Digit2: return "key.keyboard.2";
        case Key::Digit3: return "key.keyboard.3";
        case Key::Digit4: return "key.keyboard.4";
        case Key::Digit5: return "key.keyboard.5";
        case Key::Digit6: return "key.keyboard.6";
        case Key::Digit7: return "key.keyboard.7";
        case Key::Digit8: return "key.keyboard.8";
        case Key::Digit9: return "key.keyboard.9";
        case Key::Unknown: break;
    }
    return "key.keyboard.unknown";
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

[[nodiscard]] inline std::string_view mouseTranslationKey(MouseButton button) noexcept {
    switch (button) {
        case MouseButton::Left: return "key.mouse.left";
        case MouseButton::Right: return "key.mouse.right";
        case MouseButton::Middle: return "key.mouse.middle";
        case MouseButton::Unknown: break;
    }
    return "key.keyboard.unknown";
}

// 一个绑定的翻译键。未绑定时与 vanilla 一样落在 `key.keyboard.unknown`（"未指定"）。
[[nodiscard]] inline std::string_view bindingTranslationKey(const InputBinding& binding) noexcept {
    switch (binding.device) {
        case InputDevice::Keyboard:
            return keyTranslationKey(static_cast<Key>(binding.code));
        case InputDevice::Mouse:
            return mouseTranslationKey(static_cast<MouseButton>(binding.code));
        case InputDevice::GamepadButton:
        case InputDevice::None:
            break;
    }
    return "key.keyboard.unknown";
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
