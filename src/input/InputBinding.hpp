#pragma once

#include "input/InputAction.hpp"

#include <array>
#include <cstdint>

namespace mc::input {

// The device family a binding lives on. A single action binds to exactly one
// physical control here (PX-1 does not need alt-bindings); adding a second
// binding table would be a straight parallel array if it were ever wanted.
enum class InputDevice : std::uint8_t {
    None = 0,     // Unbound: the action can never fire from this table.
    Keyboard,     // code is a Key.
    Mouse,        // code is a MouseButton.
    GamepadButton  // code is a GamepadButton.
};

// Device-agnostic key identifiers. Values are our own — the GLFW adapter maps
// GLFW_KEY_* onto these so this core never includes <GLFW/glfw3.h>. Only the
// keys rebedrock actually binds are enumerated; the adapter reports Unknown for
// anything else.
enum class Key : std::uint16_t {
    Unknown = 0,
    W, A, S, D, Q, E, T,
    Space,
    LeftShift,
    LeftControl,
    Escape,
    Enter,
    Tab,
    Backspace,
    Slash,
    F3,
    F5,
    Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9
};

enum class MouseButton : std::uint8_t {
    Unknown = 0,
    Left,
    Right,
    Middle
};

enum class GamepadButton : std::uint8_t {
    Unknown = 0,
    A,       // south — jump
    B,       // east
    X,       // west — drop
    Y,       // north — inventory
    LeftBumper,
    RightBumper,
    Back,    // pause
    Start,
    LeftThumb,   // sprint (stick click)
    RightThumb   // sneak (stick click)
};

// The gamepad analog axes, mirroring GLFW_GAMEPAD_AXIS_* ordering so the adapter
// copies straight through. Movement reads the left stick; look reads the right.
enum class GamepadAxis : std::uint8_t {
    LeftX = 0,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count
};
inline constexpr std::size_t kGamepadAxisCount = static_cast<std::size_t>(GamepadAxis::Count);

// One binding: which device, and the code within it. `code` is the raw enum
// value of the device-specific enum (Key / MouseButton / GamepadButton), stored
// as a plain integer so the struct stays trivially copyable and comparable.
struct InputBinding final {
    InputDevice device = InputDevice::None;
    std::uint16_t code = 0;

    [[nodiscard]] friend constexpr bool operator==(const InputBinding&,
                                                   const InputBinding&) = default;
};

[[nodiscard]] constexpr InputBinding keyboard(Key key) noexcept {
    return InputBinding{InputDevice::Keyboard, static_cast<std::uint16_t>(key)};
}
[[nodiscard]] constexpr InputBinding mouse(MouseButton button) noexcept {
    return InputBinding{InputDevice::Mouse, static_cast<std::uint16_t>(button)};
}
[[nodiscard]] constexpr InputBinding gamepad(GamepadButton button) noexcept {
    return InputBinding{InputDevice::GamepadButton, static_cast<std::uint16_t>(button)};
}

// The binding table: a dense array indexed by InputAction. deref = one array
// subscript, no map/string lookup on the hot path. Rebinding is a single-slot
// write; loading a config is a bulk copy over the defaults.
class BindingTable final {
  public:
    [[nodiscard]] constexpr const InputBinding& binding(InputAction action) const noexcept {
        return bindings_[index(action)];
    }

    constexpr void bind(InputAction action, InputBinding binding) noexcept {
        bindings_[index(action)] = binding;
    }

    // The default keyboard/mouse layout — the exact keys VulkanRenderer hard
    // coded before PX-1. constexpr so it bakes into .rodata.
    [[nodiscard]] static constexpr BindingTable defaults() noexcept {
        BindingTable table;
        table.bind(InputAction::MoveForward, keyboard(Key::W));
        table.bind(InputAction::MoveBack, keyboard(Key::S));
        table.bind(InputAction::MoveLeft, keyboard(Key::A));
        table.bind(InputAction::MoveRight, keyboard(Key::D));
        table.bind(InputAction::Jump, keyboard(Key::Space));
        table.bind(InputAction::Sneak, keyboard(Key::LeftShift));
        table.bind(InputAction::Sprint, keyboard(Key::LeftControl));
        table.bind(InputAction::Attack, mouse(MouseButton::Left));
        table.bind(InputAction::Use, mouse(MouseButton::Right));
        table.bind(InputAction::Inventory, keyboard(Key::E));
        table.bind(InputAction::Hotbar1, keyboard(Key::Digit1));
        table.bind(InputAction::Hotbar2, keyboard(Key::Digit2));
        table.bind(InputAction::Hotbar3, keyboard(Key::Digit3));
        table.bind(InputAction::Hotbar4, keyboard(Key::Digit4));
        table.bind(InputAction::Hotbar5, keyboard(Key::Digit5));
        table.bind(InputAction::Hotbar6, keyboard(Key::Digit6));
        table.bind(InputAction::Hotbar7, keyboard(Key::Digit7));
        table.bind(InputAction::Hotbar8, keyboard(Key::Digit8));
        table.bind(InputAction::Hotbar9, keyboard(Key::Digit9));
        table.bind(InputAction::DropItem, keyboard(Key::Q));
        table.bind(InputAction::Chat, keyboard(Key::T));
        table.bind(InputAction::Command, keyboard(Key::Slash));
        table.bind(InputAction::Perspective, keyboard(Key::F5));
        table.bind(InputAction::Debug, keyboard(Key::F3));
        table.bind(InputAction::Pause, keyboard(Key::Escape));
        return table;
    }

  private:
    std::array<InputBinding, kInputActionCount> bindings_{};
};

}  // namespace mc::input
