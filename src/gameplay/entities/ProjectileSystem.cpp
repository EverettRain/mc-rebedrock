#include "gameplay/entities/ProjectileSystem.hpp"

#include "gameplay/EntitySystem.hpp"
#include "world/Block.hpp"
#include "world/VoxelRaycast.hpp"
#include "world/World.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace mc::gameplay {
namespace {

// AbstractArrow's hitbox is small (0.5 x 0.5 x 0.5, EntityType.ARROW
// `.sized(0.5F, 0.5F)`); half-extent for the "landed arrow touched by a
// player" contact test below, matching ExperienceOrb's own contact tolerance
// shape.
constexpr float kPickupContactRadiusSquared = 0.9F * 0.9F;

} // namespace

void ProjectileSystem::spawn(glm::vec3 position, glm::vec3 velocity, ActorReference shooterId,
                             float damage, bool critical, ProjectilePickupState pickupState,
                             ItemStack pickupItem, world::gen::JavaRandom* rng,
                             float inaccuracy) {
    Projectile projectile;
    projectile.position = position;
    projectile.previousPosition = position;
    projectile.velocity = velocity;
    // AbstractArrow#shootFromRotation / Projectile#shoot: a Gaussian scatter
    // per axis, scaled by the shot's inaccuracy — drawn from the caller's
    // deterministic stream only (never a wall clock, REGULAR.md #6). No `rng`
    // means no scatter at all (a test's dead-reckoning launch, or a future
    // Piercing continuation that must not re-roll).
    if (rng != nullptr) {
        projectile.velocity.x += static_cast<float>(rng->nextGaussian()) * inaccuracy /
                                 kProjectileScatterDivisor;
        projectile.velocity.y += static_cast<float>(rng->nextGaussian()) * inaccuracy /
                                 kProjectileScatterDivisor;
        projectile.velocity.z += static_cast<float>(rng->nextGaussian()) * inaccuracy /
                                 kProjectileScatterDivisor;
    }
    projectile.shooterId = shooterId;
    projectile.damage = damage;
    projectile.critical = critical;
    projectile.pickupState = pickupState;
    projectile.pickupItem = pickupItem;
    entities_.push_back(projectile);
}

void ProjectileSystem::restore(glm::vec3 position, glm::vec3 velocity, ActorReference shooterId,
                               float damage, bool critical, ProjectilePickupState pickupState,
                               ItemStack pickupItem, bool inGround, glm::ivec3 inBlockPos,
                               std::uint32_t lifeTicks) {
    Projectile projectile;
    projectile.position = position;
    projectile.previousPosition = position;
    projectile.velocity = velocity;
    projectile.shooterId = shooterId;
    projectile.damage = damage;
    projectile.critical = critical;
    projectile.pickupState = pickupState;
    projectile.pickupItem = pickupItem;
    projectile.inGround = inGround;
    projectile.inBlockPos = inBlockPos;
    projectile.lifeTicks = lifeTicks;
    entities_.push_back(projectile);
}

