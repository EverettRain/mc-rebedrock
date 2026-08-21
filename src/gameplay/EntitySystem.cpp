#include "gameplay/EntitySystem.hpp"

#include "gameplay/EntityRenderSnapshot.hpp"

#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <utility>

namespace mc::gameplay {
namespace {

// LivingEntity travel uses twice the gravity of an item entity, then applies
// vertical air drag after moving. Reusing the item's 0.04 made hit mobs float
// for almost twice as long as the player/vanilla trajectory.
constexpr float kGravity = 0.08F;
constexpr float kVerticalAirDrag = 0.98F;
constexpr float kGroundOffset = 0.001F;
// MobEntity#getMinAmbientSoundDelay: the base-class idle-sound interval. None
// of the built-in species overrides it, so every mob resets its ambient
// scheduler to -80 after an idle sound and it climbs one per tick from there.
constexpr int kMinAmbientSoundDelay = 80;
// The horizontal travel between playStepSound calls, roughly one block per
// footfall the way Entity#checkBlockCollision triggers steps on block edges.
constexpr float kMobStepStride = 1.0F;
constexpr float kTwoPi = 6.28318530718F;
constexpr float kDespawnBelowY = -64.0F;
constexpr float kCollisionEpsilon = 0.0001F;
// Entity#maxUpStep for a walking mob: it climbs a single block without jumping.
constexpr float kStepHeight = 0.6F;
constexpr float kHorizontalDrag = 0.91F;
// Block#getSlipperiness for ordinary ground, the same value LivingEntity#travel
// folds into the drag while a creature is standing on something.
constexpr float kGroundSlipperiness = 0.6F;
// This locomotion integrator historically expressed ordinary mob travel as
// one fifth of Java's MOVEMENT_SPEED attribute. Keep registered attributes in
// their canonical 26.1 form, but convert at this single integration boundary;
// otherwise every species becomes five times too fast. Goal modifiers are
// applied after the conversion, so PanicGoal still produces its intended
// acceleration over the normal walking speed.
constexpr float kMovementAttributeToInternalSpeed = 0.2F;
// Under the grounded drag (0.91 * 0.6) a repeated acceleration settles at 2.2
// times itself per tick, so the converted target speed divides down into the
// acceleration that produces it at steady state.
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

// Visits every 16-block section the segment [origin, origin + unit*reach]
// passes through, in ray order, via a standard 3D DDA grid walk. `visit` runs
// once per section. Reach is a few blocks here, so the traversal touches only a
// handful of sections — the linear raycast becomes a per-bucket probe.
template <typename Visit>
void walkRaySections(glm::vec3 origin, glm::vec3 unit, float reach, Visit&& visit) {
    constexpr float kInfinity = std::numeric_limits<float>::max();
    constexpr float kSize = static_cast<float>(world::kSectionSize);
    const auto cellOf = [](float value) {
        return static_cast<int>(std::floor(value / kSize));
    };
    int x = cellOf(origin.x);
    int y = cellOf(origin.y);
    int z = cellOf(origin.z);
    visit(EntitySection{x, y, z});
    if (reach <= 0.0F) {
        return;
    }
    const auto step = [](float dir) { return dir > 0.0F ? 1 : (dir < 0.0F ? -1 : 0); };
    const int sx = step(unit.x);
    const int sy = step(unit.y);
    const int sz = step(unit.z);
    // t (ray parameter, 0..reach) at which each axis crosses into the next cell.
    const auto boundaryT = [&](float originValue, float dirValue, int cell, int s) {
        if (s == 0) {
            return kInfinity;
        }
        const float edge =
            s > 0 ? static_cast<float>(cell + 1) * kSize : static_cast<float>(cell) * kSize;
        return (edge - originValue) / dirValue;
    };
    const auto cellT = [](float dir) {
        return std::abs(dir) < 1e-12F ? kInfinity : kSize / std::abs(dir);
    };
    float tMaxX = boundaryT(origin.x, unit.x, x, sx);
    float tMaxY = boundaryT(origin.y, unit.y, y, sy);
    float tMaxZ = boundaryT(origin.z, unit.z, z, sz);
    const float tDeltaX = cellT(unit.x);
    const float tDeltaY = cellT(unit.y);
    const float tDeltaZ = cellT(unit.z);
    for (int iteration = 0; iteration < 256; ++iteration) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            if (tMaxX > reach) {
                break;
            }
            x += sx;
            tMaxX += tDeltaX;
        } else if (tMaxY < tMaxZ) {
            if (tMaxY > reach) {
                break;
            }
            y += sy;
            tMaxY += tDeltaY;
        } else {
            if (tMaxZ > reach) {
                break;
            }
            z += sz;
            tMaxZ += tDeltaZ;
        }
        visit(EntitySection{x, y, z});
    }
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
        if (!world::isWorldYInRange(y)) {
            continue;
        }
        for (int z = minZ; z <= maxZ; ++z) {
            for (int x = minX; x <= maxX; ++x) {
                // A partial block only fills part of its cell, so a creature rests
                // on a slab and walks through its open half, and a fence post or
                // stair step blocks only its own boxes rather than a phantom full
                // cube. The cell iteration covers the horizontal overlap for a
                // full-footprint Column; Boxes are tested in 3D.
                if (world::shapeOverlaps(world::collisionShape(world.state(x, y, z)),
                                         static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(z), minimum.x, minimum.y, minimum.z,
                                         maximum.x, maximum.y, maximum.z)) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool EntitySystem::canOccupy(
    const world::World& world,
    glm::vec3 position,
    entities::EntityDimensions dimensions) {
    const float half = dimensions.width * 0.5F;
    return !boxIntersectsWorld(
        world,
        {position.x - half, position.y, position.z - half},
        {position.x + half, position.y + dimensions.height, position.z + half});
}

bool EntitySystem::intersectsBlock(int x, int y, int z, float boxBottom, float boxTop) const {
    if (boxTop <= boxBottom) {
        return false;
    }
    const glm::vec3 blockMinimum{
        static_cast<float>(x), static_cast<float>(y) + boxBottom, static_cast<float>(z)};
    const glm::vec3 blockMaximum{
        static_cast<float>(x) + 1.0F, static_cast<float>(y) + boxTop, static_cast<float>(z) + 1.0F};
    for (const auto& entity : entities_) {
        if (entity.damage.deathTicks >= kDeathTicks || entity.position.y < kDespawnBelowY) {
            continue;
        }
        if (boxesOverlap(entity.boundingBoxMinimum(), entity.boundingBoxMaximum(),
                         blockMinimum, blockMaximum)) {
            return true;
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
    // A spawn, restored save, or block placed around a creature can leave the
    // starting box already intersecting terrain. The axis solver below assumes
    // its zero-distance endpoint is safe, so recover to the closest free face
    // first; otherwise every small AI move bisects back to the same trapped
    // position forever.
    if (collidesAt(entity.position)) {
        const glm::vec3 minimum = entity.boundingBoxMinimum();
        const glm::vec3 maximum = entity.boundingBoxMaximum();
        const int minX = floorToInt(minimum.x + kCollisionEpsilon);
        const int maxX = floorToInt(maximum.x - kCollisionEpsilon);
        const int minY = floorToInt(minimum.y + kCollisionEpsilon);
        const int maxY = floorToInt(maximum.y - kCollisionEpsilon);
        const int minZ = floorToInt(minimum.z + kCollisionEpsilon);
        const int maxZ = floorToInt(maximum.z - kCollisionEpsilon);
        std::optional<glm::vec3> nearestFree;
        float nearestDistanceSquared = std::numeric_limits<float>::max();
        constexpr float separation = kCollisionEpsilon * 2.0F;
        const auto consider = [&](glm::vec3 candidate) {
            if (collidesAt(candidate)) {
                return;
            }
            const glm::vec3 offset = candidate - entity.position;
            const float distanceSquared = glm::dot(offset, offset);
            if (distanceSquared < nearestDistanceSquared) {
                nearestDistanceSquared = distanceSquared;
                nearestFree = candidate;
            }
        };
        for (int y = minY; y <= maxY; ++y) {
            if (!world::isWorldYInRange(y)) {
                continue;
            }
            for (int z = minZ; z <= maxZ; ++z) {
                for (int x = minX; x <= maxX; ++x) {
                    if (!world::hasCollision(world.block(x, y, z))) {
                        continue;
                    }
                    glm::vec3 candidate = entity.position;
                    candidate.x = static_cast<float>(x) - half - separation;
                    consider(candidate);
                    candidate.x = static_cast<float>(x + 1) + half + separation;
                    consider(candidate);
                    candidate = entity.position;
                    candidate.z = static_cast<float>(z) - half - separation;
                    consider(candidate);
                    candidate.z = static_cast<float>(z + 1) + half + separation;
                    consider(candidate);
                    candidate = entity.position;
                    candidate.y = static_cast<float>(y + 1) + separation;
                    consider(candidate);
                }
            }
        }
        if (nearestFree.has_value()) {
            entity.position = *nearestFree;
            entity.previousPosition = entity.position;
        }
    }
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
            glm::vec3 steppedPosition = beforeHorizontal;
            steppedPosition.y += lift;
            if (collidesAt(steppedPosition)) {
                continue;
            }
            glm::vec3 target = steppedPosition;
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
    entity.id = nextEntityId_++;
    entity.position = position;
    entity.previousPosition = position;
    entity.damage.health = entity.damage.maxHealth = type.attributes().maxHealth();
    entity.rngState =
        seed != 0U ? seed : (0x9E3779B9U ^ (static_cast<std::uint32_t>(entities_.size()) + 1U));
    entity.yaw = randomUnit(entity.rngState) * kTwoPi;
    entity.previousYaw = entity.yaw;
    entity.lookYaw = entity.yaw;
    // MobEntity#initGoals runs once at spawn.
    type.ai().configureBrain(entity.brain);
    type.ai().onSpawn(entity, entity.rngState);
    entities_.push_back(std::move(entity));
    // Register the stable id and drop the creature into its section bucket so
    // between-tick raycasts and the next tick's push see it immediately.
    const std::size_t index = entities_.size() - 1U;
    idToIndex_[entities_[index].id] = index;
    const EntitySection section = entitySectionOf(position);
    entitySections_.push_back(section);
    sections_[section].push_back(index);
}

std::uint64_t EntitySystem::restore(glm::vec3 position, const entities::EntityType& type,
                                    float yaw, glm::vec3 velocity, float health,
                                    int angerTicks, unsigned int ageTicks,
                                    std::uint32_t rngState) {
    SimpleEntity entity;
    entity.type = &type;
    entity.id = nextEntityId_++;
    entity.position = position;
    entity.previousPosition = position;
    entity.velocity = velocity;
    entity.yaw = yaw;
    entity.previousYaw = yaw;
    entity.lookYaw = yaw;
    entity.ageTicks = ageTicks;
    entity.angerTicks = angerTicks;
    entity.rngState = rngState;
    // The species owns the max; the save's health is the current value, clamped
    // so a corrupt record cannot restore a creature over its cap.
    entity.damage.maxHealth = type.attributes().maxHealth();
    entity.damage.health = std::min(health, entity.damage.maxHealth);
    // MobEntity#initGoals runs once at spawn, exactly like a fresh spawn.
    type.ai().configureBrain(entity.brain);
    type.ai().onSpawn(entity, entity.rngState);
    entities_.push_back(std::move(entity));
    const std::size_t index = entities_.size() - 1U;
    idToIndex_[entities_[index].id] = index;
    const EntitySection section = entitySectionOf(position);
    entitySections_.push_back(section);
    sections_[section].push_back(index);
    return entities_[index].id;
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
    std::vector<std::uint8_t> tested(entities_.size(), 0U);
    // An entity is indexed by its feet/centre section, but its AABB can cross a
    // section face. Probe the immediate neighbours of every ray section so a
    // ray through (for example) a cow's upper body in section Y+1 still sees
    // the cow whose feet are bucketed in section Y. `tested` keeps an entity
    // spanning several visited neighbourhoods from being evaluated repeatedly.
    const auto testSection = [&](EntitySection section) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto found = sections_.find(
                        EntitySection{section.chunkX + dx, section.sectionY + dy,
                                      section.chunkZ + dz});
                    if (found == sections_.end()) {
                        continue;
                    }
                    for (const std::size_t index : found->second) {
                        if (index >= entities_.size() || tested[index] != 0U) {
                            continue;
                        }
                        tested[index] = 1U;
                        const auto& entity = entities_[index];
                        // Skip only creatures that are no longer in the world
                        // (despawned or finished their death animation). A body
                        // mid-death remains pickable until removal.
                        if (entity.damage.deathTicks >= kDeathTicks ||
                            entity.position.y < kDespawnBelowY) {
                            continue;
                        }
                        const auto hit = rayBoxDistance(
                            origin, unit, entity.boundingBoxMinimum(),
                            entity.boundingBoxMaximum());
                        if (!hit.has_value() || *hit > reach) {
                            continue;
                        }
                        if (!nearest.has_value() || *hit < nearest->distance) {
                            nearest = EntityRayHit{entity.id, *hit};
                        }
                    }
                }
            }
        }
    };
    walkRaySections(origin, unit, reach, testSection);
    return nearest;
}

SimpleEntity* EntitySystem::byId(std::uint64_t id) {
    const auto found = idToIndex_.find(id);
    return found == idToIndex_.end() ? nullptr : &entities_[found->second];
}

const SimpleEntity* EntitySystem::byId(std::uint64_t id) const {
    const auto found = idToIndex_.find(id);
    return found == idToIndex_.end() ? nullptr : &entities_[found->second];
}

bool EntitySystem::hurt(std::uint64_t entityId, float amount, glm::vec3 knockbackOrigin,
                        ActorReference attacker) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    auto& entity = entities_[found->second];
    // The guards and the invulnerability window live in the shared pipeline, so
    // the player and every mob resolve a hit the same way.
    const DamageOutcome outcome =
        applyDamage(entity.damage, DamageType::EntityAttack, amount);
    if (!outcome.landed) {
        return false;
    }
    entity.lastAttacker = attacker;
    entity.lastAttackerPosition = knockbackOrigin;
    entity.recentAttackerTicks = 100;
    ++entity.lastHurtSequence;
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
    // Vanilla only supplies the vertical lift while the target is grounded.
    // An accepted follow-up hit in mid-air still refreshes horizontal
    // knockback, but cannot reset the target to another full jump arc.
    if (entity.onGround) {
        entity.velocity.y = std::min(entity.velocity.y * 0.5F + kKnockbackStrength,
                                     kKnockbackStrength);
    }
    // playHurtSound's resetSoundDelay: a landed hit pushes the next idle sound
    // back, so a mob being mauled stops its ambient chatter mid-fight.
    entity.ambientSoundChance = -kMinAmbientSoundDelay;
    pendingSounds_.push_back({entity.position,
                              outcome.died ? MobSoundEvent::Death : MobSoundEvent::Hurt,
                              entity.type});
    if (outcome.died) {
        die(entity);
    }
    return true;
}

