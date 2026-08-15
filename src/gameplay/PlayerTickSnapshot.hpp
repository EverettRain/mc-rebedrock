#pragma once

// The player's action and physics state, published once per simulation tick
// under the world write lock, so the render thread reads a single coherent
// snapshot instead of live gameplay objects the tick may be mid-mutation on.
// The renderer interpolates between the previous/current endpoints with its own
// per-frame alpha (animation §13.1: intra-frame pose = previous/current +
// partialTicks).
//
// Gameplay owns the type (the simulation publishes it); render/ consumes it.

#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerActionState.hpp"

#include <glm/vec3.hpp>

#include <cstdint>

namespace mc::gameplay {

struct PlayerTickSnapshot final {
    // The world tick this snapshot was taken at. The renderer uses it to know
    // when the snapshot advanced (a new tick between frames).
    std::uint64_t serverTick = 0U;

    // The swing/use timelines, with their previous/current progress endpoints.
    SwingState swing;
    ItemUseState use;

    // Physics interpolation endpoints (feet position).
    glm::vec3 physicsPrevious{0.0F};
    glm::vec3 physicsCurrent{0.0F};

    // Walk animation endpoints.
    float previousStride = 0.0F;
    float stride = 0.0F;
    float previousSpeed = 0.0F;
    float speed = 0.0F;

    bool sneaking = false;
    bool flying = false;
    bool sprinting = false;

    // The selected inventory stack, for the held-item render and the arm pose.
    ItemStack heldStack{};
};

} // namespace mc::gameplay
