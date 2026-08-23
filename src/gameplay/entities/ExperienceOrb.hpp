#pragma once

// XP-1: the experience orb — the first non-living pickup entity. It is a
// dense value-type pool (position/velocity/value/age/pickupDelay), tick-driven
// like ItemEntitySystem's drops, and deliberately NOT folded into MobEntity:
// an orb has no AI, no health, no equipment, none of the columns a Mob
// carries (XP-DESIGN.md §3 / XP-experience/REGULAR.md #6).
//
// Ports 26.1 net.minecraft.world.entity.ExperienceOrb:
//   - gravity 0.03/tick (vs. an item's 0.04), 0.98 air drag, underwater float.
//   - followNearbyPlayer: within 8 blocks of the nearest ALIVE player, orbs
//     accelerate toward them (aim point = player position + eye height/2);
//     acceleration scales with `(1 - sqrt(distSq)/8)^2 * 0.1` (ExperienceOrb.
//     followNearbyPlayer). Contact radius reuses the item pickup's near-zero
//     tolerance the way award()/playerTouch's bounding-box touch does.
//   - scanForMerges: same-value orbs within a small radius merge their count
//     (not their value — a merged orb is N stacked orbs of the same
//     denomination, matching ExperienceOrb.merge's `count += other.count`).
//   - award()/getExperienceValue(): a requested amount splits into vanilla's
//     fixed denominations (1,2,3,4,5,6,7,8,...,3,7,17,37,73,149,307,617,
//     1237,2477 climbing sequence — see getExperienceValue's descending
//     table), each landing as one orb (or merging into an existing one).
//   - age >= 6000 ticks (5 minutes) despawns.
//
// Determinism (REGULAR.md / XP-experience/REGULAR.md #3): the initial
// scatter velocity and the world position, when unstuck-from-solid, come
// from a caller-supplied JavaRandom stream — never the wall clock or a
// global RNG — so spawnExperienceOrbs replays identically for the same seed
// and call sequence.
#include "gameplay/Inventory.hpp"
#include "world/gen/JavaRandom.hpp"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::world {
class World;
}

namespace mc::gameplay {

class PlayerExperience;

// One experience orb. A plain value type — no owning pointers, no vtable —
// so the pool below is a flat std::vector the tick walks by index, matching
// ItemEntitySystem's ItemEntity precedent exactly.
struct ExperienceOrb final {
    glm::vec3 position{0.0F};
    glm::vec3 previousPosition{0.0F};
    glm::vec3 velocity{0.0F};
    // The denomination this orb carries (ExperienceOrb.DATA_VALUE), always one
    // of the fixed award() values.
    std::int32_t value = 0;
    // How many orbs are stacked into this one record (ExperienceOrb.count):
    // scanForMerges folds same-value neighbours into a single instance instead
    // of keeping N separate entities, the way identical item drops group.
    std::int32_t count = 1;
    std::uint32_t ageTicks = 0U;
    // ExperienceOrb.playerTouch: `takeXpDelay` gates pickup to at most once
    // every couple of ticks per orb, and also serves as the immediate-pickup
    // guard for a freshly spawned orb (vanilla does not zero-delay orbs the
    // way item drops get a 10-tick untouchable window, but XP-2's future
    // sources spawn many orbs in one call — a same-tick pickupDelay keeps a
    // orb from being touched before its own spawn call finishes placing it).
    std::uint32_t pickupDelayTicks = 0U;

    [[nodiscard]] friend bool operator==(const ExperienceOrb&, const ExperienceOrb&) = default;
};

class ExperienceOrbSystem final {
  public:
    // Places one orb of exactly `value` points (no denomination splitting —
    // that is spawnExperienceOrbs' job below). `rng` supplies the scatter
    // velocity and yaw, matching ExperienceOrb's constructor
    // (`random.nextDouble() * 0.2 - 0.1) * 2.0` etc.) bit for bit against a
    // caller-owned deterministic stream.
    void spawnOne(glm::vec3 position, std::int32_t value, world::gen::JavaRandom& rng);

    // 26.1 ExperienceOrb.award/awardWithDirection + getExperienceValue: splits
    // `amount` into the vanilla fixed denominations, each landing as its own
    // spawnOne() call (or merging into an existing same-tick orb the way
    // tryMergeToExisting does — approximated here by the ordinary scanForMerges
    // pass that runs every tick, since XP-1 does not yet need the same-call
    // "search a random group id" optimisation vanilla uses to keep spawning
    // O(log amount) orbs cheap when many are dropped in one place).
    void spawnMany(glm::vec3 position, std::int32_t amount, world::gen::JavaRandom& rng);

    // Reinstates an orb from a save, keeping its age/count/pickupDelay — spawnOne
    // would reset those, and age drives both despawn and (indirectly) merge
    // priority (merge() keeps the smaller of the two ages).
    void restore(glm::vec3 position, glm::vec3 velocity, std::int32_t value, std::int32_t count,
                std::uint32_t ageTicks, std::uint32_t pickupDelayTicks);

    // Advances one tick: physics, the 8-block player magnet, same-value merge,
    // contact pickup (adds to `experience` and removes the orb) and the
    // 6000-tick despawn. Returns the total points collected this tick (0 when
    // nothing was picked up), so the caller can gate a pickup sound the way
    // ItemEntitySystem::tick's return value gates ItemPickup.
    [[nodiscard]] std::int32_t tick(const world::World& world, glm::vec3 playerPosition,
                                    bool playerAlive, PlayerExperience& experience);

    [[nodiscard]] const std::vector<ExperienceOrb>& entities() const { return entities_; }

  private:
    std::vector<ExperienceOrb> entities_;
};

// 26.1 ExperienceOrb.getExperienceValue: the largest fixed denomination not
// exceeding `amount`. A pure function so spawnMany's split loop and any
// future test can call it standalone without a system instance.
[[nodiscard]] std::int32_t experienceOrbDenomination(std::int32_t amount);

} // namespace mc::gameplay
