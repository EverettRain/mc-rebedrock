#pragma once

// RW-0: the projectile system — the third non-living SoA pool, alongside
// ItemEntitySystem (dropped items) and ExperienceOrbSystem (experience orbs).
// A flying arrow/trident-to-be is a flat value type with physics, a raycast
// hit test and a pickup/despawn timer; it carries no AI, no health, no
// equipment, none of the columns a Mob (SimpleEntity) carries — RW-DESIGN.md
// §3 / RW-experience/REGULAR.md #2 forbid folding it into MobEntity the same
// way XP-1 forbids it for orbs.
//
// Ports 26.1 net.minecraft.world.entity.projectile.AbstractArrow /
// PersistentProjectileEntity:
//   - gravity 0.05/tick (AbstractArrow#getDefaultGravity, heavier than an
//     item's 0.04 or an orb's 0.03 — an arrow drops faster than a puff of
//     dropped loot), 0.99 in-air drag (tickDespawn's applied per-axis
//     multiplyBy(0.99) after gravity is subtracted from Y), 0.6 water drag
//     (isInWater's stronger per-axis damping compared to the 0.99 air case).
//   - Each tick raycasts along the tick's displacement (current position to
//     where gravity/drag would carry it) against the world's entities and
//     blocks, exactly like AbstractArrow#tick's ProjectileUtil.getHitResultOnMoveVector.
//     A block hit sticks the projectile in place (inGround, inBlockPos
//     recorded, velocity zeroed) the way onHitBlock does; an entity hit routes
//     through Damage.hpp's applyDamage (never bypassed — that is what makes a
//     future armored target automatically take less arrow damage once EQ-2
//     lands, RW-DESIGN.md §5) plus knockback and a critical-hit bonus.
//   - pickupState (AbstractArrow.PickupStatus): DISALLOWED (no pickup, ever),
//     ALLOWED (a contacting player picks it up as the carried pickupItem, once
//     RW-1 sets that field to a real arrow stack), CREATIVE_ONLY (only a
//     creative player may pick it up — RW-1's future concern; RW-0 stores the
//     state and lets any player pick up an ALLOWED projectile since there is
//     no creative-mode signal wired into this pool yet).
//   - lifeTicks counts up; a stuck arrow that outlives the timeout despawns
//     (AbstractArrow#tickDespawn's `life >= lifetime` check, vanilla default
//     1200 ticks / 60 seconds once no longer moving fast).
//
// Determinism (REGULAR.md / RW-experience/REGULAR.md #6): critical-hit rolls
// and any future scatter draw from a caller-supplied JavaRandom stream — never
// the wall clock or a global RNG — so the same seed and launch sequence always
// produces the same trajectory and hits.
#include "gameplay/Damage.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/entities/MobBrain.hpp"
#include "world/gen/JavaRandom.hpp"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mc::world {
class World;
}

