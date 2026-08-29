#include "input/InputActionRouting.hpp"
#include "input/ScreenMode.hpp"

#include <cassert>

// The modal precedence (input/ScreenMode.hpp) the renderer's five window
// callbacks all route through, and the action gate derived from it.
//
// This rule used to exist as five hand-written chains of flag tests inside the
// GLFW callbacks, in a slightly different order each — plus two more copies as
// the `worldReady && !inventoryOpen && !paused && !chatOpen` expressions that
// gated the per-frame input poll. None of that is reachable from a headless
// test, which is exactly why it is a pure function now. The equivalence pass at
// the bottom brute-forces every flag combination against the old expressions, so
// the unification is provably a refactor.

namespace {

using namespace mc::input;

ScreenMode modeOf(bool capture, bool textField, bool chat, bool inventory, bool paused) {
    return screenModeOf(ScreenState{capture, textField, chat, inventory, paused});
}

}  // namespace

int main() {
    // --- Precedence, most captive first. ---
    {
        // Nothing up: the world owns the input.
        assert(modeOf(false, false, false, false, false) == ScreenMode::Play);
        assert(modeOf(false, false, false, false, true) == ScreenMode::Menu);
        assert(modeOf(false, false, false, true, false) == ScreenMode::Inventory);
        assert(modeOf(false, false, true, false, false) == ScreenMode::Chat);
        assert(modeOf(false, true, false, false, true) == ScreenMode::TextField);
        assert(modeOf(true, false, false, false, true) == ScreenMode::KeyCapture);

        // A capturing key-bind row outranks the menu page it sits on, and the
        // world-name field outranks everything below it.
        assert(modeOf(true, true, true, true, true) == ScreenMode::KeyCapture);
        assert(modeOf(false, true, true, true, true) == ScreenMode::TextField);
        assert(modeOf(false, false, true, true, true) == ScreenMode::Chat);
        assert(modeOf(false, false, false, true, true) == ScreenMode::Inventory);
    }

    // --- Pointer input: a capturing row and a name field are still menu pages,
    //     so clicks, drags and the wheel keep working on them. ---
    {
        assert(isMenuScreen(ScreenMode::Menu));
        assert(isMenuScreen(ScreenMode::KeyCapture));
        assert(isMenuScreen(ScreenMode::TextField));
        assert(!isMenuScreen(ScreenMode::Inventory));
        assert(!isMenuScreen(ScreenMode::Chat));
        assert(!isMenuScreen(ScreenMode::Play));
    }

    // --- The gate: only Play drives the world; the inventory toggle also has to
    //     work FROM the inventory, or E could open it and never close it. ---
    {
        const auto play = dispatchGateFor(ScreenMode::Play, /*worldReady=*/true);
        assert(play.gameplayEnabled && play.inventoryToggleEnabled);
        const auto inventory = dispatchGateFor(ScreenMode::Inventory, true);
        assert(!inventory.gameplayEnabled && inventory.inventoryToggleEnabled);
        const auto menu = dispatchGateFor(ScreenMode::Menu, true);
        assert(!menu.gameplayEnabled && !menu.inventoryToggleEnabled);
        // No world means no gameplay input at all, whatever the screen says.
        const auto loading = dispatchGateFor(ScreenMode::Play, /*worldReady=*/false);
        assert(!loading.gameplayEnabled && !loading.inventoryToggleEnabled);

        // E closes the inventory; the debug/perspective toggles work from any
        // screen; the strictly in-play actions do not.
        assert(shouldDispatchAction(InputAction::Inventory, inventory));
        assert(!shouldDispatchAction(InputAction::DropItem, inventory));
        assert(shouldDispatchAction(InputAction::Debug, menu));
        assert(shouldDispatchAction(InputAction::Perspective, menu));
        assert(!shouldDispatchAction(InputAction::Inventory, menu));
        assert(shouldDispatchAction(InputAction::Hotbar1, play));
        assert(!shouldDispatchAction(InputAction::Hotbar1, inventory));
    }

    // --- Equivalence with the expressions this replaced, over every input. ---
    {
        for (int bits = 0; bits < 64; ++bits) {
            const bool capture = (bits & 1) != 0;
            const bool textField = (bits & 2) != 0;
            const bool chat = (bits & 4) != 0;
            const bool inventory = (bits & 8) != 0;
            const bool paused = (bits & 16) != 0;
            const bool worldReady = (bits & 32) != 0;
            // A capturing row and the world-name field only exist on a menu
            // page, so those states always carry paused — the combinations the
            // renderer can actually reach.
            if ((capture || textField) && !paused) {
                continue;
            }
            // "Inventory open AND paused" is the one state where this rule and
            // the expressions it replaced disagree, and it is unreachable:
            // setPaused(true) closes the inventory before it pauses. It is also
            // the state the five old callbacks disagreed with EACH OTHER about
            // (the cursor and Escape paths treated the inventory as the owner,
            // the mouse-button path treated the pause menu as the owner) — which
            // is the ambiguity a single total rule exists to remove. Asserted
            // on its own below.
            if (inventory && paused) {
                continue;
            }
            const auto gate =
                dispatchGateFor(modeOf(capture, textField, chat, inventory, paused), worldReady);
            assert(gate.gameplayEnabled == (worldReady && !inventory && !paused && !chat));
            assert(gate.inventoryToggleEnabled == (worldReady && !paused && !chat));
        }
    }

    // --- The one state the old expressions and this rule differ on. ---
    {
        // Unreachable in the renderer (setPaused closes the inventory first), so
        // no behaviour rides on it — but the rule is total, so it has an answer:
        // the inventory overlay owns the input, which is what the Escape path and
        // the cursor path already did. Pinned so a future change that makes the
        // state reachable has to look at this deliberately.
        assert(modeOf(false, false, false, /*inventory=*/true, /*paused=*/true) ==
               ScreenMode::Inventory);
        const auto gate = dispatchGateFor(ScreenMode::Inventory, /*worldReady=*/true);
        assert(!gate.gameplayEnabled);
        assert(gate.inventoryToggleEnabled);  // the old expression said false here
    }

    return 0;
}
