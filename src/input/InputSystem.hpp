#pragma once

#include "input/InputAction.hpp"
#include "input/InputBinding.hpp"
#include "input/RawInputFrame.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <glm/vec3.hpp>

namespace mc::input {

// The continuous movement intent the InputSystem derives each frame. Field-for-
// field the subset of gameplay::MovementInput the client fills from local input
// (the GUI adapter copies these across); kept independent so this core does not
// pull the gameplay headers. `forwardPressed` here is the fresh-forward edge the
// sprint double-tap window keys off; `jumpPressed` is the fresh-jump edge.
struct MovementIntent final {
    float forward = 0.0F;
    float strafe = 0.0F;
    glm::vec3 lookDirection{0.0F, 0.0F, -1.0F};
    bool jumpHeld = false;
    bool descendHeld = false;
    bool sneakHeld = false;
    bool sprintHeld = false;
    bool jumpPressed = false;
    bool forwardPressed = false;
};

// A discrete UI action event: an action that fired an edge this frame and is not
// part of continuous movement. VulkanRenderer drains these instead of matching
// GLFW_PRESS per key. `phase` distinguishes a fresh press from a release so the
// interaction (attack/use) can key its stop edge off the release.
enum class EventPhase : std::uint8_t { Pressed, Released };

struct ActionEvent final {
    InputAction action;
    EventPhase phase;
};

// The single input collection point. Holds the binding table, the previous
// frame's per-action level (for edge detection) and gamepad tuning. `poll()`
// takes a RawInputFrame and returns the derived movement intent while filling an
// event queue — zero heap allocation, the queue is a fixed inline array.
class InputSystem final {
  public:
    // The queue is bounded by the number of actions: at most one edge per action
    // per frame, so kInputActionCount slots can never overflow.
    static constexpr std::size_t kMaxEvents = kInputActionCount;

    struct EventQueue final {
        std::array<ActionEvent, kMaxEvents> events{};
        std::size_t count = 0;

        void clear() noexcept { count = 0; }
        void push(ActionEvent event) noexcept {
            if (count < kMaxEvents) {
                events[count++] = event;
            }
        }
        [[nodiscard]] std::size_t size() const noexcept { return count; }
        [[nodiscard]] const ActionEvent& operator[](std::size_t i) const noexcept {
            return events[i];
        }
    };

    InputSystem() = default;

    [[nodiscard]] const BindingTable& bindings() const noexcept { return bindings_; }
    void setBindings(const BindingTable& table) noexcept { bindings_ = table; }
    // Rebind a single action; the next poll's edge detection is unaffected by the
    // change because it compares the same *action* slot across frames.
    void rebind(InputAction action, InputBinding binding) noexcept {
        bindings_.bind(action, binding);
    }

    void setGamepadTuning(const GamepadTuning& tuning) noexcept { tuning_ = tuning; }
    [[nodiscard]] const GamepadTuning& gamepadTuning() const noexcept { return tuning_; }

    [[nodiscard]] bool isHeld(InputAction action) const noexcept {
        return current_[index(action)];
    }

    // Resets the edge history so the next frame reports no stale press/release
    // edges — used when a screen closes and gameplay resumes.
    void resetEdges() noexcept {
        previous_.fill(false);
        current_.fill(false);
    }

    // The whole per-frame derivation. `enableGameplayActions` gates the
    // continuous movement + gameplay edges (they must be suppressed while a menu
    // or chat is up); UI edges the caller wants even behind a screen can still be
    // read via the returned queue by leaving it true, but VulkanRenderer keeps
    // the historic behaviour: when a screen is up it sends a zeroed intent.
    MovementIntent poll(const RawInputFrame& frame, EventQueue& out,
                        bool enableGameplayActions = true) noexcept {
        out.clear();

        // 1. Sample every action's level for this frame (keyboard/mouse/pad),
        //    folding the gamepad face buttons onto the same action slots.
        for (std::size_t i = 0; i < kInputActionCount; ++i) {
            const auto action = static_cast<InputAction>(i);
            current_[i] = frame.isDown(bindings_.binding(action));
        }
        applyGamepadButtonOverlay(frame);

        // 2. Emit an edge for any action whose level changed since last frame.
        for (std::size_t i = 0; i < kInputActionCount; ++i) {
            const bool now = current_[i];
            const bool was = previous_[i];
            if (now && !was) {
                out.push(ActionEvent{static_cast<InputAction>(i), EventPhase::Pressed});
            } else if (!now && was) {
                out.push(ActionEvent{static_cast<InputAction>(i), EventPhase::Released});
            }
        }

        // 3. Derive the continuous movement intent from the action levels + the
        //    left stick. Gated so a screen zeros the player.
        MovementIntent intent;
        intent.lookDirection = frame.lookDirection;
        if (enableGameplayActions) {
            intent = deriveMovement(frame);
        }

        // 4. The previous frame becomes this frame for the next poll's edges.
        previous_ = current_;
        return intent;
    }

