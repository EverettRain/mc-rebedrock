#include "gameplay/entities/ExperienceOrb.hpp"

#include "gameplay/PlayerExperience.hpp"
#include "world/Block.hpp"
#include "world/World.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace mc::gameplay {
namespace {

// ExperienceOrb's AABB is 0.5x0.5x0.5 (EntityType.EXPERIENCE_ORB `.sized(0.5F,
// 0.5F)`); half-extent for the per-axis collision probe below.
constexpr float kHalfExtent = 0.25F;
// ExperienceOrb.getDefaultGravity(): 0.03/tick, lighter than an item's 0.04 —
// orbs hang in the air a little longer.
constexpr float kGravity = 0.03F;
constexpr float kAirDrag = 0.98F;
// followNearbyPlayer's MAX_FOLLOW_DIST, squared for the cheap compare.
constexpr float kFollowRadius = 8.0F;
constexpr float kFollowRadiusSquared = kFollowRadius * kFollowRadius;
// ORB_MERGE_DISTANCE (0.5) inflated the same way scanForMerges inflates its
// own bounding box by 0.5 on every side before the neighbour query.
constexpr float kMergeDistance = 0.5F;
constexpr float kMergeDistanceSquared = kMergeDistance * kMergeDistance;
// LIFETIME: 6000 ticks (5 minutes at 20 TPS) before an unclaimed orb despawns.
constexpr std::uint32_t kLifetimeTicks = 6000U;
// The XP-1 same-tick "don't touch what you just placed" guard (see
// ExperienceOrb.hpp's pickupDelayTicks doc comment).
constexpr std::uint32_t kSpawnPickupDelayTicks = 2U;

[[nodiscard]] bool collidesAt(const world::World& world, glm::vec3 centre) {
    const glm::vec3 minimum = centre - glm::vec3{kHalfExtent};
    const glm::vec3 maximum = centre + glm::vec3{kHalfExtent};
    const int minX = static_cast<int>(std::floor(minimum.x + 1e-4F));
    const int maxX = static_cast<int>(std::floor(maximum.x - 1e-4F));
    const int minY = static_cast<int>(std::floor(minimum.y + 1e-4F));
    const int maxY = static_cast<int>(std::floor(maximum.y - 1e-4F));
    const int minZ = static_cast<int>(std::floor(minimum.z + 1e-4F));
    const int maxZ = static_cast<int>(std::floor(maximum.z - 1e-4F));
    for (int y = minY; y <= maxY; ++y) {
        if (!world::isWorldYInRange(y)) continue;
        for (int z = minZ; z <= maxZ; ++z) {
            for (int x = minX; x <= maxX; ++x) {
                if (world::hasCollision(world.block(x, y, z))) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

std::int32_t experienceOrbDenomination(std::int32_t amount) {
    // ExperienceOrb.getExperienceValue: the largest fixed denomination not
    // exceeding `maxValue`, descending table ported verbatim.
    if (amount >= 2477) return 2477;
    if (amount >= 1237) return 1237;
    if (amount >= 617) return 617;
    if (amount >= 307) return 307;
    if (amount >= 149) return 149;
    if (amount >= 73) return 73;
    if (amount >= 37) return 37;
    if (amount >= 17) return 17;
    if (amount >= 7) return 7;
    return amount >= 3 ? 3 : 1;
}

void ExperienceOrbSystem::spawnOne(glm::vec3 position, std::int32_t value,
                                   world::gen::JavaRandom& rng) {
    if (value <= 0) {
        return;
    }
    // ExperienceOrb(Level, Vec3 pos, Vec3 roughly, int value): the random
    // scatter velocity, ported bit for bit (roughDirection is always ZERO from
    // spawnMany's award()-equivalent call, so the "flip toward roughly" branch
    // never triggers here — matching 26.1's own award()/awardWithDirection(pos,
    // Vec3.ZERO, amount) call site, since XP-1 has no directional-award caller
    // yet).
    const glm::vec3 randomMovement{
        (rng.nextDouble() * 0.2 - 0.1) * 2.0,
        rng.nextDouble() * 0.2 * 2.0,
        (rng.nextDouble() * 0.2 - 0.1) * 2.0,
    };
    entities_.push_back({position, position, randomMovement, value, 1, 0U,
                         kSpawnPickupDelayTicks});
}

void ExperienceOrbSystem::spawnMany(glm::vec3 position, std::int32_t amount,
                                    world::gen::JavaRandom& rng) {
    // ExperienceOrb.awardWithDirection: split into vanilla's fixed
    // denominations, largest-first, each becoming its own orb. XP-1 does not
    // yet reimplement tryMergeToExisting's same-tick "search a random group id"
    // pre-merge (that is an optimisation to keep award() O(log amount) orbs
    // even for huge amounts, not a behaviour vanilla depends on): the ordinary
    // scanForMerges pass in tick() folds same-value orbs together on the very
    // next tick regardless, so a spawnMany call still converges to the same
    // steady-state entity count.
    while (amount > 0) {
        const std::int32_t denomination = experienceOrbDenomination(amount);
        amount -= denomination;
        spawnOne(position, denomination, rng);
    }
}

void ExperienceOrbSystem::restore(glm::vec3 position, glm::vec3 velocity, std::int32_t value,
                                  std::int32_t count, std::uint32_t ageTicks,
                                  std::uint32_t pickupDelayTicks) {
    if (value <= 0 || count <= 0) {
        return;
    }
    entities_.push_back({position, position, velocity, value, count, ageTicks, pickupDelayTicks});
}

std::int32_t ExperienceOrbSystem::tick(const world::World& world, glm::vec3 playerPosition,
                                       bool playerAlive, PlayerExperience& experience) {
    std::int32_t collectedPoints = 0;

    // followNearbyPlayer: within 8 blocks of the nearest ALIVE player (there is
    // exactly one player in this build, so "nearest" degenerates to "the
    // player, if alive and in range" — a future multi-player XP-2+ extension
    // would loop candidates here the way EntitySystem's own player-push logic
    // will need to).
    for (auto& orb : entities_) {
        if (!playerAlive) {
            continue;
        }
        // ExperienceOrb.followNearbyPlayer: aim at the player's centre plus half
        // their eye height, not their feet — an orb visibly rises to meet a
        // standing player rather than skimming the ground.
        constexpr float kAimHeightOffset = 0.81F;  // eyeHeight(1.62)/2, PlayerController::kEyeHeight
        const glm::vec3 aimPoint = playerPosition + glm::vec3{0.0F, kAimHeightOffset, 0.0F};
        const glm::vec3 delta = aimPoint - orb.position;
        const float distanceSquared = glm::dot(delta, delta);
        if (distanceSquared > kFollowRadiusSquared || distanceSquared <= 1e-8F) {
            continue;
        }
        const float power = 1.0F - std::sqrt(distanceSquared) / kFollowRadius;
        const glm::vec3 direction = delta / std::sqrt(distanceSquared);
        orb.velocity += direction * (power * power * 0.1F);
    }

    // Physics: gravity, drag, underwater float, per-axis collision-resolved
    // move — the same shape as ItemEntitySystem::tick's loop, with orb-specific
    // gravity/drag constants and no floor-slipperiness term (vanilla's orb
    // tick has none; only items read block friction).
    for (auto& orb : entities_) {
        orb.previousPosition = orb.position;
        ++orb.ageTicks;
        if (orb.pickupDelayTicks > 0U) {
            --orb.pickupDelayTicks;
        }

        const bool colliding = collidesAt(world, orb.position);
        const auto foot = orb.position - glm::vec3{0.0F, 0.2F, 0.0F};
        const bool inWater = world::isFluid(world.block(static_cast<int>(std::floor(orb.position.x)),
                                                        static_cast<int>(std::floor(foot.y)),
                                                        static_cast<int>(std::floor(orb.position.z))));
        if (inWater) {
            // ExperienceOrb.setUnderwaterMovement: damp horizontally, float up
            // gently (capped ascent, never a plunge).
            orb.velocity.x *= 0.99F;
            orb.velocity.y = std::min(orb.velocity.y + 5.0e-4F, 0.06F);
            orb.velocity.z *= 0.99F;
        } else if (!colliding) {
            orb.velocity.y -= kGravity;
        }

        const auto moveAxis = [&](int axis, float amount) {
            if (std::abs(amount) < 1e-4F) {
                return;
            }
            glm::vec3 candidate = orb.position;
            candidate[axis] += amount;
            if (!collidesAt(world, candidate)) {
                orb.position = candidate;
                return;
            }
            float safe = 0.0F;
            float blocked = 1.0F;
            for (int iteration = 0; iteration < 12; ++iteration) {
                const float middle = (safe + blocked) * 0.5F;
                candidate = orb.position;
                candidate[axis] += amount * middle;
                if (collidesAt(world, candidate)) {
                    blocked = middle;
                } else {
                    safe = middle;
                }
            }
            orb.position[axis] += amount * safe;
            orb.velocity[axis] = 0.0F;
        };
        moveAxis(1, orb.velocity.y);
        moveAxis(0, orb.velocity.x);
        moveAxis(2, orb.velocity.z);

        orb.velocity *= kAirDrag;
    }

    // scanForMerges: fold same-value orbs within the merge radius into one
    // record, summing count (not value — vanilla's merge keeps the smaller
    // denomination, it just stacks how many of it there are). Value
    // conservation: every merge moves `other.count` into `orb.count` and zeros
    // `other.count`, so the sum of value*count over all surviving+removed
    // orbs is unchanged (the sabotage③-target invariant).
    for (std::size_t index = 0; index < entities_.size(); ++index) {
        auto& orb = entities_[index];
        if (orb.count <= 0) {
            continue;
        }
        for (std::size_t other = index + 1; other < entities_.size(); ++other) {
            auto& candidate = entities_[other];
            if (candidate.count <= 0 || candidate.value != orb.value) {
                continue;
            }
            const glm::vec3 delta = candidate.position - orb.position;
            if (glm::dot(delta, delta) > kMergeDistanceSquared) {
                continue;
            }
            // ExperienceOrb.merge: count sums, age takes the younger (smaller)
            // of the two so a long-idle orb absorbing a fresh one does not
            // instantly despawn.
            orb.count += candidate.count;
            orb.ageTicks = std::min(orb.ageTicks, candidate.ageTicks);
            candidate.count = 0;
        }
    }

    // playerTouch: contact pickup. An orb touched while its pickupDelay is
    // still counting down is left alone (mirrors takeXpDelay's per-orb gate,
    // simplified to XP-1's single-player case: vanilla's delay is on the
    // *player*, this build's is on the *orb*, which is equivalent while there
    // is exactly one player and gives the same "don't double-collect the same
    // orb in one tick" guarantee).
    if (playerAlive) {
        for (auto& orb : entities_) {
            if (orb.count <= 0 || orb.pickupDelayTicks > 0U) {
                continue;
            }
            const glm::vec3 delta = playerPosition - orb.position;
            // Contact = the orb's bounding box touches the player's — approximated
            // by the same near-zero pickup tolerance ItemEntitySystem uses,
            // widened slightly for the orb's larger 0.5-wide box.
            if (glm::dot(delta, delta) <= 0.6F * 0.6F) {
                const std::int32_t points = orb.value * orb.count;
                experience.addExperience(points);
                collectedPoints += points;
                orb.count = 0;
            }
        }
    }

    std::erase_if(entities_, [](const ExperienceOrb& orb) {
        return orb.count <= 0 || orb.ageTicks >= kLifetimeTicks || orb.position.y < -8.0F;
    });

    return collectedPoints;
}

} // namespace mc::gameplay
