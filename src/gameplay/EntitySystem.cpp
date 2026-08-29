#include "gameplay/EntitySystem.hpp"

#include "gameplay/EnchantmentCombat.hpp"
#include "gameplay/EntityRenderSnapshot.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/Random.hpp"

#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
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
// The void line an entity is cleared below — vanilla's minY-64, shared with the
// dropped-item/orb/falling-block despawn so every "fell out of the world" test
// agrees (see world::kVoidDespawnY). Not kMinY itself: bedrock sits at kMinY, so
// the clear line must be below the world, leaving the same 64-block void buffer
// vanilla gives.
constexpr float kDespawnBelowY = world::kVoidDespawnY;
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

// Wander/AI draws run off the shared deterministic mc::rng (Java's
// LegacyRandomSource core), advancing the entity's own 48-bit state — no
// <random>, no shared global generator. A value in [0, 1) is nextFloat.
[[nodiscard]] float randomUnit(std::uint64_t& state) { return mc::rng::nextFloat(state); }

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

void EntitySystem::installBreedingGoals(SimpleEntity& entity) {
    const entities::EntityType& type = *entity.type;
    if (!type.breedable()) {
        return;
    }
    // AnimalMateGoal (2) outranks temptation (3), which outranks the wander/look
    // fallback AnimalAi installs at 5+, matching the vanilla Animal goal order.
    const entities::BreedingProfile& breeding = type.breeding();
    entity.brain.goals().add(2, std::make_unique<entities::AnimalMateGoal>(1.0F));
    entity.brain.goals().add(
        3, std::make_unique<entities::TemptGoal>(breeding.temptItem, 1.25F, /*range=*/10.0F));
    // FollowParent sits above the wander/look fallback (5+) so a baby trailing the
    // herd is not constantly interrupted by a wander pick of equal priority.
    entity.brain.goals().add(4, std::make_unique<entities::FollowParentGoal>(1.1F));
}

