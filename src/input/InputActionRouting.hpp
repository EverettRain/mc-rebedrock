#pragma once

// The per-action gate that decides which discrete action edges the client acts
// on given the current screen state. Pulled out of VulkanRenderer so the routing
// rule is a Vulkan-free, GLFW-free pure function the headless tests can pin —
// the PX-1 regression was exactly this rule being a single all-or-nothing
// `if (!gameplayEnabled) return;`, which stranded E (close inventory) and the
// debug/perspective toggles behind any open screen.
//
// The rule mirrors the pre-PX-1 per-key guards:
//   Debug / Perspective  — unconditional (the old F3/F5 branches had no guard).
//   Inventory            — while a world is up and the player is neither paused
//                          nor typing in chat, so E both opens and CLOSES the
//                          inventory (the inventory screen disables the gameplay
//                          poll; gating this on gameplayEnabled would strand it).
//   DropItem / Hotbar*   — strictly in play (gameplayEnabled).
//   everything else      — not routed here (mouse attack/use, movement, chat).

#include "input/InputAction.hpp"

namespace mc::input {

// The screen-state gate sampled once per frame by the client.
struct InputDispatchGate final {
    // worldReady && !inventoryOpen && !paused && !chatOpen: the player is in
    // active play with no screen up.
    bool gameplayEnabled = false;
    // worldReady && !paused && !chatOpen: a world exists and the player is not in
    // a menu or the chat line. True even while the inventory overlay is open, so
    // the inventory toggle can close it.
    bool inventoryToggleEnabled = false;
};

// Whether the given pressed action should take effect under this gate. Pure and
// total: an unmapped-here action returns false, so the caller's switch only acts
// on the ones this rule green-lights.
[[nodiscard]] constexpr bool shouldDispatchAction(InputAction action,
                                                  const InputDispatchGate& gate) noexcept {
    switch (action) {
        case InputAction::Debug:
        case InputAction::Perspective:
            return true;  // toggle from any screen state, matching vanilla F3/F5
        case InputAction::Inventory:
            return gate.inventoryToggleEnabled;
        case InputAction::DropItem:
            return gate.gameplayEnabled;
        default:
            return gate.gameplayEnabled && isHotbarAction(action);
    }
}

}  // namespace mc::input
