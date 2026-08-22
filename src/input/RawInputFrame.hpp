#pragma once

#include "input/InputBinding.hpp"

#include <array>
#include <cstddef>
#include <glm/vec3.hpp>

namespace mc::input {

// A single frame's raw device state, device-agnostic. The GLFW backend fills
// this by translating GLFW codes; tests fill it directly with no window. The
// InputSystem consumes it and derives held/pressed/released per action.
//
// Keyboard/mouse are level state (down now?). The gamepad carries analog axes
// plus button levels. Everything is fixed-size so a frame costs no allocation.
struct RawInputFrame final {
    // Indexed by the raw enum value of Key / MouseButton; true == down this frame.
    std::array<bool, 64> keyDown{};
    std::array<bool, 8> mouseDown{};

    bool gamepadConnected = false;
    std::array<float, kGamepadAxisCount> gamepadAxes{};
    std::array<bool, 16> gamepadButtonDown{};

    // The camera facing this frame, passed through to MovementInput. Not a
    // "device" but it is sampled at the same point, so it rides along.
    glm::vec3 lookDirection{0.0F, 0.0F, -1.0F};

    void setKey(Key key, bool down) noexcept {
        keyDown[static_cast<std::size_t>(key)] = down;
    }
    void setMouse(MouseButton button, bool down) noexcept {
        mouseDown[static_cast<std::size_t>(button)] = down;
    }
    void setGamepadButton(GamepadButton button, bool down) noexcept {
        gamepadButtonDown[static_cast<std::size_t>(button)] = down;
    }
    void setAxis(GamepadAxis axis, float value) noexcept {
        gamepadAxes[static_cast<std::size_t>(axis)] = value;
    }

    [[nodiscard]] bool isDown(const InputBinding& binding) const noexcept {
        switch (binding.device) {
            case InputDevice::Keyboard:
                return keyDown[binding.code];
            case InputDevice::Mouse:
                return mouseDown[binding.code];
            case InputDevice::GamepadButton:
                return gamepadConnected && gamepadButtonDown[binding.code];
            case InputDevice::None:
                break;
        }
        return false;
    }
};

// Tunables for translating stick axes into movement/look. Configurable per the
// PX-1 "死区/灵敏度可配" requirement.
struct GamepadTuning final {
    float deadZone = 0.15F;
    float lookSensitivity = 1.0F;
    // Some pads/APIs report the Y axis pushed-up as negative; the adapter can
    // flip it here so forward is always +1 without touching the mapping code.
    bool invertLeftY = true;
    bool invertRightY = false;
};

}  // namespace mc::input