void EntitySystem::spawn(glm::vec3 position, const entities::EntityType& type, std::uint64_t seed) {
    SimpleEntity entity;
    entity.type = &type;
    entity.id = nextEntityId_++;
    entity.position = position;
    entity.previousPosition = position;
    entity.damage.health = entity.damage.maxHealth = type.attributes().maxHealth();
    // Scramble the semantic seed into a 48-bit LegacyRandomSource state the way
    // java.util.Random(seed) does, so the stream is well-mixed from the first
    // draw instead of starting on a raw (and low-entropy) value.
    const std::uint64_t rawSeed =
        seed != 0U ? seed : (0x9E3779B9ULL ^ (static_cast<std::uint64_t>(entities_.size()) + 1U));
    entity.rngState = mc::rng::seedFromValue(rawSeed);
    entity.yaw = randomUnit(entity.rngState) * kTwoPi;
    entity.previousYaw = entity.yaw;
    entity.lookYaw = entity.yaw;
    // AR-A4: a laysEggs() species gets its first countdown rolled off the same
    // stream its yaw/AI draw from, so a fresh spawn's first lay tick is
    // reproducible per seed exactly like every other timer here. A
    // non-laying species leaves this at zero, never observed (the tick-site
    // egg scheduler is itself gated on laysEggs()).
    if (type.laysEggs()) {
        entity.eggLayTimer =
            kEggLayBaseTicks +
            static_cast<int>(mc::rng::nextInt(entity.rngState,
                                              static_cast<std::uint32_t>(kEggLayRandomTicks)));
    }
    // MobEntity#initGoals runs once at spawn.
    type.ai().configureBrain(entity.brain);
    installBreedingGoals(entity);
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
                                    std::uint64_t rngState, int fireTicks,
                                    const ActiveEffects& effects, int age, int loveTicks,
                                    DyeColor color) {
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
    // A fire-immune species can never be ablaze; drop a stray record on restore.
    entity.fireTicks = type.fireImmune() ? 0 : std::max(fireTicks, 0);
    // The active MobEffects travel with the save (a poisoned creature reopens
    // still poisoned with its remaining duration intact).
    entity.effects = effects;
    // AgeableMob age/love travel with the save: a baby reopens a baby with its
    // remaining growth, an adult keeps its breed cooldown, love survives. A
    // non-breedable species is forced to adult, so a stray record cannot leave it
    // a permanent baby with no way to grow up.
    entity.age = type.breedable() ? age : 0;
    entity.loveTicks = type.breedable() ? std::max(loveTicks, 0) : 0;
    // DYE-0: the dye colour travels with the save (a dyed sheep reopens the
    // colour it was dyed). A coloured-species field on every creature; a species
    // with no colour semantics simply idles at the restored default white.
    entity.color = color;
    // The species owns the max; the save's health is the current value, clamped
    // so a corrupt record cannot restore a creature over its cap.
    entity.damage.maxHealth = type.attributes().maxHealth();
    entity.damage.health = std::min(health, entity.damage.maxHealth);
    // AR-A4: eggLayTimer is not part of the save record (like
    // ambientSoundChance/stepAccumulator above it), so a reopened world simply
    // rerolls a fresh countdown off the restored rngState — a laying species
    // never gets stuck at zero, and a non-laying species leaves it unused.
    if (type.laysEggs()) {
        entity.eggLayTimer =
            kEggLayBaseTicks +
            static_cast<int>(mc::rng::nextInt(entity.rngState,
                                              static_cast<std::uint32_t>(kEggLayRandomTicks)));
    }
    // MobEntity#initGoals runs once at spawn, exactly like a fresh spawn.
    type.ai().configureBrain(entity.brain);
    installBreedingGoals(entity);
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
                        ActorReference attacker, DamageType type,
                        float extraKnockbackStrength) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    auto& entity = entities_[found->second];
    // The guards and the invulnerability window live in the shared pipeline, so
    // the player and every mob resolve a hit the same way regardless of
    // whether it came from a melee swing or (RW-0) a projectile. EQ-3: a mob's
    // own Resistance / Fire Resistance effects feed the same defensive stages a
    // player's do, gathered here off its effect store so the pipeline stays a
    // pure transform. Mobs carry no armor yet, so those context fields stay
    // defaulted.
    DamageContext context{type, amount};
    context.resistanceLevel = resistanceLevel(entity.effects);
    context.fireImmune = isFireImmune(entity.effects);
    const DamageOutcome outcome = applyDamage(entity.damage, context);
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
    // fixed lift, with the existing velocity halved first. ENCH-1's Knockback
    // enchant (extraKnockbackStrength, see EnchantmentCombat.hpp's
    // meleeKnockbackEnchantBonus) folds additively into the strength here
    // rather than replaying vanilla's second, separate takeKnockback call —
    // see that function's own comment for why; zero for every caller with no
    // weapon enchant to add.
    const float knockbackStrength = kKnockbackStrength + extraKnockbackStrength;
    const glm::vec3 away = pushBetween(knockbackOrigin, entity.position);
    const float lengthSquared = away.x * away.x + away.z * away.z;
    if (lengthSquared > 1e-9F) {
        const float inverse = knockbackStrength / std::sqrt(lengthSquared);
        entity.velocity.x = entity.velocity.x * 0.5F + away.x * inverse;
        entity.velocity.z = entity.velocity.z * 0.5F + away.z * inverse;
    }
    // Vanilla only supplies the vertical lift while the target is grounded.
    // An accepted follow-up hit in mid-air still refreshes horizontal
    // knockback, but cannot reset the target to another full jump arc.
    if (entity.onGround) {
        entity.velocity.y = std::min(entity.velocity.y * 0.5F + knockbackStrength,
                                     knockbackStrength);
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

bool EntitySystem::setOnFire(std::uint64_t entityId, int seconds) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    auto& entity = entities_[found->second];
    // Entity#setSecondsOnFire: a fire-immune creature never catches, and a dead
    // one is not relit. Vanilla only ever lengthens a burn, so take the max of
    // the current and requested duration; a negative request cannot shorten it.
    if (entity.type->fireImmune() || entity.damage.dead() || seconds <= 0) {
        return entity.fireTicks > 0;
    }
    entity.fireTicks = std::max(entity.fireTicks, seconds * kTicksPerSecond);
    return entity.fireTicks > 0;
}