namespace mc::gameplay {

class EntitySystem;

// AbstractArrow.PickupStatus, ported verbatim.
enum class ProjectilePickupState : std::uint8_t {
    NoPickup,      // DISALLOWED: never picked up (a hostile mob's shot, e.g.)
    Pickupable,    // ALLOWED: any player walking through it picks it up
    CreativeOnly,  // CREATIVE_ONLY: only a creative-mode player may retrieve it
};

// One flying (or stuck) projectile. A plain value type — no owning pointers,
// no vtable — so the pool below is a flat std::vector the tick walks by
// index, matching ItemEntity/ExperienceOrb exactly.
struct Projectile final {
    glm::vec3 position{0.0F};
    glm::vec3 previousPosition{0.0F};
    glm::vec3 velocity{0.0F};
    // Entity#getId of whoever fired this, so the hit does not knock back or
    // damage its own shooter and so future revenge-target AI can react to it.
    // ActorReference::player() for the player's own shots (RW-1's bow), or
    // ActorReference::entity(id) once a mob can fire one.
    ActorReference shooterId{};
    // RW-1a #8 — the projectile's BASE damage, NOT the final applied damage.
    // AbstractArrow#onHitEntity computes the applied damage at hit time as
    // `ceil(velocity.length() * baseDamage)`, so a shot that has bled off speed
    // over a long arc hits softer than a point-blank one. RW-0 baked the launch
    // velocity into this field and applied it flat; RW-1a stores the true base
    // and derives the velocity term in the tick (see tick()).
    float damage = 2.0F;
    // AbstractArrow#isCritArrow: a fully-drawn bow shot adds a random integer
    // 0-to-(i/2+1) bonus (RW-1a #13) and the render-only "sparkle" particles;
    // this field carries the flag, the tick rolls the bonus deterministically.
    bool critical = false;
    // RW-4 — the bow's Punch/Flame enchantment effects baked in at launch (Power
    // is folded straight into `damage` above, needing no field). `punchKnockback`
    // is the extra knockback STRENGTH an entity hit passes into
    // EntitySystem::hurt(extraKnockbackStrength); `flameIgniteSeconds` is the
    // seconds of burning an entity hit inflicts via EntitySystem::setOnFire. Both
    // default to zero (an unenchanted arrow adds no shove and no fire). Transient
    // combat modifiers only: they are NOT part of the PJTL save block (a
    // mid-flight Punch/Flame arrow reloaded from disk loses the enchant effect —
    // a documented RW-4 simplification, kept so the persistence golden and codec
    // stay untouched, since a stuck arrow never re-hits an entity anyway).
    float punchKnockback = 0.0F;
    int flameIgniteSeconds = 0;
    ProjectilePickupState pickupState = ProjectilePickupState::Pickupable;
    // What a successful pickup gives the player. Empty (the default) means
    // "nothing to give" — RW-0's generic mechanic, since no arrow ITEM exists
    // yet (that is RW-1's job: it will set this to the arrow ItemStack).
    ItemStack pickupItem{};
    // AbstractArrow#inGround: stuck in a block face, not moving, eligible for
    // the despawn timeout and for pickup.
    bool inGround = false;
    // The block cell the projectile is stuck in, valid only while inGround.
    glm::ivec3 inBlockPos{0};
    // AbstractArrow#life: ticks since this projectile stopped moving (started
    // counting the moment it stuck); despawns once it crosses the timeout.
    std::uint32_t lifeTicks = 0U;
    // Transient, tick-local: set the instant an entity hit consumes this
    // projectile (AbstractArrow#onHitEntity's non-piercing remove-on-impact)
    // or a player pickup claims it, so the end-of-tick sweep removes it
    // without overloading inGround/lifeTicks (both of which are meaningful,
    // save-persisted state for a still-stuck arrow). Never true across a
    // save/restore round trip — a consumed projectile is gone before the tick
    // that consumed it ends.
    bool consumed = false;

