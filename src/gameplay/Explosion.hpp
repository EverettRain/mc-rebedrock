#pragma once

// EXP-1: the explosion itself, as arithmetic over the world.
//
// 26.1's `ServerExplosion` is one class doing four separable things: cast rays
// to find which blocks give way, sample how much of an entity the blast can
// see, turn that into damage, and turn it into knockback. All four are pure
// functions of the world and the blast, so they live here and the session does
// the writing — the same split Sleep.hpp takes.
//
// The ray cast is vanilla's exactly, and it is deliberately not "optimised" into
// a sphere test: the shape of an explosion — why a wall shelters what is behind
// it, why the hole is not a ball — IS the ray cast. 1352 rays sounds like a lot
// and is: it is 6 faces of a 16x16x16 shell, and vanilla pays it too, because an
// explosion is rare and its shape is the whole point.

#include "world/Block.hpp"
#include "world/BlockPos.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace mc::gameplay {

// What one blast is. `radius` is vanilla's `radius` field (4.0 for TNT, 3.0 for
// a creeper, 5.0 for a bed in the nether), NOT a distance in blocks — the reach
// is derived from it.
struct ExplosionSpec final {
    glm::vec3 center{0.0F};
    float radius = 4.0F;
    // Whether blocks are destroyed at all. A bed in the nether and a creeper
    // both destroy; `/summon`-style cosmetic blasts would not.
    bool destroysBlocks = true;
};

namespace explosion_detail {

// The 16x16x16 shell vanilla walks: only the cells on the surface of the cube
// contribute a ray, which is what makes it 1352 and not 4096.
inline constexpr int kShell = 16;
inline constexpr float kStep = 0.3F;
inline constexpr float kStepCost = 0.22500001F;

// `remainingPower -= (resistance + 0.3F) * 0.3F` — the block's own explosion
// resistance, which this build already carries as BlockDefinition's
// blastResistance. Air offers none; a fluid offers its own (water is 100).
[[nodiscard]] inline float blockExplosionResistance(const world::World& world, int x, int y,
                                                    int z) {
    const auto block = world.block(x, y, z);
    if (block == world::Block::Air) {
        return 0.0F;
    }
    return world::blockDefinition(block).blastResistance;
}

} // namespace explosion_detail

// The blocks the blast breaks. Deterministic given `randomState`, which is the
// caller's LCG — the same "the caller owns and replays the stream" rule the loot
// roll takes, so a replayed tick blows the same hole.
[[nodiscard]] std::vector<world::BlockPos> explodedPositions(const world::World& world,
                                                             const ExplosionSpec& spec,
                                                             std::uint64_t& randomState);

// ServerExplosion#getSeenPercent: how much of a box the blast can see, sampled
// on a grid whose spacing comes from the box's own size. Returns 0..1.
[[nodiscard]] float seenPercent(const world::World& world, glm::vec3 center, glm::vec3 boxMin,
                                glm::vec3 boxMax);

// ExplosionDamageCalculator#getEntityDamageAmount:
//     p = (1 - dist/(2r)) * exposure ;  damage = (p*p + p) / 2 * 7 * (2r) + 1
// `distance` is the entity's distance from the blast centre.
[[nodiscard]] inline float explosionDamage(float radius, float distance, float exposure) {
    const float doubleRadius = radius * 2.0F;
    if (doubleRadius <= 0.0F) {
        return 0.0F;
    }
    const float normalised = distance / doubleRadius;
    if (normalised > 1.0F) {
        return 0.0F;
    }
    const float power = (1.0F - normalised) * exposure;
    return (power * power + power) / 2.0F * 7.0F * doubleRadius + 1.0F;
}

// The push, before knockback resistance: `(1 - dist/(2r)) * exposure` along the
// direction from the centre to the entity.
[[nodiscard]] inline float explosionKnockback(float radius, float distance, float exposure) {
    const float doubleRadius = radius * 2.0F;
    if (doubleRadius <= 0.0F) {
        return 0.0F;
    }
    const float normalised = distance / doubleRadius;
    return normalised > 1.0F ? 0.0F : (1.0F - normalised) * exposure;
}

// Whether a blast at `center` with this radius reaches `position` at all — the
// `dist > 1` early out hurtEntities makes, hoisted so a caller can skip the
// expensive exposure sampling.
[[nodiscard]] inline bool withinBlastReach(glm::vec3 center, float radius, glm::vec3 position) {
    const float doubleRadius = radius * 2.0F;
    if (doubleRadius <= 0.0F) {
        return false;
    }
    const glm::vec3 delta = position - center;
    return std::sqrt(glm::dot(delta, delta)) <= doubleRadius;
}

} // namespace mc::gameplay