bool EntitySystem::applyEffect(std::uint64_t entityId, core::StatusEffectId effect,
                               std::int32_t durationTicks, std::uint8_t amplifier) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    auto& entity = entities_[found->second];
    // A corpse does not accrue effects.
    if (entity.damage.dead()) {
        return false;
    }
    return mc::gameplay::applyEffect(entity.effects, effect, durationTicks, amplifier);
}

bool EntitySystem::removeEffect(std::uint64_t entityId, core::StatusEffectId effect) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    return mc::gameplay::removeEffect(entities_[found->second].effects, effect);
}

std::size_t EntitySystem::clearEffects(std::uint64_t entityId) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return 0U;
    }
    return mc::gameplay::clearEffects(entities_[found->second].effects);
}

bool EntitySystem::hasEffect(std::uint64_t entityId, core::StatusEffectId effect) const {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    return mc::gameplay::hasEffect(entities_[found->second].effects, effect);
}

bool EntitySystem::applyBaneOfArthropodsSlowness(std::uint64_t entityId, std::uint8_t level) {
    if (level == 0U) {
        return false;
    }
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    auto& entity = entities_[found->second];
    if (entity.damage.dead()) {
        return false;
    }
    // DamageEnchantment#onTargetDamaged: `20 + random.nextInt(10 * level)` ticks
    // of Slowness IV. `user.getRandom()` in vanilla; here the draw runs off the
    // target's own reproducible stream (mc::rng, Java's LegacyRandomSource core)
    // so a replayed hit lands the identical duration — no wall clock.
    const int randomDraw = static_cast<int>(mc::rng::nextInt(
        entity.rngState,
        static_cast<std::uint32_t>(baneOfArthropodsSlownessRandomBound(level))));
    const int durationTicks = baneOfArthropodsSlownessTicks(level, randomDraw);
    return mc::gameplay::applyEffect(entity.effects, slownessEffect(), durationTicks,
                                     kBaneOfArthropodsSlownessAmplifier);
}

bool EntitySystem::setInLove(std::uint64_t entityId) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    SimpleEntity& entity = entities_[found->second];
    // Only a breedable adult off cooldown enters love — a baby or a mob still on
    // its post-breed cooldown cannot.
    if (entity.dead() || !entity.canBreed()) {
        return false;
    }
    entity.loveTicks = kLoveTicks;
    return true;
}

bool EntitySystem::setAge(std::uint64_t entityId, int age) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    SimpleEntity& entity = entities_[found->second];
    // A non-breedable species has no age axis; keep it an adult.
    entity.age = entity.type->breedable() ? age : 0;
    return true;
}

bool EntitySystem::ageUp(std::uint64_t entityId, int seconds) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    SimpleEntity& entity = entities_[found->second];
    // AgeableMob#ageUp: only a baby (age < 0) of a breedable species grows; an
    // adult or a mob on breed cooldown is untouched. `type->breedable()` keeps a
    // non-ageable species pinned at 0.
    if (entity.dead() || !entity.type->breedable() || entity.age >= 0) {
        return false;
    }
    // age += seconds * 20, clamped at 0 (never overshoot into cooldown), exactly
    // as vanilla's ageUp does before it writes setAge.
    int age = entity.age + seconds * 20;
    if (age > 0) {
        age = 0;
    }
    if (age == entity.age) {
        return false;
    }
    entity.age = age;
    return true;
}

bool EntitySystem::ate(std::uint64_t entityId) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    SimpleEntity& entity = entities_[found->second];
    if (entity.dead()) {
        return false;
    }
    // Sheep#ate: clear the sheared flag (regrow wool) and, if this is a lamb,
    // age it up 60 seconds. Both halves report a change; a wooled adult that
    // somehow reached here is a no-op.
    bool changed = false;
    if (entity.sheared) {
        entity.sheared = false;
        changed = true;
    }
    if (ageUp(entityId, kSheepEatAgeUpSeconds)) {
        changed = true;
    }
    return changed;
}

bool EntitySystem::shear(std::uint64_t entityId) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    SimpleEntity& entity = entities_[found->second];
    // Sheep#readyForShearing: alive, not already sheared, not a baby.
    // Sabotage anchor ③ is the `entity.sheared` half of this guard.
    if (entity.dead() || entity.sheared || entity.baby()) {
        return false;
    }
    entity.sheared = true;
    return true;
}