    [[nodiscard]] friend bool operator==(const Projectile&, const Projectile&) = default;
};

// AbstractArrow physics/despawn constants (26.1), named here so RW-1..3 and
// tests can reference the same numbers the tick uses rather than re-deriving
// them.
inline constexpr float kProjectileGravity = 0.05F;         // getDefaultGravity()
inline constexpr float kProjectileAirDrag = 0.99F;          // tickDespawn's multiplyBy
inline constexpr float kProjectileWaterDrag = 0.6F;         // isInWater's stronger damping
inline constexpr std::uint32_t kProjectileLifetimeTicks = 1200U;  // tickDespawn's `lifetime`
// AbstractArrow#doPostHitEffects / EntityHitResult knockback: an unenchanted
// hit shoves the target by (knockback * 6/10) blocks/tick; 0 here (no
// Punch/Knockback enchant support yet, RW-4) reduces to vanilla's baseline
// shove, which every arrow already carries independent of the enchant bonus.
inline constexpr float kProjectileBaseKnockback = 0.6F;
// RW-1a #13 — AbstractArrow#onHitEntity's crit bonus, ported verbatim as an
// integer roll rather than the RW-0 flat multiply: once the base damage `i` is
// `ceil(velocity.length() * baseDamage)`, a crit adds `random.nextInt(i / 2 +
// 2)` extra half-hearts. Kept here as the `+ 2` addend so tests and the tick
// reference the same number.
inline constexpr int kProjectileCriticalBonusBase = 2;

// RW-1a #16 — Projectile#shoot's per-axis divergence: the velocity's normalized
// direction is jittered by `random.triangle(0.0, 0.0172275 * inaccuracy)` on
// each axis BEFORE being rescaled to the shot's speed (so the scatter is a fixed
// angular spread independent of draw strength), replacing RW-0's post-scale
// Gaussian. 1.0 here matches a bow's own default inaccuracy (`BowItem` passes
// 1.0F to `shoot`).
inline constexpr float kProjectileDefaultInaccuracy = 1.0F;
inline constexpr double kProjectileScatterSpread = 0.0172275;

class ProjectileSystem final {
  public:
    // Launches one projectile. `rng`, when supplied, applies AbstractArrow's
    // own small Gaussian scatter to `velocity` — vanilla's `shoot()` jitters
    // even a dead-on aim by `inaccuracy`/20 per axis — drawn from the world's
    // deterministic JavaRandom stream (REGULAR.md), never the wall clock or a
    // global RNG, so the same seed and the same launch call sequence always
    // lands the same trajectory. `rng == nullptr` (the default) launches with
    // no scatter at all — deterministic by construction, useful for tests and
    // for RW-4's future Piercing shots that must not re-roll per pierce.
    void spawn(glm::vec3 position, glm::vec3 velocity, ActorReference shooterId, float damage,
              bool critical = false, ProjectilePickupState pickupState = ProjectilePickupState::Pickupable,
              ItemStack pickupItem = {}, world::gen::JavaRandom* rng = nullptr,
              float inaccuracy = kProjectileDefaultInaccuracy,
              float punchKnockback = 0.0F, int flameIgniteSeconds = 0);

    // Reinstates a projectile from a save, keeping the fields a fresh spawn()
    // would reset (inGround/inBlockPos/lifeTicks) so a world reopens with its
    // stuck arrows exactly where they landed.
    void restore(glm::vec3 position, glm::vec3 velocity, ActorReference shooterId, float damage,
                bool critical, ProjectilePickupState pickupState, ItemStack pickupItem,
                bool inGround, glm::ivec3 inBlockPos, std::uint32_t lifeTicks);

    // Advances every projectile one 20 TPS tick: physics for the ones still
    // flying, a per-tick raycast along the displacement for a fresh hit
    // (entity -> Damage.hpp + knockback + crit bonus; block -> stick), the
    // landed-arrow pickup contact test against the player, and the lifetime
    // despawn. `entities` is the world's creature pool the hit raycast tests
    // against (and the pipe the entity-hit damage is applied through);
    // `playerPosition`/`playerPresent` gate the player-contact pickup path the
    // same way ItemEntitySystem's magnet is gated by an inventory reference.
    //
    // RW-1a #7 — `pickupInventory`, when non-null, is where a collected
    // pickupItem is actually stowed: the projectile is consumed ONLY when the
    // stack fits (AbstractArrow#playerTouch's `player.getInventory().add(...)`
    // guard — a full backpack leaves the arrow on the ground to try again next
    // tick, never silently deleted). Any stacks that were stowed are still
    // returned so the caller can play the pickup sound. `pickupInventory ==
    // nullptr` keeps the RW-0 behaviour (collect + consume unconditionally,
    // returning the stacks for a caller that stows them itself) — useful for
    // tests that assert the raw pickup mechanic without an inventory.
    [[nodiscard]] std::vector<ItemStack> tick(const world::World& world, EntitySystem& entities,
                                              glm::vec3 playerPosition, bool playerPresent,
                                              world::gen::JavaRandom& rng,
                                              Inventory* pickupInventory = nullptr);

    [[nodiscard]] const std::vector<Projectile>& entities() const { return entities_; }

  private:
    std::vector<Projectile> entities_;
};

} // namespace mc::gameplay