bool EntitySystem::kill(std::uint64_t entityId) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    auto& entity = entities_[found->second];
    // Entity#kill / LivingEntity#kill: OutOfWorld damage at infinite magnitude,
    // the same path /kill routes a player through.
    const DamageOutcome outcome = mc::gameplay::kill(entity.damage);
    if (outcome.died) {
        // The shared pipeline never plays a sound; raise the death one here,
        // the way a fatal hurt() reports it through pendingSounds_.
        pendingSounds_.push_back({entity.position, MobSoundEvent::Death, entity.type});
        die(entity);
    }
    return outcome.landed;
}

void EntitySystem::clear() {
    entities_.clear();
    entitySections_.clear();
    idToIndex_.clear();
    sections_.clear();
    nextEntityId_ = 1U;
    gameTick_ = 0U;
}

std::vector<SimpleEntity> EntitySystem::removeInChunk(int chunkX, int chunkZ) {
    // Swap-and-pop so removal is O(removed) rather than shifting the vector;
    // rebuildSpatialIndex reconciles the id and section indexes afterwards.
    // Positions are the feet the persistence layer saved, so the same floor
    // division buckets them.
    const auto floorDiv = [](int value, int divisor) {
        return value >= 0 ? value / divisor : -(((-value) + divisor - 1) / divisor);
    };
    const auto inChunk = [&](const SimpleEntity& entity) {
        return floorDiv(static_cast<int>(std::floor(entity.position.x)), world::kChunkWidth) ==
                   chunkX &&
               floorDiv(static_cast<int>(std::floor(entity.position.z)), world::kChunkDepth) ==
                   chunkZ;
    };
    std::vector<SimpleEntity> removed;
    for (std::size_t index = 0; index < entities_.size();) {
        if (!inChunk(entities_[index])) {
            ++index;
            continue;
        }
        removed.push_back(std::move(entities_[index]));
        entities_[index] = std::move(entities_.back());
        entities_.pop_back();
        // The element swapped in must be examined too.
    }
    if (!removed.empty()) {
        rebuildSpatialIndex();
    }
    return removed;
}