bool EntitySystem::dye(std::uint64_t entityId, DyeColor color) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    SimpleEntity& entity = entities_[found->second];
    // SheepEntity#mobInteract's DyeItem gate: a live, dyeable creature whose
    // colour would actually change. A dead creature, a species with no dye
    // semantics, or a redundant same-colour dye all no-op (the caller then
    // spends no item — vanilla only decrements the stack inside the
    // colour-changed branch).
    if (entity.dead() || !entity.type->dyeable() || entity.color == color) {
        return false;
    }
    entity.color = color;
    return true;
}

bool EntitySystem::clearSheared(std::uint64_t entityId) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return false;
    }
    SimpleEntity& entity = entities_[found->second];
    if (!entity.sheared) {
        return false;
    }
    entity.sheared = false;
    return true;
}

void EntitySystem::processBreeding(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& requests) {
    for (const auto& [firstId, secondId] : requests) {
        SimpleEntity* first = byId(firstId);
        SimpleEntity* second = byId(secondId);
        // Both parents must still exist, be the same species, and still be ready:
        // a parent that died, grew out of love, or already bred this tick (its
        // cooldown is now non-zero) cancels the breed.
        if (first == nullptr || second == nullptr || first->type != second->type ||
            !first->readyToMate() || !second->readyToMate()) {
            continue;
        }
        // Capture what the baby needs before spawn(), which appends to entities_
        // and may reallocate the vector — invalidating first/second. Both parents
        // go on cooldown and fall out of love now (the vanilla anti-spam rule), so
        // a herd cannot breed every tick.
        const glm::vec3 midpoint = (first->position + second->position) * 0.5F;
        const std::uint64_t babySeed =
            (first->rngState ^ (second->rngState * 2654435761ULL)) | 1ULL;
        const entities::EntityType& species = *first->type;
        // Animal#finalizeSpawnChildFromBreeding: a successful breed always pays
        // 1-7 experience (getRandom().nextInt(7) + 1), independent of the
        // species' own xpReward (that field is a kill-only reward; breeding is
        // its own flat roll). Drawn off the first parent's own deterministic
        // stream — the same source its wander and AI already advance — before
        // the cooldown/love writes below and before spawn() can reallocate the
        // vector this loop iterates.
        const std::int32_t breedExperience =
            1 + static_cast<std::int32_t>(mc::rng::nextInt(first->rngState, 7U));
        const glm::vec3 breedPosition = first->position;
        first->age = kBreedCooldownTicks;
        second->age = kBreedCooldownTicks;
        first->loveTicks = 0;
        second->loveTicks = 0;
        pendingExperience_.emplace_back(breedPosition, breedExperience);
        // The baby spawns at the parents' midpoint as a newborn (age negative);
        // its RNG is derived from both parents so the same seed breeds identically.
        spawn(midpoint, species, babySeed);
        entities_.back().age = kBabyAgeTicks;
    }
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

std::optional<SimpleEntity> EntitySystem::detach(std::uint64_t entityId) {
    const auto found = idToIndex_.find(entityId);
    if (found == idToIndex_.end()) {
        return std::nullopt;
    }
    const std::size_t index = found->second;
    SimpleEntity taken = std::move(entities_[index]);
    // Swap-and-pop, matching removeInChunk; rebuildSpatialIndex reconciles the id
    // and section indexes afterwards.
    entities_[index] = std::move(entities_.back());
    entities_.pop_back();
    rebuildSpatialIndex();
    return taken;
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
    // LivingEntity#shouldDropLoot: with mob_drops off, the corpse leaves nothing
    // — and, further down, pays no experience either (LivingEntity#dropExperience
    // reads the same rule). The loot roll itself is skipped rather than rolled
    // and discarded, so the deterministic loot stream is not advanced by a death
    // that produced nothing.
    EntityDrops drops =
        mobDropsEnabled_ ? entity.kind().rollLoot(lootRandomState_) : EntityDrops{};
    // DYE-2: Sheep.json's wool drop rolls a placeholder white wool (the loot fn
    // has no access to the entity), so its colour is applied here where the
    // dying entity's authoritative DyeColor is in scope — the kill-path analogue
    // of the shear path's woolBlockFor. Any wool entry is retinted to the mob's
    // colour and its BlockItem re-derived; a white sheep keeps white wool, a
    // dyed one drops its colour. Wool-agnostic to every other species: non-wool
    // drops (mutton, feathers, rotten flesh) fail isWool and pass through
    // untouched, so this never needs a per-species check.
    for (std::size_t entry = 0; entry < drops.count; ++entry) {
        ItemStack& stack = drops.entries[entry];
        if (items::isWool(stack.block)) {
            stack.block = items::woolBlockFor(entity.color);
            stack.item = blockItemFor(stack.block);
        }
    }
    pendingDrops_.emplace_back(entity.position + glm::vec3{0.0F, 0.25F, 0.0F}, drops);
    // LivingEntity#dropExperience, gated by lastHurtByPlayerMemoryTime > 0
    // (Java's PLAYER_HURT_EXPERIENCE_TIME window, 100 ticks — the same 100
    // hurt() stamps into recentAttackerTicks). A creature that starved, burned,
    // fell or was struck by another mob never had a *player* attacker land
    // inside that window, so it must not pay out: only hurt()'s default
    // ActorReference::player() attacker sets lastAttacker to Kind::Player, and
    // only while recentAttackerTicks (ticked down every tick in tick()) is
    // still positive does that attribution count as "recent" the way vanilla's
    // memory timer does. A zero xpReward (the default; every passive animal)
    // costs nothing extra to check.
    if (mobDropsEnabled_ && entity.type->xpRewardMax() > 0 &&
        entity.lastAttacker.kind == ActorReference::Kind::Player &&
        entity.recentAttackerTicks > 0) {
        // AnimalEntity rolls 1..3, an ordinary Mob pays its flat reward; the roll
        // draws from the same deterministic loot stream every other death-time
        // drop in this file uses.
        pendingExperience_.emplace_back(entity.position,
                                        entity.type->rollExperienceReward(lootRandomState_));
    }
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
    float simulationRadius,
    bool raining,
    ItemStack heldItem,
    const EnvironmentSnapshot& environment) {
    EntityTickResult result;
    ++gameTick_;
    // AnimalMateGoal emits breeds here; the babies are spawned after the AI loop
    // (spawning mid-loop would reallocate the vector this loop iterates). Each
    // entry is (parentA id, parentB id) — the lower-id parent filed it.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> breedRequests;
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
        PlayerAiView{pusher, playerPresent, playerAlive, playerCreative, pusherWidth, pusherHeight,
                     heldItem},
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
        // AgeableMob#aiStep age logic (EM-3): a baby's age climbs toward zero and
        // becomes an adult on the tick it reaches it; an adult's positive breed
        // cooldown counts back down to zero. Love counts down independently. Only
        // moves for a breedable species (age/love stay zero otherwise).
        if (entity.age < 0) {
            ++entity.age;  // grows up; reaches 0 -> adult, full-size hitbox
        } else if (entity.age > 0) {
            --entity.age;  // breed cooldown expiring
        }
        if (entity.loveTicks > 0) {
            --entity.loveTicks;
        }
        entity.movementSpeedMultiplier = 1.0F;

        // AR-M2f: LivingEntity#isInDaylight, generalised off the type rather than
        // duplicated per hostile class. Gate is three data reads and no species
        // switch: MobCategory::Monster (a passive cow never reaches this branch),
        // EntityBehavior::Undead (the family vanilla wires this to via
        // Zombie#burnsInDaylight — a creeper or spider never burns even though
        // both are MONSTER), and !sunImmune() (husk's own bit skips it here —
        // the SunImmune bit was introduced alongside this ignition source in
        // AR-M2, not reserved earlier by EM1/AR-M1). "Day" is
        // Level#isDay's own definition (skyDarken < 4), read off the same
        // per-tick EnvironmentSnapshot NaturalSpawner uses so this can never
        // disagree with the spawner about what time it is.
        //
        // The ignition itself is vanilla's probabilistic rule verbatim, not the
        // AR-M2 placeholder that burned on any full-sun cell: a brightness band
        // (f > 0.5), then a per-tick roll (random.nextFloat()*30 < (f-0.4)*2),
        // then sky visibility. The roll draws off the entity's own reproducible
        // stream (entity.rngState, Java's LegacyRandomSource core — the same
        // this.random.nextFloat() vanilla uses), never the wall clock, so a
        // fixed seed reproduces the exact ignition tick sequence. `f` is the
        // eye-cell brightness with the tick's ambient darkness applied, so rain
        // and thunder fold straight into the gate: heavier weather lowers f
        // below the 0.5 band and the mob simply stops catching, no separate rain
        // branch (Entity#baseTick's rain still extinguishes an already-lit one
        // just below). Not in water reuses the fireFootX/Y/Z cells the fire tick
        // performs, so both checks agree on what "underwater" means. Vanilla
        // also skips a helmeted zombie; no mob armor exists yet (EQ-4 defers it),
        // so nothing here reads one.
        if (!entity.damage.dead() && entity.type->category() == entities::MobCategory::Monster &&
            entity.type->undead() && !entity.type->sunImmune() &&
            environment.ambientDarkness < 4) {
            const int footY = floorToInt(entity.position.y);
            const int headX = floorToInt(entity.position.x);
            const int headY = footY + 1;
            const int headZ = floorToInt(entity.position.z);
            const float brightness =
                environment::eyeBrightness(world, headX, headY, headZ, environment);
            // Draw the roll only inside the brightness band, matching vanilla's
            // short-circuit order (f > 0.5F && random.nextFloat()*30 < ...): a
            // dark cell never advances the entity's stream, so unlit mobs stay
            // bit-for-bit aligned with the seed.
            const bool bandPassed = brightness > 0.5F;
            const bool rolled =
                bandPassed &&
                mc::rng::nextFloat(entity.rngState) * 30.0F < (brightness - 0.4F) * 2.0F;
            const bool skyVisible = world.directSkyLight(headX, headY, headZ) >= 15U;
            const bool submerged = world::isFluid(world.block(headX, footY, headZ)) ||
                                   world::isFluid(world.block(headX, headY, headZ));
            if (rolled && skyVisible && !submerged) {
                entity.fireTicks = std::max(entity.fireTicks, 8 * kTicksPerSecond);
            }
        }

        // Entity#baseTick's fire block. A burning creature is put out the moment
        // it touches water or stands in the rain under open sky (isBeingRainedOn),
        // and otherwise takes one point of OnFire damage every second while
        // `fireTicks` counts down. A fire-immune species never carries fireTicks
        // (setOnFire refuses it), so the whole block is skipped for it. The burn
        // routes through the shared damage pipeline and the same die() path fall
        // damage uses, so a mob that burns to death drops its loot once.
        if (entity.fireTicks > 0 && !entity.damage.dead()) {
            const int fireFootX = floorToInt(entity.position.x);
            const int fireFootY = floorToInt(entity.position.y);
            const int fireFootZ = floorToInt(entity.position.z);
            const bool inWater =
                world::isFluid(world.block(fireFootX, fireFootY, fireFootZ)) ||
                world::isFluid(world.block(fireFootX, fireFootY + 1, fireFootZ));
            // Entity#isBeingRainedOn: raining, and the head cell can see the sky.
            const bool rainedOn =
                raining && world.directSkyLight(fireFootX, fireFootY + 1, fireFootZ) > 0U;
            if (inWater || rainedOn) {
                entity.fireTicks = 0;
            } else {
                // Vanilla checks `fireTicks % 20 == 0` before decrementing, so a
                // 100-tick burn hits at 100/80/60/40/20 — five points for five
                // seconds — and the last decrement leaves it at zero.
                if (entity.fireTicks % kFireDamageInterval == 0) {
                    // EQ-3: Fire Resistance makes the per-second burn bounce off
                    // (LivingEntity#hurt rejects any IsFire source when the
                    // effect is held); the shared pipeline's fireImmune guard
                    // does it, so an immune mob simply takes no burn tick.
                    DamageContext burn{DamageType::OnFire, 1.0F};
                    burn.fireImmune = isFireImmune(entity.effects);
                    const DamageOutcome outcome = applyDamage(entity.damage, burn);
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
                --entity.fireTicks;
            }
        }

        // LivingEntity#tickEffects: advance the active MobEffects. The store
        // returns what this tick wants applied; the creature's DamageState and
        // movement multiplier apply it, so poison/regeneration go through the
        // shared damage pipeline and the same die() path fire uses. An entity
        // with no effects returns immediately (count == 0), so this is free for
        // the common case. The speed factor multiplies into the per-tick
        // movement multiplier that was just reset to 1.0, so a lapsed speed
        // effect restores the base speed on its own with nothing to unload.
        if (!entity.damage.dead() && !entity.effects.empty()) {
            const EffectTickOutcome effectTick =
                tickEffects(entity.effects, entity.damage.health);
            if (effectTick.heal > 0.0F) {
                entity.damage.health =
                    std::min(entity.damage.health + effectTick.heal, entity.damage.maxHealth);
            }
            if (effectTick.damage > 0.0F) {
                const DamageOutcome outcome =
                    applyDamage(entity.damage, effectTick.damageType, effectTick.damage);
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
            entity.movementSpeedMultiplier *= effectTick.speedMultiplier;
        }

        // MobEntity#baseTick's ambient scheduler: every tick it rolls
        // nextInt(1000) against a counter that climbs one per tick and snaps
        // back to -getMinAmbientSoundDelay() (80) after an idle sound, so a
        // species barks roughly every four seconds. The roll is strictly less
        // than the pre-increment counter, so a freshly reset (negative) counter
        // can never fire — exactly like vanilla's nextInt(1000) < counter++.
        if (!entity.damage.dead() &&
            static_cast<int>(mc::rng::nextInt(entity.rngState, 1000U)) <
                entity.ambientSoundChance++) {
            entity.ambientSoundChance = -kMinAmbientSoundDelay;
            pendingSounds_.push_back({entity.position, MobSoundEvent::Ambient, entity.type});
        }

        // AR-A4: ChickenEntity#aiStep's egg timer, generalised off
        // laysEggs() rather than a chicken-only branch — any future
        // egg-laying species (vanilla's own set is chicken-only in 26.1) rides
        // the same countdown. Reuses the death-loot drop queue (pendingDrops_)
        // since "one item stack appears at this position" is exactly what that
        // queue already carries; GameSession drains both through the same
        // ItemEntitySystem::spawn call. Off the entity's own rngState, so lay
        // ticks are deterministic per seed like every other timer here.
        if (!entity.damage.dead() && entity.type->laysEggs() && --entity.eggLayTimer <= 0) {
            EntityDrops drops;
            drops.add(entity.type->eggLay().item);
            pendingDrops_.emplace_back(entity.position + glm::vec3{0.0F, 0.25F, 0.0F}, drops);
            entity.eggLayTimer =
                kEggLayBaseTicks +
            static_cast<int>(mc::rng::nextInt(entity.rngState,
                                              static_cast<std::uint32_t>(kEggLayRandomTicks)));
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
            if (const auto partner = entity.brain.takeBreedRequest()) {
                breedRequests.emplace_back(entity.id, *partner);
            }
            if (const auto grassCell = entity.brain.takeEatGrassRequest()) {
                result.grassEats.push_back({entity.id, *grassCell});
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
        // vanilla's isTouchingWater check. AR-A4: a fallImmune() type (a
        // chicken, in vanilla also bats/parrots) still resets fallDistance on
        // landing like everyone else — it just never converts it to damage,
        // read off the type rather than a species switch here.
        const int footX = floorToInt(entity.position.x);
        const int footY = floorToInt(entity.position.y);
        const int footZ = floorToInt(entity.position.z);
        if (entity.damage.dead() ||
            world::isFluid(world.block(footX, footY, footZ)) ||
            world::isFluid(world.block(footX, footY + 1, footZ))) {
            entity.fallDistance = 0.0F;
        } else if (entity.onGround) {
            if (entity.fallDistance > 0.0F && !entity.type->fallImmune()) {
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

    // AnimalMateGoal#breed: resolve the breeds the AI pass requested now that the
    // loop over the (possibly-reallocating) vector is done. Spawning a baby here
    // appends through spawn(), which keeps every parallel index consistent.
    processBreeding(breedRequests);

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
