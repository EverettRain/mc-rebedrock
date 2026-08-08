#include "gameplay/EntitySystem.hpp"

#include "world/Block.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace mc::gameplay {
namespace {

constexpr float kGravity = 0.04F;      // matches the item-entity fall rate
constexpr float kGroundOffset = 0.001F;
constexpr float kTwoPi = 6.28318530718F;
constexpr float kDespawnBelowY = -64.0F;
constexpr float kCollisionEpsilon = 0.0001F;
// Entity#maxUpStep for a walking mob: it climbs a single block without jumping.
constexpr float kStepHeight = 0.6F;
constexpr float kHorizontalDrag = 0.91F;
// Block#getSlipperiness for ordinary ground, the same value LivingEntity#travel
// folds into the drag while a creature is standing on something.
constexpr float kGroundSlipperiness = 0.6F;
// Under the grounded drag (0.91 * 0.6) a repeated acceleration settles at 2.2
// times itself per tick, so a species' blocks-per-tick wander speed divides down
// into the acceleration that actually produces it.
constexpr float kAccelerationToStepRatio = 2.2026F;
// LivingEntity#takeKnockback with the default 0.4 strength. The hurt,
// invulnerability and death windows now live in Damage.hpp, shared with the
// player.
constexpr float kKnockbackStrength = 0.4F;

// Small deterministic LCG (Numerical Recipes constants) so wander is
// reproducible without pulling in <random> or a shared global generator.
[[nodiscard]] std::uint32_t nextRandom(std::uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state;
}

// A value in [0, 1) from the top 24 bits (the low bits of an LCG are weak).
[[nodiscard]] float randomUnit(std::uint32_t& state) {
    return static_cast<float>(nextRandom(state) >> 8) / static_cast<float>(1U << 24);
}

[[nodiscard]] int floorToInt(float value) { return static_cast<int>(std::floor(value)); }

// Entity#pushAwayFrom, including its unusual normalisation: vanilla takes the
// larger of the two axis distances, square-roots it, and divides both by that.
[[nodiscard]] glm::vec3 pushBetween(glm::vec3 from, glm::vec3 to) {
    double deltaX = static_cast<double>(to.x) - static_cast<double>(from.x);
    double deltaZ = static_cast<double>(to.z) - static_cast<double>(from.z);
    const double largest = std::max(std::abs(deltaX), std::abs(deltaZ));
    if (largest < 0.01) {
        return glm::vec3{0.0F};
    }
    const double scale = std::sqrt(largest);
    deltaX /= scale;
    deltaZ /= scale;
    const double reduction = std::min(1.0 / scale, 1.0) * 0.05;
    return glm::vec3{static_cast<float>(deltaX * reduction), 0.0F,
                     static_cast<float>(deltaZ * reduction)};
}

[[nodiscard]] bool boxesOverlap(
    glm::vec3 firstMinimum,
    glm::vec3 firstMaximum,
    glm::vec3 secondMinimum,
    glm::vec3 secondMaximum) {
    return firstMinimum.x < secondMaximum.x && firstMaximum.x > secondMinimum.x &&
           firstMinimum.y < secondMaximum.y && firstMaximum.y > secondMinimum.y &&
           firstMinimum.z < secondMaximum.z && firstMaximum.z > secondMinimum.z;
}

// Slab method: where along the ray it enters the box, or nothing when it misses
// or the box is behind the origin. Mirrors Box#raycast.
[[nodiscard]] std::optional<float> rayBoxDistance(
    glm::vec3 origin,
    glm::vec3 direction,
    glm::vec3 minimum,
    glm::vec3 maximum) {
    float entry = 0.0F;
    float exit = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-6F) {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) {
                return std::nullopt;
            }
            continue;
        }
        const float inverse = 1.0F / direction[axis];
        float near = (minimum[axis] - origin[axis]) * inverse;
        float far = (maximum[axis] - origin[axis]) * inverse;
        if (near > far) {
            std::swap(near, far);
        }
        entry = std::max(entry, near);
        exit = std::min(exit, far);
        if (entry > exit) {
            return std::nullopt;
        }
    }
    return entry;
}

} // namespace