std::vector<ItemStack> ProjectileSystem::tick(const world::World& world, EntitySystem& entities,
                                              glm::vec3 playerPosition, bool playerPresent,
                                              world::gen::JavaRandom& rng) {
    static_cast<void>(rng);  // reserved for RW-1+'s draw-dependent scatter roll
    std::vector<ItemStack> pickedUp;

    for (auto& projectile : entities_) {
        projectile.previousPosition = projectile.position;

        if (projectile.inGround) {
            // AbstractArrow#tickDespawn: only a stuck (no longer moving)
            // projectile accrues lifeTicks toward the pickup-timeout despawn.
            ++projectile.lifeTicks;
            continue;
        }

        // --- physics: gravity, drag, water drag (AbstractArrow#tick) ---
        const glm::ivec3 foot{
            static_cast<int>(std::floor(projectile.position.x)),
            static_cast<int>(std::floor(projectile.position.y)),
            static_cast<int>(std::floor(projectile.position.z)),
        };
        const bool inWater = world::isFluid(world.block(foot.x, foot.y, foot.z));
        projectile.velocity.y -= kProjectileGravity;
        if (inWater) {
            projectile.velocity *= kProjectileWaterDrag;
        } else {
            projectile.velocity *= kProjectileAirDrag;
        }

        const glm::vec3 origin = projectile.position;
        const glm::vec3 displacement = projectile.velocity;
        const float travelled = glm::length(displacement);

        // --- hit test: raycast the tick's displacement against entities, then
        // blocks (AbstractArrow#tick's ProjectileUtil.getHitResultOnMoveVector
        // checks entities first, since a projectile that would also clip a
        // block on the same step still hits whichever is nearer). ---
        std::optional<EntityRayHit> entityHit;
        if (travelled > 1e-6F) {
            entityHit = entities.raycast(origin, displacement, travelled);
            // A projectile never hits its own shooter (AbstractArrow#canHitEntity's
            // `entity == this.getOwner()` exclusion) — the tick a bow's own arrow
            // spawns at the player's eye must not immediately "hit" the player
            // who fired it once players are targetable by this raycast.
            if (entityHit.has_value() && projectile.shooterId.kind == ActorReference::Kind::Entity &&
                entityHit->entityId == projectile.shooterId.entityId) {
                entityHit.reset();
            }
        }

        std::optional<world::VoxelRaycastHit> blockHit;
        if (travelled > 1e-6F) {
            const glm::vec3 direction = displacement / travelled;
            blockHit = world::raycastVoxels(world, origin, direction, travelled,
                                            /*includeFluids=*/false);
        }

        // Entities take priority when both are found and the entity is at
        // least as close, matching vanilla's own tie-break (an arrow that
        // would clip a mob standing in a doorway hits the mob, not the frame).
        const bool hitEntityFirst = entityHit.has_value() &&
            (!blockHit.has_value() || entityHit->distance <= blockHit->distance);

        if (hitEntityFirst) {
            // --- entity hit: through Damage.hpp, never bypassed (sabotage① target) ---
            float appliedDamage = projectile.damage;
            if (projectile.critical) {
                // AbstractArrow#getBaseDamage: a fully-drawn crit shot deals
                // strictly more than a non-crit shot of the same base damage.
                appliedDamage *= kProjectileCriticalDamageMultiplier;
            }
            const glm::vec3 hitPoint = origin + displacement * (entityHit->distance / travelled);
            const bool landed = entities.hurt(entityHit->entityId, appliedDamage, hitPoint,
                                              projectile.shooterId, DamageType::Projectile);
            static_cast<void>(landed);
            // AbstractArrow#onHitEntity: a spent arrow (non-piercing, RW-4's
            // future concern) is consumed on impact rather than sticking, the
            // way vanilla's `pierceLevel <= 0` branch removes it after the hit.
            projectile.consumed = true;
            continue;
        }

        if (blockHit.has_value()) {
            // --- block hit: stick, never pass through (sabotage③ target) ---
            projectile.position = origin + displacement * (blockHit->distance / std::max(travelled, 1e-6F));
            projectile.velocity = glm::vec3{0.0F};
            projectile.inGround = true;
            projectile.inBlockPos = blockHit->block;
            projectile.lifeTicks = 0U;
            continue;
        }

        // No hit this tick: the full displacement is safe to take.
        projectile.position = origin + displacement;

        // Despawn a flying projectile that fell out of the world (mirrors the
        // pools above; vanilla's own removal is via the general out-of-world
        // check rather than a projectile-specific one).
    }

    // --- pickup: a landed, pickupable projectile touched by the player is
    // collected and its pickupItem handed back to the caller. ---
    if (playerPresent) {
        for (auto& projectile : entities_) {
            if (!projectile.inGround || projectile.pickupState == ProjectilePickupState::NoPickup) {
                continue;
            }
            const glm::vec3 delta = playerPosition - projectile.position;
            if (glm::dot(delta, delta) > kPickupContactRadiusSquared) {
                continue;
            }
            if (!projectile.pickupItem.empty()) {
                pickedUp.push_back(projectile.pickupItem);
            }
            projectile.consumed = true;
        }
    }

    std::erase_if(entities_, [](const Projectile& projectile) {
        return projectile.consumed ||
               (projectile.inGround && projectile.lifeTicks >= kProjectileLifetimeTicks) ||
               projectile.position.y < -64.0F;
    });

    return pickedUp;
}

} // namespace mc::gameplay
