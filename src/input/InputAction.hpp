#pragma once

#include <cstddef>
#include <cstdint>

namespace mc::input {

// The device-agnostic action layer. Everything the client can *do* is one of
// these; the physical key/button/axis that triggers it is a separate binding
// (see InputBinding). VulkanRenderer consumes actions, never raw GLFW codes.
//
// This deliberately does NOT include GLFW — the whole point (PX-1) is that the
// action/binding/edge machinery is a Vulkan-free *and* GLFW-free pure core that
// links into the runtime library and is exercised by headless unit tests. A thin
// GLFW adapter (GlfwInputBackend, GUI-only) translates GLFW codes into this
// core's Key/MouseButton/Gamepad enums.
enum class InputAction : std::uint8_t {
    MoveForward = 0,
    MoveBack,
    MoveLeft,
    MoveRight,
    Jump,
    Sneak,
    Sprint,
    Attack,
    Use,
    Inventory,
    Hotbar1,
    Hotbar2,
    Hotbar3,
    Hotbar4,
    Hotbar5,
    Hotbar6,
    Hotbar7,
    Hotbar8,
    Hotbar9,
    DropItem,
    Chat,
    Command,
    Perspective,
    Debug,
    Pause,

    Count
};

inline constexpr std::size_t kInputActionCount = static_cast<std::size_t>(InputAction::Count);

[[nodiscard]] constexpr std::size_t index(InputAction action) noexcept {
    return static_cast<std::size_t>(action);
}

// The nine hotbar slots are consecutive so a slot number maps to an action by
// arithmetic rather than a switch. Guards keep an out-of-range slot from walking
// off the enum.
[[nodiscard]] constexpr InputAction hotbarAction(std::size_t slotZeroBased) noexcept {
    return static_cast<InputAction>(index(InputAction::Hotbar1) + slotZeroBased);
}

[[nodiscard]] constexpr bool isHotbarAction(InputAction action) noexcept {
    return index(action) >= index(InputAction::Hotbar1) &&
           index(action) <= index(InputAction::Hotbar9);
}

[[nodiscard]] constexpr std::size_t hotbarSlot(InputAction action) noexcept {
    return index(action) - index(InputAction::Hotbar1);
}

}  // namespace mc::input
