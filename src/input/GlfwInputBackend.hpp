#pragma once

// The GLFW-facing half of PX-1. It is the ONLY input file that includes
// <GLFW/glfw3.h>; it translates GLFW key/mouse/gamepad codes onto the Vulkan-
// free input core (Key/MouseButton/GamepadButton) and samples a window into a
// RawInputFrame. Because it drags in GLFW it lives outside mc_rebedrock_runtime
// and is compiled only by the GUI target — hence it cannot be exercised on the
// headless container and its wiring is verified on mac.

#include "input/InputSystem.hpp"
#include "input/RawInputFrame.hpp"

#include <GLFW/glfw3.h>

namespace mc::input {

// GLFW key code -> core Key. Unlisted keys map to Key::Unknown (harmless: they
// bind to no action). Only the keys rebedrock actually uses are translated.
[[nodiscard]] inline Key keyFromGlfw(int glfwKey) noexcept {
    switch (glfwKey) {
        case GLFW_KEY_W: return Key::W;
        case GLFW_KEY_A: return Key::A;
        case GLFW_KEY_S: return Key::S;
        case GLFW_KEY_D: return Key::D;
        case GLFW_KEY_Q: return Key::Q;
        case GLFW_KEY_E: return Key::E;
        case GLFW_KEY_T: return Key::T;
        case GLFW_KEY_SPACE: return Key::Space;
        case GLFW_KEY_LEFT_SHIFT: return Key::LeftShift;
        case GLFW_KEY_LEFT_CONTROL: return Key::LeftControl;
        case GLFW_KEY_ESCAPE: return Key::Escape;
        case GLFW_KEY_ENTER: return Key::Enter;
        case GLFW_KEY_KP_ENTER: return Key::Enter;
        case GLFW_KEY_TAB: return Key::Tab;
        case GLFW_KEY_BACKSPACE: return Key::Backspace;
        case GLFW_KEY_SLASH: return Key::Slash;
        case GLFW_KEY_F3: return Key::F3;
        case GLFW_KEY_F5: return Key::F5;
        case GLFW_KEY_1: return Key::Digit1;
        case GLFW_KEY_2: return Key::Digit2;
        case GLFW_KEY_3: return Key::Digit3;
        case GLFW_KEY_4: return Key::Digit4;
        case GLFW_KEY_5: return Key::Digit5;
        case GLFW_KEY_6: return Key::Digit6;
        case GLFW_KEY_7: return Key::Digit7;
        case GLFW_KEY_8: return Key::Digit8;
        case GLFW_KEY_9: return Key::Digit9;
        default: return Key::Unknown;
    }
}

[[nodiscard]] inline MouseButton mouseFromGlfw(int glfwButton) noexcept {
    switch (glfwButton) {
        case GLFW_MOUSE_BUTTON_LEFT: return MouseButton::Left;
        case GLFW_MOUSE_BUTTON_RIGHT: return MouseButton::Right;
        case GLFW_MOUSE_BUTTON_MIDDLE: return MouseButton::Middle;
        default: return MouseButton::Unknown;
    }
}

// The GLFW gamepad button ordering maps 1:1 to our GamepadButton enum for the
// buttons we care about.
[[nodiscard]] inline int glfwGamepadButton(GamepadButton button) noexcept {
    switch (button) {
        case GamepadButton::A: return GLFW_GAMEPAD_BUTTON_A;
        case GamepadButton::B: return GLFW_GAMEPAD_BUTTON_B;
        case GamepadButton::X: return GLFW_GAMEPAD_BUTTON_X;
        case GamepadButton::Y: return GLFW_GAMEPAD_BUTTON_Y;
        case GamepadButton::LeftBumper: return GLFW_GAMEPAD_BUTTON_LEFT_BUMPER;
        case GamepadButton::RightBumper: return GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER;
        case GamepadButton::Back: return GLFW_GAMEPAD_BUTTON_BACK;
        case GamepadButton::Start: return GLFW_GAMEPAD_BUTTON_START;
        case GamepadButton::LeftThumb: return GLFW_GAMEPAD_BUTTON_LEFT_THUMB;
        case GamepadButton::RightThumb: return GLFW_GAMEPAD_BUTTON_RIGHT_THUMB;
        case GamepadButton::Unknown: break;
    }
    return -1;
}

// Sample a live GLFW window + first connected gamepad into a RawInputFrame. The
// caller supplies the look direction (from the camera) since GLFW does not carry
// it. Keyboard/mouse are polled level-state; the key callback still produces the
// text-entry/menu edges, but the InputSystem now owns the gameplay level+edges.
inline void sampleGlfwWindow(GLFWwindow* window, glm::vec3 lookDirection, RawInputFrame& frame) {
    const auto sampleKey = [&](Key key, int glfwKey) {
        frame.setKey(key, glfwGetKey(window, glfwKey) == GLFW_PRESS);
    };
    sampleKey(Key::W, GLFW_KEY_W);
    sampleKey(Key::A, GLFW_KEY_A);
    sampleKey(Key::S, GLFW_KEY_S);
    sampleKey(Key::D, GLFW_KEY_D);
    sampleKey(Key::Q, GLFW_KEY_Q);
    sampleKey(Key::E, GLFW_KEY_E);
    sampleKey(Key::T, GLFW_KEY_T);
    sampleKey(Key::Space, GLFW_KEY_SPACE);
    sampleKey(Key::LeftShift, GLFW_KEY_LEFT_SHIFT);
    sampleKey(Key::LeftControl, GLFW_KEY_LEFT_CONTROL);
    sampleKey(Key::Slash, GLFW_KEY_SLASH);
    sampleKey(Key::Digit1, GLFW_KEY_1);
    sampleKey(Key::Digit2, GLFW_KEY_2);
    sampleKey(Key::Digit3, GLFW_KEY_3);
    sampleKey(Key::Digit4, GLFW_KEY_4);
    sampleKey(Key::Digit5, GLFW_KEY_5);
    sampleKey(Key::Digit6, GLFW_KEY_6);
    sampleKey(Key::Digit7, GLFW_KEY_7);
    sampleKey(Key::Digit8, GLFW_KEY_8);
    sampleKey(Key::Digit9, GLFW_KEY_9);

    frame.setMouse(MouseButton::Left,
                   glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    frame.setMouse(MouseButton::Right,
                   glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
    frame.setMouse(MouseButton::Middle,
                   glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS);

    frame.lookDirection = lookDirection;

    GLFWgamepadstate pad;
    frame.gamepadConnected = false;
    if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1) == GLFW_TRUE &&
        glfwGetGamepadState(GLFW_JOYSTICK_1, &pad) == GLFW_TRUE) {
        frame.gamepadConnected = true;
        for (std::size_t axis = 0; axis < kGamepadAxisCount; ++axis) {
            frame.gamepadAxes[axis] = pad.axes[axis];
        }
        for (std::uint16_t b = 1; b <= static_cast<std::uint16_t>(GamepadButton::RightThumb); ++b) {
            const int glfwButton = glfwGamepadButton(static_cast<GamepadButton>(b));
            if (glfwButton >= 0) {
                frame.setGamepadButton(static_cast<GamepadButton>(b),
                                       pad.buttons[glfwButton] == GLFW_PRESS);
            }
        }
    }
}

}  // namespace mc::input
