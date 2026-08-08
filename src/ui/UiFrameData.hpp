#pragma once

#include "gameplay/GameMode.hpp"
#include "gameplay/Inventory.hpp"

#include <cstddef>

namespace mc::gameplay {
class GameSession;
}

namespace mc::ui {

// The gameplay state the in-game HUD renders, captured once per frame from the
// game session so the draw passes read a consistent snapshot instead of poking
// live gameplay objects mid-frame. Container slots (the inventory grid, the
// crafting/furnace panels) are deliberately not copied: they are drawn as they
// are, and copying every slot every frame would duplicate the very state the
// panel is showing. Only the HUD's heads-up reads — vitals, the held stack and
// the game mode — ride the snapshot.
struct UiFrameData final {
    float health = 0.0F;
    int foodLevel = 0;
    int airTicks = 0;
    int ticksSinceDamage = 1000;
    gameplay::GameMode gameMode = gameplay::GameMode::Survival;
    bool eating = false;
    gameplay::ItemStack selectedStack{};
    std::size_t selectedHotbarSlot = 0;
};

} // namespace mc::ui
