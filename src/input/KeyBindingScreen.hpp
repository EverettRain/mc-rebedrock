#pragma once

// PX-5: the Key Binds screen's rebinding logic, as a Vulkan-free state object
// over the PX-1 InputSystem — the SINGLE SOURCE of bindings. Clicking a row
// begins a capture; the next key/mouse press is written straight into the
// InputSystem's binding table via rebind(), so the game's actual controls change
// (there is no private copy). A rebind that collides with another action's
// binding is reported so the page can warn; resetting restores the defaults.
//
// Headless-testable: construct with an InputSystem, beginCapture(action),
// applyKey(Key) — then assert system.bindings() changed, that a conflict is
// flagged, and that resetToDefaults() restores the vanilla layout. No GLFW, no
// Vulkan, no window.

#include "input/InputBinding.hpp"
#include "input/InputSystem.hpp"

#include <optional>

namespace mc::input {

// The result of applying a captured control to the action being rebound.
struct RebindResult final {
    bool applied = false;               // the binding was written
    bool conflict = false;              // another action already used this control
    InputAction conflictingAction = InputAction::Count;  // valid only if conflict
};

// Owns only the transient "which action is capturing" state; the bindings live
// in the InputSystem it points at. The screen never copies the table.
class KeyBindingScreen final {
  public:
    explicit KeyBindingScreen(InputSystem& system) noexcept : system_{&system} {}

    // Whether a row is currently waiting for the next key press.
    [[nodiscard]] bool capturing() const noexcept { return capturing_.has_value(); }
    [[nodiscard]] InputAction capturingAction() const noexcept {
        return capturing_.value_or(InputAction::Count);
    }

    // Begin (or cancel) capturing for an action's row. Clicking the same row again
    // cancels; clicking a different row moves the capture.
    void beginCapture(InputAction action) noexcept {
        if (capturing_.has_value() && *capturing_ == action) {
            capturing_.reset();  // toggle off
        } else {
            capturing_ = action;
        }
    }

    void cancelCapture() noexcept { capturing_.reset(); }

    // The binding an action currently points at, read straight from the source.
    [[nodiscard]] InputBinding bindingOf(InputAction action) const noexcept {
        return system_->bindings().binding(action);
    }

    // Apply an arbitrary captured control to the capturing row. If nothing is
    // capturing this is a no-op. Detects a conflict (the control already belongs
    // to another action) but still applies — vanilla rebinds and shows the clash;
    // the caller may then choose to also unbind the loser. Ends the capture.
    RebindResult applyBinding(InputBinding binding) noexcept {
        RebindResult result;
        if (!capturing_.has_value()) {
            return result;
        }
        const InputAction target = *capturing_;
        // Find any OTHER action already bound to this control.
        for (std::size_t i = 0; i < kInputActionCount; ++i) {
            const auto other = static_cast<InputAction>(i);
            if (other == target) {
                continue;
            }
            if (system_->bindings().binding(other) == binding &&
                binding.device != InputDevice::None) {
                result.conflict = true;
                result.conflictingAction = other;
                break;
            }
        }
        system_->rebind(target, binding);  // single source: write through
        result.applied = true;
        capturing_.reset();
        return result;
    }

    // Convenience overloads for the two device families the screen captures.
    RebindResult applyKey(Key key) noexcept { return applyBinding(keyboard(key)); }
    RebindResult applyMouse(MouseButton button) noexcept { return applyBinding(mouse(button)); }

    // Restore the vanilla defaults through the single source, ending any capture.
    void resetToDefaults() noexcept {
        system_->setBindings(BindingTable::defaults());
        capturing_.reset();
    }

  private:
    InputSystem* system_;
    std::optional<InputAction> capturing_{};
};

}  // namespace mc::input