bool EntitySystem::boxIntersectsWorld(
    const world::World& world,
    glm::vec3 minimum,
    glm::vec3 maximum) {
    const int minX = floorToInt(minimum.x + kCollisionEpsilon);
    const int maxX = floorToInt(maximum.x - kCollisionEpsilon);
    const int minY = floorToInt(minimum.y + kCollisionEpsilon);
    const int maxY = floorToInt(maximum.y - kCollisionEpsilon);
    const int minZ = floorToInt(minimum.z + kCollisionEpsilon);
    const int maxZ = floorToInt(maximum.z - kCollisionEpsilon);
    for (int y = minY; y <= maxY; ++y) {
        if (y < 0 || y >= world::kWorldHeight) {
            continue;
        }
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

void EntitySystem::moveWithCollisions(
    const world::World& world,
    SimpleEntity& entity,
    glm::vec3 distance) {
    const auto box = entity.dimensions();
    const float half = box.width * 0.5F;
    const auto collidesAt = [&](glm::vec3 feet) {
        return boxIntersectsWorld(
            world,
            {feet.x - half, feet.y, feet.z - half},
            {feet.x + half, feet.y + box.height, feet.z + half});
    };
    // Vanilla resolves Y first, then the horizontal axes, and bisects toward the
    // contact so a creature ends up flush against the surface it hit.
    const auto moveAxis = [&](int axis, float amount) {
        if (std::abs(amount) <= kCollisionEpsilon) {
            return false;
        }
        glm::vec3 candidate = entity.position;
        candidate[axis] += amount;
        if (!collidesAt(candidate)) {
            entity.position = candidate;
            return false;
        }
        float safe = 0.0F;
        float blocked = 1.0F;
        for (int iteration = 0; iteration < 12; ++iteration) {
            const float middle = (safe + blocked) * 0.5F;
            candidate = entity.position;
            candidate[axis] += amount * middle;
            if (collidesAt(candidate)) {
                blocked = middle;
            } else {
                safe = middle;
            }
        }
        entity.position[axis] += amount * safe;
        entity.velocity[axis] = 0.0F;
        return true;
    };

    entity.onGround = false;
    if (moveAxis(1, distance.y) && distance.y < 0.0F) {
        entity.onGround = true;
    }
    const glm::vec3 beforeHorizontal = entity.position;
    const bool blockedX = moveAxis(0, distance.x);
    const bool blockedZ = moveAxis(2, distance.z);
    if ((blockedX || blockedZ) && entity.onGround) {
        // Entity#maxUpStep: retry the horizontal move from a step up, and keep
        // it only if the creature both clears the obstacle and lands again.
        // The 0.6 vanilla step clears stairs and partial blocks; a full
        // one-block rise has to lift the body to the top of the block (vanilla
        // mobs hop those via their navigation, but the same clear-and-land
        // test settles it either way). The first candidate that clears both
        // the lift and the horizontal move wins, so low obstacles still take
        // the shallow step.
        constexpr float kLiftCandidates[] = {kStepHeight, 1.0F};
        for (const float lift : kLiftCandidates) {
            SimpleEntity stepped = entity;
            stepped.position = beforeHorizontal;
            stepped.position.y += lift;
            if (collidesAt(stepped.position)) {
                continue;
            }
            glm::vec3 target = stepped.position;
            target.x += distance.x;
            target.z += distance.z;
            if (collidesAt(target)) {
                continue;
            }
            const float dropped =
                std::floor(target.y) + kGroundOffset >= beforeHorizontal.y
                    ? std::floor(target.y) + kGroundOffset
                    : target.y;
            target.y = collidesAt({target.x, dropped, target.z}) ? target.y : dropped;
            entity.position = target;
            entity.onGround = true;
            break;
        }
    }
}

void EntitySystem::spawn(glm::vec3 position, const entities::EntityType& type, std::uint32_t seed) {
    SimpleEntity entity;
    entity.type = &type;
    entity.position = position;
    entity.previousPosition = position;
    entity.damage.health = entity.damage.maxHealth = type.attributes().maxHealth;
    entity.rngState =
        seed != 0U ? seed : (0x9E3779B9U ^ (static_cast<std::uint32_t>(entities_.size()) + 1U));
    entity.yaw = randomUnit(entity.rngState) * kTwoPi;
    entity.previousYaw = entity.yaw;
    // MobEntity#initGoals runs once at spawn.
    type.ai().onSpawn(entity, entity.rngState);
    entities_.push_back(entity);
}

std::optional<EntityRayHit> EntitySystem::raycast(
    glm::vec3 origin,
    glm::vec3 direction,
    float reach) const {
    const float lengthSquared = glm::dot(direction, direction);
    if (lengthSquared < 1e-9F) {
        return std::nullopt;
    }
    const glm::vec3 unit = direction / std::sqrt(lengthSquared);
    std::optional<EntityRayHit> nearest;
    for (std::size_t index = 0; index < entities_.size(); ++index) {
        const auto& entity = entities_[index];
        // Skip only creatures that are no longer in the world (despawned or
        // finished their death animation). A body mid-death still has its
        // collision box and keeps blocking the ray, exactly like vanilla's
        // still-present (but dying) entity stays pickable until removed —
        // otherwise a one-hit kill in creative would let the dig reach the
        // block behind the corpse. Whether the creature can take damage is a
        // separate question, answered by hurt().
        if (entity.damage.deathTicks >= kDeathTicks || entity.position.y < kDespawnBelowY) {
            continue;
        }
        // Vanilla inflates the target box slightly (Entity#getTargetingMargin is
        // zero for a pig, but the pick box still grows by 0.3 on each side).
        constexpr float margin = 0.3F;
        const auto hit = rayBoxDistance(
            origin, unit, entity.boundingBoxMinimum() - glm::vec3{margin},
            entity.boundingBoxMaximum() + glm::vec3{margin});
        if (!hit.has_value() || *hit > reach) {
            continue;
        }
        if (!nearest.has_value() || *hit < nearest->distance) {
            nearest = EntityRayHit{index, *hit};
        }
    }
    return nearest;
}

bool EntitySystem::hurt(std::size_t index, float amount, glm::vec3 knockbackOrigin) {
    if (index >= entities_.size()) {
        return false;
    }
    auto& entity = entities_[index];
    // The guards and the invulnerability window live in the shared pipeline, so
    // the player and every mob resolve a hit the same way.
    const DamageOutcome outcome =
        applyDamage(entity.damage, DamageSource::EntityAttack, amount);
    if (!outcome.landed) {
        return false;
    }
    // Angerable#setTarget: a neutral species turns on its attacker here; passive
    // and hostile mobs override nothing and ignore this.
    entity.kind().ai().onAttacked(entity, entity.rngState);

    // LivingEntity#takeKnockback: horizontal shove away from the attacker plus a
    // fixed lift, with the existing velocity halved first.
    const glm::vec3 away = pushBetween(knockbackOrigin, entity.position);
    const float lengthSquared = away.x * away.x + away.z * away.z;
    if (lengthSquared > 1e-9F) {
        const float inverse = kKnockbackStrength / std::sqrt(lengthSquared);
        entity.velocity.x = entity.velocity.x * 0.5F + away.x * inverse;
        entity.velocity.z = entity.velocity.z * 0.5F + away.z * inverse;
    }
    entity.velocity.y = entity.velocity.y * 0.5F + 0.4F;
    entity.velocity.y = std::min(entity.velocity.y, 0.4F);
    // A hit creature bolts: re-pick a heading away from the attacker.
    entity.moving = true;
    entity.yaw = std::atan2(away.x, away.z);
    entity.wanderTimer = 40U;

    pendingSounds_.emplace_back(entity.position, outcome.died);
    if (outcome.died) {
        die(entity);
    }
    return true;
}

bool EntitySystem::kill(std::size_t index) {
    if (index >= entities_.size()) {
        return false;
    }
    auto& entity = entities_[index];
    // Entity#kill / LivingEntity#kill: OutOfWorld damage at infinite magnitude,
    // the same path /kill routes a player through.
    const DamageOutcome outcome = mc::gameplay::kill(entity.damage);
    if (outcome.died) {
        // The shared pipeline never plays a sound; raise the death one here,
        // the way a fatal hurt() reports it through pendingSounds_.
        pendingSounds_.emplace_back(entity.position, true);
        die(entity);
    }
    return outcome.landed;
}

bool EntitySystem::die(SimpleEntity& entity) {
    // LivingEntity#onDeath: the beginDeath guard mirrors the `dead` field that
    // keeps onDeath from running twice; loot leaves on this same tick, not when
    // the corpse's twenty-tick animation completes.
    if (!beginDeath(entity.damage)) {
        return false;
    }
    pendingDrops_.emplace_back(entity.position + glm::vec3{0.0F, 0.25F, 0.0F},
                               entity.kind().rollLoot(lootRandomState_));
    return true;
}

EntityTickResult EntitySystem::tick(
    const world::World& world,
    glm::vec3 pusher,
    float pusherWidth,
    float pusherHeight,
    Difficulty difficulty) {
    EntityTickResult result;
    for (auto& entity : entities_) {
        entity.previousPosition = entity.position;
        entity.previousYaw = entity.yaw;
        entity.previousWalkDistance = entity.walkDistance;
        ++entity.ageTicks;
        if (entity.damage.invulnerableTicks > 0) {
            --entity.damage.invulnerableTicks;
        }
        if (entity.damage.hurtTicks > 0) {
            --entity.damage.hurtTicks;
        }
        // Angerable#tickAngerLogic: a provoked neutral mob cools off over time.
        if (entity.angerTicks > 0) {
            --entity.angerTicks;
        }

        if (entity.damage.dead()) {
            // LivingEntity#updatePostDeath: the corpse tips over for twenty
            // ticks and is then removed; its loot already left on the tick
            // health crossed zero (see die()).
            static_cast<void>(advanceDeath(entity.damage));
            entity.velocity.x = 0.0F;
            entity.velocity.z = 0.0F;
        } else if (entity.wanderTimer == 0U) {
            // Every so often the species' AI re-picks a heading or pauses and
            // schedules the next decision (WanderAroundGoal). The behaviour is
            // read from the entity type, not branched on here.
            entity.kind().ai().chooseWanderIntent(entity, entity.rngState);
        }
        if (entity.wanderTimer > 0U) {
            --entity.wanderTimer;
        }

        // Horizontal intent comes from the wander heading, plus whatever
        // knockback is still decaying; vertical from gravity.
        // Vanilla samples the friction before moving and reuses it for the
        // end-of-tick drag, so a creature that just left the ground still pays
        // the ground value on that tick.
        const bool groundedBeforeMovement = entity.onGround;
        const float wanderSpeed = entity.kind().attributes().movementSpeed;
        if (!entity.dead() && entity.moving && groundedBeforeMovement) {
            const float acceleration = wanderSpeed / kAccelerationToStepRatio;
            entity.velocity.x += std::sin(entity.yaw) * acceleration;
            entity.velocity.z += std::cos(entity.yaw) * acceleration;
        }
        entity.velocity.y -= kGravity;

        moveWithCollisions(world, entity, entity.velocity);
        if (entity.onGround) {
            entity.velocity.y = 0.0F;
        }
        const float drag =
            groundedBeforeMovement ? kHorizontalDrag * kGroundSlipperiness : kHorizontalDrag;
        entity.velocity.x *= drag;
        entity.velocity.z *= drag;

        // Accumulate horizontal travel so the walk clip advances with real motion.
        const float dx = entity.position.x - entity.previousPosition.x;
        const float dz = entity.position.z - entity.previousPosition.z;
        entity.walkDistance += std::sqrt(dx * dx + dz * dz);

        // Entity#fall / #checkFallDistance, shared by every creature (the
        // player runs its own in PlayerVitals). The distance falls is
        // accumulated while airborne and resolved the landing tick:
        // LivingEntity#handleFallDamage costs ceil(fallDistance - 3) health,
        // applied through the shared invulnerability window but with no
        // knockback and no anger hook. Water cancels a fall, exactly like
        // vanilla's isTouchingWater check.
        const int footX = floorToInt(entity.position.x);
        const int footY = floorToInt(entity.position.y);
        const int footZ = floorToInt(entity.position.z);
        if (entity.damage.dead() ||
            world::isFluid(world.block(footX, footY, footZ)) ||
            world::isFluid(world.block(footX, footY + 1, footZ))) {
            entity.fallDistance = 0.0F;
        } else if (entity.onGround) {
            if (entity.fallDistance > 0.0F) {
                const float damage = std::ceil(entity.fallDistance - 3.0F);
                const DamageOutcome outcome =
                    applyDamage(entity.damage, DamageSource::Fall, damage);
                if (outcome.landed) {
                    pendingSounds_.emplace_back(entity.position, outcome.died);
                    if (outcome.died) {
                        die(entity);
                    }
                }
            }
            entity.fallDistance = 0.0F;
        } else {
            const float fallDelta = entity.position.y - entity.previousPosition.y;
            if (fallDelta < 0.0F) {
                entity.fallDistance -= fallDelta;
            }
        }
    }

    // Entity#pushAwayFrom, applied between every pair and against the player.
    // Both sides take the shove, which is what keeps a herd from stacking into
    // one column and lets the player nudge a pig out of a doorway.
    for (std::size_t first = 0; first < entities_.size(); ++first) {
        if (entities_[first].dead()) {
            continue;
        }
        for (std::size_t second = first + 1; second < entities_.size(); ++second) {
            if (entities_[second].dead()) {
                continue;
            }
            if (!boxesOverlap(entities_[first].boundingBoxMinimum(),
                              entities_[first].boundingBoxMaximum(),
                              entities_[second].boundingBoxMinimum(),
                              entities_[second].boundingBoxMaximum())) {
                continue;
            }
            const glm::vec3 push =
                pushBetween(entities_[first].position, entities_[second].position);
            entities_[first].velocity -= push;
            entities_[second].velocity += push;
        }
        const float pusherHalf = pusherWidth * 0.5F;
        if (boxesOverlap(entities_[first].boundingBoxMinimum(),
                         entities_[first].boundingBoxMaximum(),
                         {pusher.x - pusherHalf, pusher.y, pusher.z - pusherHalf},
                         {pusher.x + pusherHalf, pusher.y + pusherHeight,
                          pusher.z + pusherHalf})) {
            const glm::vec3 push = pushBetween(entities_[first].position, pusher);
            entities_[first].velocity -= push;
            result.playerPush += push;
        }
    }

    // MobEntity#checkDespawn: a Peaceful world clears out the mobs that are
    // disallowed there (the hostile MONSTER category) the same tick, silently and
    // without loot — the category decides, not a per-species check.
    const bool peaceful = difficulty == Difficulty::Peaceful;
    std::erase_if(entities_, [peaceful](const SimpleEntity& entity) {
        if (entity.position.y < kDespawnBelowY || entity.damage.deathTicks >= kDeathTicks) {
            return true;
        }
        return peaceful &&
               entities::mobCategoryTraits(entity.kind().category()).disallowedInPeaceful;
    });
    result.liveCount = entities_.size();
    return result;
}

} // namespace mc::gameplay