void EntitySystem::rebuildSpatialIndex() {
    idToIndex_.clear();
    sections_.clear();
    idToIndex_.reserve(entities_.size());
    entitySections_.clear();
    entitySections_.reserve(entities_.size());
    for (std::size_t index = 0; index < entities_.size(); ++index) {
        const auto& entity = entities_[index];
        idToIndex_[entity.id] = index;
        const EntitySection section = entitySectionOf(entity.position);
        entitySections_.push_back(section);
        sections_[section].push_back(index);
    }
}

void EntitySystem::updateSectionMembership(std::size_t index) {
    const EntitySection next = entitySectionOf(entities_[index].position);
    const EntitySection previous = entitySections_[index];
    if (next == previous) return;
    auto previousBucket = sections_.find(previous);
    if (previousBucket != sections_.end()) {
        auto& indices = previousBucket->second;
        const auto found = std::ranges::find(indices, index);
        if (found != indices.end()) {
            *found = indices.back();
            indices.pop_back();
        }
        if (indices.empty()) sections_.erase(previousBucket);
    }
    sections_[next].push_back(index);
    entitySections_[index] = next;
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
    Difficulty difficulty,
    bool playerAlive,
    bool playerCreative,
    float simulationRadius) {
    EntityTickResult result;
    ++gameTick_;
    const bool playerPresent = pusher.y > -900.0F;
    // ServerWorld's tick distance: a creature beyond the radius is frozen this
    // tick (dormant but rendered). No radius means nothing is gated.
    const float radiusSquared = playerPresent && simulationRadius > 0.0F
        ? simulationRadius * simulationRadius
        : std::numeric_limits<float>::max();
    const auto isFrozen = [&](const SimpleEntity& entity) {
        if (!playerPresent) {
            return false;
        }
        const float dx = entity.position.x - pusher.x;
        const float dz = entity.position.z - pusher.z;
        return dx * dx + dz * dz > radiusSquared;
    };
    MobAiContext aiContext{
        world,
        std::span<const SimpleEntity>{entities_},
        idToIndex_,
        PlayerAiView{pusher, playerPresent, playerAlive, playerCreative, pusherWidth, pusherHeight},
        gameTick_};
    for (auto& entity : entities_) {
        entity.previousPosition = entity.position;
        entity.previousYaw = entity.yaw;
        entity.previousWalkDistance = entity.walkDistance;
        if (isFrozen(entity)) {
            // Frozen but mid-death: the corpse still completes its animation so
            // it is eventually removed; everything else stays dormant.
            if (entity.damage.dead()) {
                static_cast<void>(advanceDeath(entity.damage));
                entity.velocity.x = 0.0F;
                entity.velocity.z = 0.0F;
            }
            continue;
        }
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
        if (entity.recentAttackerTicks > 0) {
            --entity.recentAttackerTicks;
        }
        if (entity.wanderTimer > 0U) {
            --entity.wanderTimer;
        }
        entity.movementSpeedMultiplier = 1.0F;

        // MobEntity#baseTick's ambient scheduler: every tick it rolls
        // nextInt(1000) against a counter that climbs one per tick and snaps
        // back to -getMinAmbientSoundDelay() (80) after an idle sound, so a
        // species barks roughly every four seconds. The roll is strictly less
        // than the pre-increment counter, so a freshly reset (negative) counter
        // can never fire — exactly like vanilla's nextInt(1000) < counter++.
        if (!entity.damage.dead() &&
            static_cast<int>(nextRandom(entity.rngState) % 1000U) <
                entity.ambientSoundChance++) {
            entity.ambientSoundChance = -kMinAmbientSoundDelay;
            pendingSounds_.push_back({entity.position, MobSoundEvent::Ambient, entity.type});
        }

        if (entity.damage.dead()) {
            // LivingEntity#updatePostDeath: the corpse tips over for twenty
            // ticks and is then removed; its loot already left on the tick
            // health crossed zero (see die()).
            static_cast<void>(advanceDeath(entity.damage));
            entity.brain.stop(entity, aiContext);
            entity.velocity.x = 0.0F;
            entity.velocity.z = 0.0F;
        } else {
            entity.kind().ai().tick(entity, entity.rngState);
            entity.brain.tick(entity, aiContext);
            if (const auto attack = entity.brain.takeAttackRequest()) {
                result.mobAttacks.push_back({entity.id, attack->target, attack->amount});
            }
        }

        // Horizontal intent comes from the wander heading, plus whatever
        // knockback is still decaying; vertical from gravity.
        // Vanilla samples the friction before moving and reuses it for the
        // end-of-tick drag, so a creature that just left the ground still pays
        // the ground value on that tick.
        const bool groundedBeforeMovement = entity.onGround;
        const float wanderSpeed = entity.kind().attributes().movementSpeed() *
                                  kMovementAttributeToInternalSpeed *
                                  entity.movementSpeedMultiplier;
        if (!entity.dead() && entity.moving && groundedBeforeMovement) {
            const float acceleration = wanderSpeed / kAccelerationToStepRatio;
            entity.velocity.x += std::sin(entity.yaw) * acceleration;
            entity.velocity.z += std::cos(entity.yaw) * acceleration;
        }
        // LivingEntity#travel moves with the current velocity first. Gravity
        // and vertical drag prepare the velocity for the next tick; applying
        // gravity before movement shortens the first step but, together with
        // the old item gravity, greatly lengthened the whole knockback arc.
        moveWithCollisions(world, entity, entity.velocity);
        entity.velocity.y = (entity.velocity.y - kGravity) * kVerticalAirDrag;
        const float drag =
            groundedBeforeMovement ? kHorizontalDrag * kGroundSlipperiness : kHorizontalDrag;
        entity.velocity.x *= drag;
        entity.velocity.z *= drag;

        // Accumulate horizontal travel so the walk clip advances with real motion.
        const float dx = entity.position.x - entity.previousPosition.x;
        const float dz = entity.position.z - entity.previousPosition.z;
        const float stepDelta = std::sqrt(dx * dx + dz * dz);
        entity.walkDistance += stepDelta;

        // MobEntity#playStepSound: while a creature actually walks it steps once
        // per block of horizontal travel at its step volume (0.15). Airborne or
        // dead bodies accumulate nothing, so a knocked-back mob does not scuffle.
        if (!entity.damage.dead() && entity.onGround && stepDelta > 0.0F) {
            entity.stepAccumulator += stepDelta;
            if (entity.stepAccumulator >= kMobStepStride) {
                entity.stepAccumulator = std::fmod(entity.stepAccumulator, kMobStepStride);
                pendingSounds_.push_back({entity.position, MobSoundEvent::Step, entity.type});
            }
        } else {
            entity.stepAccumulator = 0.0F;
        }

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
                    applyDamage(entity.damage, DamageType::Fall, damage);
                if (outcome.landed) {
                    entity.ambientSoundChance = -kMinAmbientSoundDelay;
                    pendingSounds_.push_back(
                        {entity.position,
                         outcome.died ? MobSoundEvent::Death : MobSoundEvent::Hurt,
                         entity.type});
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

    // Maintain persistent buckets like vanilla's EntitySectionStorage. Most
    // creatures remain in the same 16-block section, so the common case is only
    // one integer section comparison and performs no container mutation.
    for (std::size_t index = 0; index < entities_.size(); ++index) {
        updateSectionMembership(index);
    }
    std::size_t testedPairs = 0U;
    for (const auto& [section, indices] : sections_) {
        for (const std::size_t first : indices) {
            auto& firstEntity = entities_[first];
            if (firstEntity.dead() || isFrozen(firstEntity)) {
                continue;
            }
            // Every unordered pair is processed once, from the smaller index's
            // side (the index order is a total order over this tick's buckets),
            // so the symmetric shove is applied exactly once.
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        const auto neighbor = sections_.find(
                            EntitySection{section.chunkX + dx, section.sectionY + dy,
                                          section.chunkZ + dz});
                        if (neighbor == sections_.end()) {
                            continue;
                        }
                        for (const std::size_t second : neighbor->second) {
                            if (second <= first) {
                                continue;
                            }
                            auto& secondEntity = entities_[second];
                            if (secondEntity.dead() || isFrozen(secondEntity)) {
                                continue;
                            }
                            ++testedPairs;
                            if (!boxesOverlap(firstEntity.boundingBoxMinimum(),
                                              firstEntity.boundingBoxMaximum(),
                                              secondEntity.boundingBoxMinimum(),
                                              secondEntity.boundingBoxMaximum())) {
                                continue;
                            }
                            const glm::vec3 push =
                                pushBetween(firstEntity.position, secondEntity.position);
                            firstEntity.velocity -= push;
                            secondEntity.velocity += push;
                        }
                    }
                }
            }
        }
    }

    // The player's share, resolved through the pusher's section neighbourhood
    // rather than a sweep of every creature.
    const EntitySection pusherSection = entitySectionOf(pusher);
    const float pusherHalf = pusherWidth * 0.5F;
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                const auto neighbor = sections_.find(
                    EntitySection{pusherSection.chunkX + dx, pusherSection.sectionY + dy,
                                  pusherSection.chunkZ + dz});
                if (neighbor == sections_.end()) {
                    continue;
                }
                for (const std::size_t index : neighbor->second) {
                    auto& entity = entities_[index];
                    if (entity.dead() || isFrozen(entity)) {
                        continue;
                    }
                    if (boxesOverlap(entity.boundingBoxMinimum(),
                                     entity.boundingBoxMaximum(),
                                     {pusher.x - pusherHalf, pusher.y, pusher.z - pusherHalf},
                                     {pusher.x + pusherHalf, pusher.y + pusherHeight,
                                      pusher.z + pusherHalf})) {
                        const glm::vec3 push = pushBetween(entity.position, pusher);
                        entity.velocity -= push;
                        result.playerPush += push;
                    }
                }
            }
        }
    }

    // MobEntity#checkDespawn: a Peaceful world clears out the mobs that are
    // disallowed there (the hostile MONSTER category) the same tick, silently and
    // without loot — the category decides, not a per-species check.
    const bool peaceful = difficulty == Difficulty::Peaceful;
    // MobEntity#checkDespawn's despawn-range half: a distant-despawning category
    // (MONSTER/AMBIENT/WATER_CREATURE) that has spent the last 40 ticks past 128
    // blocks is silently removed; CREATURE (animals) stay forever. The 128-block
    // range is vanilla's fixed despawn distance, independent of the simulation
    // radius.
    constexpr int kDespawnThreshold = 40;
    for (auto& entity : entities_) {
        if (!playerPresent ||
            !entities::mobCategoryTraits(entity.kind().category()).despawnsWhenDistant ||
            entity.position.y < kDespawnBelowY) {
            continue;
        }
        const float dx = entity.position.x - pusher.x;
        const float dz = entity.position.z - pusher.z;
        if (dx * dx + dz * dz > 128.0F * 128.0F) {
            ++entity.despawnTicks;
        } else {
            entity.despawnTicks = 0;
        }
    }
    const std::size_t sizeBeforeRemoval = entities_.size();
    std::erase_if(entities_, [&, peaceful](const SimpleEntity& entity) {
        if (entity.position.y < kDespawnBelowY || entity.damage.deathTicks >= kDeathTicks) {
            return true;
        }
        if (peaceful &&
            entities::mobCategoryTraits(entity.kind().category()).disallowedInPeaceful) {
            return true;
        }
        return playerPresent && entity.despawnTicks >= kDespawnThreshold;
    });
    // Compaction shifts vector slots, so rebuild only when something was
    // actually removed. A normal tick now performs no global index rebuild.
    if (entities_.size() != sizeBeforeRemoval) rebuildSpatialIndex();
    result.liveCount = entities_.size();

    // One-time diagnostic: confirms the spatial hash turned the O(n²) push into
    // O(neighbours). At herd scale candidates stay far below the live² the old
    // full sweep would have tested; it never fires in normal gameplay.
    static bool s_pushDiagnosticPrinted = false;
    if (!s_pushDiagnosticPrinted && result.liveCount >= 64U) {
        s_pushDiagnosticPrinted = true;
        std::cerr << "[entity] push candidates=" << testedPairs << " live=" << result.liveCount
                  << '\n';
    }
    return result;
}

std::optional<EntityRayHit> raycastSnapshotEntities(
    const EntityRenderSnapshot& snapshot,
    glm::vec3 origin,
    glm::vec3 direction,
    float reach) {
    const float lengthSquared = glm::dot(direction, direction);
    if (lengthSquared < 1e-9F) {
        return std::nullopt;
    }
    const glm::vec3 unit = direction / std::sqrt(lengthSquared);
    std::optional<EntityRayHit> nearest;
    for (const auto& entity : snapshot.entities()) {
        if (entity.type == nullptr || entity.deathTicks >= kDeathTicks) {
            continue;
        }
        const auto dimensions = entity.type->dimensions();
        const float half = dimensions.width * 0.5F;
        const glm::vec3 minimum{entity.position.x - half, entity.position.y,
                                entity.position.z - half};
        const glm::vec3 maximum{entity.position.x + half, entity.position.y + dimensions.height,
                                entity.position.z + half};
        const auto hit = rayBoxDistance(origin, unit, minimum, maximum);
        if (!hit.has_value() || *hit > reach) {
            continue;
        }
        if (!nearest.has_value() || *hit < nearest->distance) {
            nearest = EntityRayHit{entity.id, *hit};
        }
    }
    return nearest;
}

} // namespace mc::gameplay