  private:
    // Face buttons OR onto the keyboard/mouse level for the same action so a pad
    // and a key are interchangeable. Mapped to the same actions the defaults use.
    void applyGamepadButtonOverlay(const RawInputFrame& frame) noexcept {
        if (!frame.gamepadConnected) {
            return;
        }
        const auto down = [&](GamepadButton button) {
            return frame.gamepadButtonDown[static_cast<std::size_t>(button)];
        };
        orAction(InputAction::Jump, down(GamepadButton::A));
        orAction(InputAction::Inventory, down(GamepadButton::Y));
        orAction(InputAction::DropItem, down(GamepadButton::X));
        orAction(InputAction::Sprint, down(GamepadButton::LeftThumb));
        orAction(InputAction::Sneak, down(GamepadButton::RightThumb));
        orAction(InputAction::Pause, down(GamepadButton::Back));
    }

    void orAction(InputAction action, bool down) noexcept {
        current_[index(action)] = current_[index(action)] || down;
    }

    [[nodiscard]] MovementIntent deriveMovement(const RawInputFrame& frame) const noexcept {
        MovementIntent intent;
        intent.lookDirection = frame.lookDirection;

        // Keyboard axis: forward = W - S, strafe = D - A (the exact expression
        // VulkanRenderer inlined, now sourced from the binding table).
        const float keyForward = (isHeld(InputAction::MoveForward) ? 1.0F : 0.0F) -
                                 (isHeld(InputAction::MoveBack) ? 1.0F : 0.0F);
        const float keyStrafe = (isHeld(InputAction::MoveRight) ? 1.0F : 0.0F) -
                                (isHeld(InputAction::MoveLeft) ? 1.0F : 0.0F);
        intent.forward = keyForward;
        intent.strafe = keyStrafe;

        // Left stick adds analog movement when no key is pushing that axis. Y is
        // optionally inverted so pushing the stick forward is +forward.
        if (frame.gamepadConnected) {
            const float rawY = frame.gamepadAxes[static_cast<std::size_t>(GamepadAxis::LeftY)];
            const float rawX = frame.gamepadAxes[static_cast<std::size_t>(GamepadAxis::LeftX)];
            const float stickForward =
                applyDeadZone(tuning_.invertLeftY ? -rawY : rawY);
            const float stickStrafe = applyDeadZone(rawX);
            if (keyForward == 0.0F) {
                intent.forward = stickForward;
            }
            if (keyStrafe == 0.0F) {
                intent.strafe = stickStrafe;
            }
        }

        intent.jumpHeld = isHeld(InputAction::Jump);
        intent.sneakHeld = isHeld(InputAction::Sneak);
        // Descend shares the sneak key (the historic LEFT_SHIFT dual-role for
        // creative flight descent).
        intent.descendHeld = isHeld(InputAction::Sneak);
        intent.sprintHeld = isHeld(InputAction::Sprint);

        // Edges: jump-pressed toggles creative flight; forward-pressed feeds the
        // sprint double-tap window. Both are fresh presses this frame.
        intent.jumpPressed = isHeld(InputAction::Jump) && !previous_[index(InputAction::Jump)];
        intent.forwardPressed =
            isHeld(InputAction::MoveForward) && !previous_[index(InputAction::MoveForward)];
        return intent;
    }

    [[nodiscard]] float applyDeadZone(float value) const noexcept {
        if (std::fabs(value) < tuning_.deadZone) {
            return 0.0F;
        }
        return value;
    }

    BindingTable bindings_ = BindingTable::defaults();
    GamepadTuning tuning_{};
    std::array<bool, kInputActionCount> previous_{};
    std::array<bool, kInputActionCount> current_{};
};

}  // namespace mc::input
