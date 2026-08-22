#include "input/BindingConfig.hpp"
#include "input/InputSystem.hpp"
#include "input/RawInputFrame.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>

using namespace mc::input;

namespace {

// Finds the first event for an action, or returns false if none fired.
bool hasEvent(const InputSystem::EventQueue& queue, InputAction action, EventPhase phase) {
    for (std::size_t i = 0; i < queue.size(); ++i) {
        if (queue[i].action == action && queue[i].phase == phase) {
            return true;
        }
    }
    return false;
}

// --- Binding table: the default layout maps the historic keys ----------------
void testDefaultBindingTable() {
    const BindingTable table = BindingTable::defaults();
    assert(table.binding(InputAction::MoveForward) == keyboard(Key::W));
    assert(table.binding(InputAction::MoveBack) == keyboard(Key::S));
    assert(table.binding(InputAction::MoveLeft) == keyboard(Key::A));
    assert(table.binding(InputAction::MoveRight) == keyboard(Key::D));
    // The sabotage target: jump must be Space, sneak must be LeftShift. A swap
    // here (jump firing sneak) is exactly Sabotage ①.
    assert(table.binding(InputAction::Jump) == keyboard(Key::Space));
    assert(table.binding(InputAction::Sneak) == keyboard(Key::LeftShift));
    assert(table.binding(InputAction::Sprint) == keyboard(Key::LeftControl));
    assert(table.binding(InputAction::Attack) == mouse(MouseButton::Left));
    assert(table.binding(InputAction::Use) == mouse(MouseButton::Right));
    // Hotbar slots are contiguous and map by arithmetic.
    for (std::size_t slot = 0; slot < 9; ++slot) {
        const auto action = hotbarAction(slot);
        assert(hotbarSlot(action) == slot);
        assert(table.binding(action).device == InputDevice::Keyboard);
    }
}

// --- Key -> action mapping: pressing W drives forward, D drives right ---------
void testKeyToMovementMapping() {
    InputSystem system;
    InputSystem::EventQueue queue;
    RawInputFrame frame;

    frame.setKey(Key::W, true);
    frame.setKey(Key::D, true);
    MovementIntent intent = system.poll(frame, queue);
    assert(intent.forward == 1.0F);
    assert(intent.strafe == 1.0F);

    // Opposite keys cancel — W and S both held = no forward.
    frame = RawInputFrame{};
    frame.setKey(Key::W, true);
    frame.setKey(Key::S, true);
    intent = system.poll(frame, queue);
    assert(intent.forward == 0.0F);

    // Sneak/sprint held levels flow through.
    frame = RawInputFrame{};
    frame.setKey(Key::LeftShift, true);
    frame.setKey(Key::LeftControl, true);
    intent = system.poll(frame, queue);
    assert(intent.sneakHeld);
    assert(intent.descendHeld);
    assert(intent.sprintHeld);
}

// --- Rebinding: after a rebind the new key drives the action -------------------
void testRebinding() {
    InputSystem system;
    InputSystem::EventQueue queue;

    // Move jump onto the E key. Space no longer jumps.
    system.rebind(InputAction::Jump, keyboard(Key::E));
    assert(system.bindings().binding(InputAction::Jump) == keyboard(Key::E));

    RawInputFrame frame;
    frame.setKey(Key::Space, true);
    MovementIntent intent = system.poll(frame, queue);
    assert(!intent.jumpHeld);  // old binding is dead

    frame = RawInputFrame{};
    frame.setKey(Key::E, true);
    intent = system.poll(frame, queue);
    assert(intent.jumpHeld);   // new binding fires
    assert(intent.jumpPressed);  // and it is a fresh edge
}

// --- Edge detection: pressed on the rising frame, released on the falling -----
void testEdgeDetection() {
    InputSystem system;
    InputSystem::EventQueue queue;
    RawInputFrame frame;

    // Frame 1: E goes down -> Inventory Pressed edge, no Released.
    frame.setKey(Key::E, true);
    system.poll(frame, queue);
    assert(hasEvent(queue, InputAction::Inventory, EventPhase::Pressed));
    assert(!hasEvent(queue, InputAction::Inventory, EventPhase::Released));

    // Frame 2: E still down -> no new edge (held, not re-pressed). This is the
    // Sabotage ② target: a broken released-derivation makes the key "stick" and
    // re-fire or never release.
    system.poll(frame, queue);
    assert(!hasEvent(queue, InputAction::Inventory, EventPhase::Pressed));
    assert(!hasEvent(queue, InputAction::Inventory, EventPhase::Released));

    // Frame 3: E released -> Released edge fires exactly once.
    frame.setKey(Key::E, false);
    system.poll(frame, queue);
    assert(!hasEvent(queue, InputAction::Inventory, EventPhase::Pressed));
    assert(hasEvent(queue, InputAction::Inventory, EventPhase::Released));

    // Frame 4: still up -> quiet.
    system.poll(frame, queue);
    assert(queue.size() == 0);
}

// --- Jump/forward press edges only fire on the rising frame -------------------
void testMovementEdges() {
    InputSystem system;
    InputSystem::EventQueue queue;
    RawInputFrame frame;

    frame.setKey(Key::Space, true);
    MovementIntent intent = system.poll(frame, queue);
    assert(intent.jumpHeld && intent.jumpPressed);

    // Held on the next frame: still held, but no longer a fresh press.
    intent = system.poll(frame, queue);
    assert(intent.jumpHeld && !intent.jumpPressed);

    // Forward press edge feeds the sprint double-tap window.
    frame = RawInputFrame{};
    frame.setKey(Key::W, true);
    intent = system.poll(frame, queue);
    assert(intent.forwardPressed);
    intent = system.poll(frame, queue);
    assert(!intent.forwardPressed);
}

// --- UI hotbar/mouse events flow through the queue ---------------------------
void testHotbarAndMouseEvents() {
    InputSystem system;
    InputSystem::EventQueue queue;
    RawInputFrame frame;

    frame.setKey(Key::Digit3, true);
    system.poll(frame, queue);
    assert(hasEvent(queue, InputAction::Hotbar3, EventPhase::Pressed));

    frame = RawInputFrame{};
    frame.setMouse(MouseButton::Left, true);
    system.poll(frame, queue);
    assert(hasEvent(queue, InputAction::Attack, EventPhase::Pressed));
    frame.setMouse(MouseButton::Left, false);
    system.poll(frame, queue);
    assert(hasEvent(queue, InputAction::Attack, EventPhase::Released));
}

// --- Screen gating: gameplay actions suppressed but the intent look survives ---
void testGameplayGating() {
    InputSystem system;
    InputSystem::EventQueue queue;
    RawInputFrame frame;
    frame.setKey(Key::W, true);
    frame.lookDirection = {0.0F, 0.0F, 1.0F};
    MovementIntent intent = system.poll(frame, queue, /*enableGameplayActions=*/false);
    assert(intent.forward == 0.0F);
    assert(intent.lookDirection.z == 1.0F);
}

// --- Gamepad: left stick drives movement, deadzone kills drift, sign is right --
void testGamepadAxes() {
    InputSystem system;
    InputSystem::EventQueue queue;
    GamepadTuning tuning;
    tuning.deadZone = 0.2F;
    tuning.invertLeftY = true;
    system.setGamepadTuning(tuning);

    RawInputFrame frame;
    frame.gamepadConnected = true;

    // Push the stick fully forward: GLFW reports -1 on LeftY, and invertLeftY
    // flips it to +forward. This is the Sabotage ③ target (axis sign reversed =
    // forward becomes backward).
    frame.setAxis(GamepadAxis::LeftY, -1.0F);
    MovementIntent intent = system.poll(frame, queue);
    assert(intent.forward > 0.0F);

    // Pull it back: forward goes negative.
    frame.setAxis(GamepadAxis::LeftY, 1.0F);
    intent = system.poll(frame, queue);
    assert(intent.forward < 0.0F);

    // A small drift inside the dead zone reads as zero.
    frame.setAxis(GamepadAxis::LeftY, -0.1F);
    frame.setAxis(GamepadAxis::LeftX, 0.05F);
    intent = system.poll(frame, queue);
    assert(intent.forward == 0.0F);
    assert(intent.strafe == 0.0F);

    // Right stick X drives strafe.
    frame = RawInputFrame{};
    frame.gamepadConnected = true;
    frame.setAxis(GamepadAxis::LeftX, 0.9F);
    intent = system.poll(frame, queue);
    assert(intent.strafe > 0.0F);
}

// --- Gamepad face buttons OR onto the same actions as keys --------------------
void testGamepadButtons() {
    InputSystem system;
    InputSystem::EventQueue queue;
    RawInputFrame frame;
    frame.gamepadConnected = true;
    frame.setGamepadButton(GamepadButton::A, true);  // jump
    MovementIntent intent = system.poll(frame, queue);
    assert(intent.jumpHeld);
    assert(hasEvent(queue, InputAction::Jump, EventPhase::Pressed));

    frame.setGamepadButton(GamepadButton::Y, true);  // inventory
    system.poll(frame, queue);
    assert(hasEvent(queue, InputAction::Inventory, EventPhase::Pressed));
}

// --- resetEdges clears history so no stale edges fire on resume ---------------
void testResetEdges() {
    InputSystem system;
    InputSystem::EventQueue queue;
    RawInputFrame frame;
    frame.setKey(Key::Space, true);
    system.poll(frame, queue);  // jump pressed, now held
    system.resetEdges();
    // With history cleared, a still-held key re-reports as a fresh press.
    MovementIntent intent = system.poll(frame, queue);
    assert(intent.jumpPressed);
}

// --- Config round-trip: serialize -> parse reproduces the table exactly -------
void testConfigRoundTrip() {
    BindingTable table = BindingTable::defaults();
    table.bind(InputAction::Jump, keyboard(Key::E));       // rebound key
    table.bind(InputAction::Attack, mouse(MouseButton::Right));  // rebound to a mouse button
    table.bind(InputAction::Sprint, gamepad(GamepadButton::LeftThumb));  // to a pad button

    const std::string text = serializeBindings(table);
    const BindingTable parsed = parseBindings(text);
    for (std::size_t i = 0; i < kInputActionCount; ++i) {
        const auto action = static_cast<InputAction>(i);
        assert(parsed.binding(action) == table.binding(action));
    }

    // A partial/garbage config keeps the built-in defaults for missing lines.
    const BindingTable partial = parseBindings("jump=key.E\nbogus_line\n=\nnope=key.ZZ\n");
    assert(partial.binding(InputAction::Jump) == keyboard(Key::E));
    assert(partial.binding(InputAction::MoveForward) == keyboard(Key::W));  // untouched default
}

// --- Zero-allocation poll: the queue is fixed-size and never overflows --------
void testZeroAllocQueueBound() {
    static_assert(InputSystem::kMaxEvents == kInputActionCount,
                  "queue must be bounded by the action count");
    InputSystem system;
    InputSystem::EventQueue queue;
    RawInputFrame frame;
    // Slam every keyboard-bound action down in one frame; the queue must hold
    // them all without growing past its inline storage.
    for (std::size_t i = 0; i < kInputActionCount; ++i) {
        const auto& binding = BindingTable::defaults().binding(static_cast<InputAction>(i));
        if (binding.device == InputDevice::Keyboard) {
            frame.keyDown[binding.code] = true;
        } else if (binding.device == InputDevice::Mouse) {
            frame.mouseDown[binding.code] = true;
        }
    }
    system.poll(frame, queue);
    assert(queue.size() <= InputSystem::kMaxEvents);
    assert(queue.size() > 0);
}

}  // namespace

int main() {
    testDefaultBindingTable();
    testKeyToMovementMapping();
    testRebinding();
    testEdgeDetection();
    testMovementEdges();
    testHotbarAndMouseEvents();
    testGameplayGating();
    testGamepadAxes();
    testGamepadButtons();
    testResetEdges();
    testConfigRoundTrip();
    testZeroAllocQueueBound();
    return 0;
}
