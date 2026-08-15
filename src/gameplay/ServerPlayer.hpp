#pragma once

// One connected player's authoritative state, the way 26.1's ServerPlayer owns
// its controller, inventory, vitals and action timeline. GameSession holds a
// slot map of these indexed by a stable PlayerId (the same shape EntitySystem
// uses: the id survives vector compaction). Today ReBedrock has a single local
// player, so the map has one entry; the structure exists so N2's multi-player
// and N3's snapshot can address a player by id without renaming the state.
//
// The three PlayerInput copies are kept together here: `stagedInput` is the
// render thread's write, `sharedInput` the hand-off under the input mutex, and
// `playerInput` the simulation's own snapshot at the top of each tick.

#include "gameplay/CraftingSystem.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerActionState.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/PlayerVitals.hpp"

#include <glm/vec3.hpp>

#include <cstdint>

namespace mc::gameplay {

// A stable identity for a connected player. The single local player uses
// kPrimaryPlayerId; remote players (LAN/dedicated, C-tier) get fresh ids.
using PlayerId = std::uint64_t;
inline constexpr PlayerId kPrimaryPlayerId = 1U;

struct ServerPlayer final {
    // PlayerController has no default constructor (it needs the spawn feet), so
    // neither does a player without a position.
    explicit ServerPlayer(glm::vec3 feet) : controller(feet) {}

    PlayerController controller;
    PlayerVitals vitals;
    Inventory inventory;
    CraftingSystem crafting;
    GameMode gameMode = GameMode::Creative;

    // The tick-owned swing/use timeline (N1), advanced with the world tick.
    PlayerActionState actions;

    // Three input copies on purpose: staged (render thread), shared (hand-off
    // under the mutex), simulation's own (refreshed each tick).
    PlayerInput stagedInput{};
    PlayerInput sharedInput{};
    PlayerInput playerInput{};

    // Interpolation endpoints the renderer draws between (physics partialTick).
    glm::vec3 physicsPrevious{0.0F};
    glm::vec3 physicsCurrent{0.0F};

    // The /spawnpoint result; death respawns here before the world spawn.
    glm::vec3 spawnPosition{24.0F, 76.38F, 24.0F};
    float spawnYaw = 0.0F;
    bool hasSpawn = false;

    float footstepDistance = 0.0F;
    bool previousInWater = false;

    // The vanilla 32-tick meal (N1's ItemUseState mirrors it for the animation).
    bool eating = false;
    const Item* eatingKind = nullptr;
    int eatTicks = 0;
};

} // namespace mc::gameplay
