#pragma once

#include "input/InputAction.hpp"
#include "input/InputBinding.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace mc::input {

// Stable string names for actions and codes so a rebind config is human-editable
// and round-trips losslessly. Names are the persistence contract — never write
// the raw enum integers to disk (they renumber when the enum grows).

[[nodiscard]] constexpr std::string_view actionName(InputAction action) noexcept {
    switch (action) {
        case InputAction::MoveForward: return "move_forward";
        case InputAction::MoveBack: return "move_back";
        case InputAction::MoveLeft: return "move_left";
        case InputAction::MoveRight: return "move_right";
        case InputAction::Jump: return "jump";
        case InputAction::Sneak: return "sneak";
        case InputAction::Sprint: return "sprint";
        case InputAction::Attack: return "attack";
        case InputAction::Use: return "use";
        case InputAction::Inventory: return "inventory";
        case InputAction::Hotbar1: return "hotbar_1";
        case InputAction::Hotbar2: return "hotbar_2";
        case InputAction::Hotbar3: return "hotbar_3";
        case InputAction::Hotbar4: return "hotbar_4";
        case InputAction::Hotbar5: return "hotbar_5";
        case InputAction::Hotbar6: return "hotbar_6";
        case InputAction::Hotbar7: return "hotbar_7";
        case InputAction::Hotbar8: return "hotbar_8";
        case InputAction::Hotbar9: return "hotbar_9";
        case InputAction::DropItem: return "drop_item";
        case InputAction::Chat: return "chat";
        case InputAction::Command: return "command";
        case InputAction::Perspective: return "perspective";
        case InputAction::Debug: return "debug";
        case InputAction::Pause: return "pause";
        case InputAction::Count: break;
    }
    return "";
}

[[nodiscard]] inline std::optional<InputAction> actionFromName(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kInputActionCount; ++i) {
        const auto action = static_cast<InputAction>(i);
        if (actionName(action) == name) {
            return action;
        }
    }
    return std::nullopt;
}

// ★ 这三个是**存档 token**，不是显示名：`"LeftShift"` / `"Slash"`，写进配置文件、
// 再由 `keyFromName` 逐字解析回来。它们从前分别叫 `keyName` / `mouseName` /
// `gamepadName`——与 `ui` 侧那份显示名（`input/InputNaming.hpp` 的
// `"Left Shift"` / `"/"`）**同命名空间、同名、同签名、函数体不同**。
//
// 两个都是 inline（`constexpr` 蕴含 inline），所以那是一次 ODR 违规：两个头文件
// 进同一个翻译单元会编译不过（所以它们从没被一起 include），但两边的 TU 在**同一个
// 二进制**里（`InputSystem.cpp` 用这里，`VulkanRenderer.cpp` 用那边），链接器挑哪一份
// 是随意的。挑错了的后果不是"显示难看"而是**丢配置**：`bindingToToken` 会把
// `"Left Shift"` 写进文件，`keyFromName` 再也解析不回来，重开游戏这条绑定静默回默认。
//
// 改名而不是合并：两边本来就该是两张表——一张给人看（要翻译、有空格），一张给文件读
// （必须逐字稳定、绝不能随语言变）。合成一张就是让存档格式跟着界面文案走。
[[nodiscard]] constexpr std::string_view keyToken(Key key) noexcept {
    switch (key) {
        case Key::W: return "W"; case Key::A: return "A"; case Key::S: return "S";
        case Key::D: return "D"; case Key::Q: return "Q"; case Key::E: return "E";
        case Key::T: return "T"; case Key::Space: return "Space";
        case Key::LeftShift: return "LeftShift"; case Key::LeftControl: return "LeftControl";
        case Key::Escape: return "Escape"; case Key::Enter: return "Enter";
        case Key::Tab: return "Tab"; case Key::Backspace: return "Backspace";
        case Key::Slash: return "Slash"; case Key::F3: return "F3"; case Key::F5: return "F5";
        case Key::Digit1: return "1"; case Key::Digit2: return "2"; case Key::Digit3: return "3";
        case Key::Digit4: return "4"; case Key::Digit5: return "5"; case Key::Digit6: return "6";
        case Key::Digit7: return "7"; case Key::Digit8: return "8"; case Key::Digit9: return "9";
        case Key::Unknown: break;
    }
    return "Unknown";
}

