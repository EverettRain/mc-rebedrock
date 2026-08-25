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
                             float inaccuracy, float punchKnockback, int flameIgniteSeconds) {
    Projectile projectile;
    projectile.position = position;
    projectile.previousPosition = position;
    projectile.velocity = velocity;
    // RW-1a #16 — Projectile#shoot's scatter, ported exactly: normalize the aim
    // direction, add a per-axis triangular jitter `random.triangle(0.0,
    // 0.0172275 * inaccuracy)`, THEN rescale to the original speed. Normalizing
    // before the add makes the jitter a fixed angular spread (the RW-0 form
    // added Gaussian noise after the scale, so a fast draw scattered wider than
    // a slow one — wrong). Drawn from the caller's deterministic stream only
    // (never a wall clock, REGULAR.md #6). No `rng`, or a degenerate (zero-
    // length) velocity, means no scatter at all — a test's dead-reckoning
    // launch, or a future Piercing continuation that must not re-roll.
    const float speed = glm::length(projectile.velocity);
    if (rng != nullptr && speed > 1e-6F) {
        const double spread = kProjectileScatterSpread * static_cast<double>(inaccuracy);
        // RandomSource#triangle(mode, deviation) = mode + deviation *
        // (nextDouble() - nextDouble()): the difference of two uniforms is a
        // symmetric triangular distribution centred on `mode`.
        const auto triangle = [&]() {
            return spread * (rng->nextDouble() - rng->nextDouble());
        };
        glm::vec3 direction = projectile.velocity / speed;
        direction.x += static_cast<float>(triangle());
        direction.y += static_cast<float>(triangle());
        direction.z += static_cast<float>(triangle());
        projectile.velocity = direction * speed;
    }
    projectile.shooterId = shooterId;
    projectile.damage = damage;
    projectile.critical = critical;
    projectile.punchKnockback = punchKnockback;
    projectile.flameIgniteSeconds = flameIgniteSeconds;
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
                                              world::gen::JavaRandom& rng,
                                              Inventory* pickupInventory) {
    // `rng` now drives the RW-1a #13 integer crit roll below.
    std::vector<ItemStack> pickedUp;

    for (auto& projectile : entities_) {
        projectile.previousPosition = projectile.position;

        if (projectile.inGround) {
            // AbstractArrow#tickDespawn: only a stuck (no longer moving)
            // projectile accrues lifeTicks toward the pickup-timeout despawn.
            ++projectile.lifeTicks;
            continue;
        }

        // RW-1a #12 — physics ordering ported exactly from AbstractArrow#tick:
        // apply the per-axis drag FIRST (0.6 in water, else 0.99), THEN subtract
        // gravity from Y, THEN move. RW-0 subtracted gravity before drag, which
        // damps the just-added gravity impulse and gives a subtly flatter arc
        // than vanilla. Water drag is a full per-axis multiply (vanilla scales
        // the whole delta by getWaterInertia before the gravity step).
        const glm::ivec3 foot{
            static_cast<int>(std::floor(projectile.position.x)),
            static_cast<int>(std::floor(projectile.position.y)),
            static_cast<int>(std::floor(projectile.position.z)),
        };
        const bool inWater = world::isFluid(world.block(foot.x, foot.y, foot.z));
        projectile.velocity *= inWater ? kProjectileWaterDrag : kProjectileAirDrag;
        projectile.velocity.y -= kProjectileGravity;

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
            // RW-1a #8 — AbstractArrow#onHitEntity: the applied damage is
            // `ceil(velocity.length() * baseDamage)`, derived HERE at hit time
            // from the projectile's current (drag-decayed) speed, not baked in
            // at launch. `travelled` is exactly this tick's speed (the length of
            // the post-drag displacement), so a long, slow arc lands softer than
            // a point-blank shot.
            int appliedDamage = static_cast<int>(std::ceil(travelled * projectile.damage));
            if (appliedDamage < 0) {
                appliedDamage = 0;
            }
            if (projectile.critical) {
                // RW-1a #13 — the crit bonus is an INTEGER roll `nextInt(i/2 +
                // 2)` added to `i` (AbstractArrow#onHitEntity), drawn from the
                // tick's deterministic stream (sabotage② target: a wall-clock or
                // global-RNG draw breaks replay). A fully-drawn crit therefore
                // deals strictly more than the same non-crit shot, but by a
                // reproducible integer amount, not a flat 1.5x.
                const int bonusBound = appliedDamage / 2 + kProjectileCriticalBonusBase;
                appliedDamage += rng.nextInt(bonusBound);
            }
            const glm::vec3 hitPoint = origin + displacement * (entityHit->distance / travelled);
            // RW-4 — Punch rides in as the hit's extraKnockbackStrength (the same
            // knockback unit melee Knockback uses), and Flame ignites the struck
            // target once the hit lands. Both are the deterministic per-shot values
            // the firing bow baked in at spawn (RangedEnchantment.hpp); zero for an
            // unenchanted arrow, so this reduces to RW-1a's plain hit.
            const bool landed = entities.hurt(entityHit->entityId,
                                              static_cast<float>(appliedDamage), hitPoint,
                                              projectile.shooterId, DamageType::Projectile,
                                              projectile.punchKnockback);
            static_cast<void>(landed);
            if (projectile.flameIgniteSeconds > 0) {
                static_cast<void>(
                    entities.setOnFire(entityHit->entityId, projectile.flameIgniteSeconds));
            }
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
    // collected and its pickupItem stowed. RW-1a #7 — the projectile is
    // consumed ONLY once the item has somewhere to go: with a `pickupInventory`,
    // a full backpack (Inventory::add returning false) leaves the arrow on the
    // ground to be retried next tick instead of deleting it (the AbstractArrow#
    // playerTouch dupe/loss bug RW-0 shipped). Without an inventory the RW-0
    // collect-and-consume behaviour stands (a test asserting the raw mechanic).
    if (playerPresent) {
        for (auto& projectile : entities_) {
            if (!projectile.inGround || projectile.pickupState == ProjectilePickupState::NoPickup) {
                continue;
            }
            const glm::vec3 delta = playerPosition - projectile.position;
            if (glm::dot(delta, delta) > kPickupContactRadiusSquared) {
                continue;
            }
            if (projectile.pickupItem.empty()) {
                // Nothing to give (a NoPickup-item projectile) — still claimed.
                projectile.consumed = true;
                continue;
            }
            if (pickupInventory != nullptr) {
                ItemStack toStow = projectile.pickupItem;
                if (!pickupInventory->add(toStow)) {
                    // Full inventory: the arrow stays on the ground, not lost.
                    continue;
                }
                pickedUp.push_back(projectile.pickupItem);
            } else {
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
