#pragma once

// Which screen owns the input right now — stated once, here, instead of being
// re-derived by every event callback.
//
// The renderer's five GLFW callbacks (key, char, scroll, mouse button, cursor
// position) each used to open with their own chain of `if (capturing) … if (page
// == CreateWorld) … if (chatOpen) … if (paused) … if (inventoryOpen) …`, in a
// slightly different order each time. Five hand-maintained copies of one
// precedence rule is how an event ends up delivered to two screens, or to none:
// the class of bug where a press and its release land in different owners.
//
// So the precedence is one pure function over the flags, and each callback
// switches on its result. The order is the modal one, most captive first:
//
//   KeyCapture  a Controls row is waiting for the next key — it consumes that
//               key rather than letting it act as a menu or gameplay key.
//   TextField   the create/edit-world name field owns the keyboard.
//   Chat        the chat line owns the keyboard.
//   Inventory   the inventory/container overlay is up (an overlay on the game
//               page, not a menu page — which is why it is its own mode).
//   Menu        a menu page owns the screen (title, pause, options, the lists).
//   Play        no screen: the world has the input.
//
// The modes are mutually exclusive by construction — setPaused() closes the
// inventory and the chat line before it pauses — and this function makes that
// total rather than implied: every flag combination lands in exactly one mode.
//
// GLFW-free and Vulkan-free, so the rule is exercised by a headless test.

#include <cstdint>

namespace mc::input {

// The renderer flags a screen mode is derived from.
struct ScreenState final {
    bool keyCapturing = false;   // a Controls row is capturing the next key
    bool textFieldOpen = false;  // the create/edit-world name field is up
    bool chatOpen = false;
    bool inventoryOpen = false;
    bool paused = false;         // a menu page owns the screen
};

enum class ScreenMode : std::uint8_t {
    KeyCapture,
    TextField,
    Chat,
    Inventory,
    Menu,
    Play,
};

[[nodiscard]] constexpr ScreenMode screenModeOf(const ScreenState& state) noexcept {
    if (state.keyCapturing) {
        return ScreenMode::KeyCapture;
    }
    if (state.textFieldOpen) {
        return ScreenMode::TextField;
    }
    if (state.chatOpen) {
        return ScreenMode::Chat;
    }
    if (state.inventoryOpen) {
        return ScreenMode::Inventory;
    }
    if (state.paused) {
        return ScreenMode::Menu;
    }
    return ScreenMode::Play;
}

// Whether a menu page owns the screen for the purposes of pointer input. The
// key-capture and text-field modes are menu pages too (the Controls and
// create-world pages) — they only claim the KEYBOARD, so a click or a drag on
// them is still ordinary menu input.
[[nodiscard]] constexpr bool isMenuScreen(ScreenMode mode) noexcept {
    return mode == ScreenMode::Menu || mode == ScreenMode::KeyCapture ||
           mode == ScreenMode::TextField;
}

}  // namespace mc::input