[[nodiscard]] inline std::optional<Key> keyFromName(std::string_view name) noexcept {
    for (std::uint16_t code = 1; code <= static_cast<std::uint16_t>(Key::Digit9); ++code) {
        const auto key = static_cast<Key>(code);
        if (keyToken(key) == name) {
            return key;
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view mouseToken(MouseButton button) noexcept {
    switch (button) {
        case MouseButton::Left: return "MouseLeft";
        case MouseButton::Right: return "MouseRight";
        case MouseButton::Middle: return "MouseMiddle";
        case MouseButton::Unknown: break;
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view gamepadToken(GamepadButton button) noexcept {
    switch (button) {
        case GamepadButton::A: return "PadA"; case GamepadButton::B: return "PadB";
        case GamepadButton::X: return "PadX"; case GamepadButton::Y: return "PadY";
        case GamepadButton::LeftBumper: return "PadLB"; case GamepadButton::RightBumper: return "PadRB";
        case GamepadButton::Back: return "PadBack"; case GamepadButton::Start: return "PadStart";
        case GamepadButton::LeftThumb: return "PadL3"; case GamepadButton::RightThumb: return "PadR3";
        case GamepadButton::Unknown: break;
    }
    return "Unknown";
}

// Serialize one binding to its "Device:Code" token. Round-trips via bindingFromToken.
[[nodiscard]] inline std::string bindingToToken(const InputBinding& binding) {
    switch (binding.device) {
        case InputDevice::Keyboard:
            return std::string("key.") + std::string(keyToken(static_cast<Key>(binding.code)));
        case InputDevice::Mouse:
            return std::string("mouse.") +
                   std::string(mouseToken(static_cast<MouseButton>(binding.code)));
        case InputDevice::GamepadButton:
            return std::string("pad.") +
                   std::string(gamepadToken(static_cast<GamepadButton>(binding.code)));
        case InputDevice::None:
            break;
    }
    return "none";
}

[[nodiscard]] inline std::optional<InputBinding> bindingFromToken(std::string_view token) {
    const auto dot = token.find('.');
    if (dot == std::string_view::npos) {
        if (token == "none") {
            return InputBinding{};
        }
        return std::nullopt;
    }
    const std::string_view prefix = token.substr(0, dot);
    const std::string_view name = token.substr(dot + 1);
    if (prefix == "key") {
        if (const auto key = keyFromName(name)) {
            return keyboard(*key);
        }
    } else if (prefix == "mouse") {
        for (std::uint16_t c = 1; c <= static_cast<std::uint16_t>(MouseButton::Middle); ++c) {
            if (mouseToken(static_cast<MouseButton>(c)) == name) {
                return mouse(static_cast<MouseButton>(c));
            }
        }
    } else if (prefix == "pad") {
        for (std::uint16_t c = 1; c <= static_cast<std::uint16_t>(GamepadButton::RightThumb); ++c) {
            if (gamepadToken(static_cast<GamepadButton>(c)) == name) {
                return gamepad(static_cast<GamepadButton>(c));
            }
        }
    }
    return std::nullopt;
}

// Serialize the whole table as "action=token" lines. Only actions differing from
// nothing are written (all of them, in enum order) so the file is complete and
// diffable. Loading applies over BindingTable::defaults(), so a partial or
// out-of-date config keeps the built-in binding for any missing/unknown line.
[[nodiscard]] inline std::string serializeBindings(const BindingTable& table) {
    std::string out;
    for (std::size_t i = 0; i < kInputActionCount; ++i) {
        const auto action = static_cast<InputAction>(i);
        out += std::string(actionName(action));
        out += '=';
        out += bindingToToken(table.binding(action));
        out += '\n';
    }
    return out;
}

[[nodiscard]] inline BindingTable parseBindings(std::string_view text) {
    BindingTable table = BindingTable::defaults();
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const auto action = actionFromName(line.substr(0, eq));
        const auto binding = bindingFromToken(line.substr(eq + 1));
        if (action && binding) {
            table.bind(*action, *binding);
        }
    }
    return table;
}

}  // namespace mc::input
