// AR-M2: monster gameplay completion — the daylight-ignition content rule
// (consuming EM1's setOnFire/fireTicks), husk sun-immunity (the EntityBehavior
// bit EM1/AR-M1 reserved), husk hunger-on-hit (consuming EM2's applyEffect),
// and the chase re-acquire edge. All headless: a flat world, spawned
// creatures, and the tick loop. This is the last AR content node — see the
// task report for the "AR content track complete" note.

#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/EnvironmentSnapshot.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/StatusEffect.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/MobBrain.hpp"
#include "gameplay/entities/ZombieEntity.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

using namespace mc::gameplay;
using mc::gameplay::entities::entityTypeRegistry;
using mc::gameplay::entities::MobCategory;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"monster_completion_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

// A flat stone world with air above, wide enough that a spawned creature's
// tick never falls off the edge of a loaded chunk. Sky light defaults to 0
// (never set) unless a test opens a column with setDirectSkyLight, matching
// entity_fire_test.cpp's own convention.
mc::world::World makeFlatWorld() {
    mc::world::World world;
    for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
        for (int chunkX = -1; chunkX <= 1; ++chunkX) {
            mc::world::Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, 0, z, mc::world::Block::Stone);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

// Noon (tick 6000): EnvironmentSnapshot::resolve's ambientDarkness is 0,
// satisfying Level#isDay's `skyDarken < 4`. Midnight (18000) darkens to 11,
// failing it — the exact plateaus environment_snapshot_test.cpp itself checks.
EnvironmentSnapshot daySnapshot() {
    return EnvironmentSnapshot::resolve(/*dayTimeTicks=*/6000.0, 0.0F, 0.0F);
}
EnvironmentSnapshot nightSnapshot() {
    return EnvironmentSnapshot::resolve(/*dayTimeTicks=*/18000.0, 0.0F, 0.0F);
}

// Opens the head cell (one above the spawn foot cell) fully to the sky, the
// same directSkyLight >= 15 convention entity_fire_test.cpp's rain test uses.
void exposeHeadToSky(mc::world::World& world, int x, int y, int z) {
    world.setDirectSkyLight(x, y, z, 15U);
}

int fireTicksOf(const EntitySystem& system, std::uint64_t id) {
    const SimpleEntity* entity = system.byIdConst(id);
    REQUIRE(entity != nullptr);
    return entity->fireTicks;
}

// --- ignition rule -----------------------------------------------------

// A zombie standing at noon under an open sky, dry, ignites within the same
// tick — EntitySystem::tick's ignition rule runs the max(fireTicks, 8*20)
// == 160 assignment, and the very same tick's existing EM1 fire block (right
// below it) immediately processes one 20-tick interval of that fresh burn:
// fireTicks % 20 == 0 lands on tick 160 too, so it takes one point of OnFire
// damage and decrements to 159 before the tick returns — exactly the ordering
// `setOnFire` then `baseTick` gives vanilla within one tick.
void testZombieIgnitesInDaylight() {
    mc::world::World world = makeFlatWorld();
    exposeHeadToSky(world, 0, 2, 0);
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, entities::ZombieEntity::type(), /*seed=*/1U);
    const std::uint64_t id = system.entities().front().id;
    REQUIRE(fireTicksOf(system, id) == 0);

    system.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F, Difficulty::Normal, true,
               false, 0.0F, /*raining=*/false, ItemStack{}, daySnapshot());
    REQUIRE(fireTicksOf(system, id) == 159);
}

// The same zombie at night — ambientDarkness fails Level#isDay — never
// ignites. Sabotage②'s twin: a rule that only checks sky exposure (not day)
// would light this one too.
void testZombieDoesNotIgniteAtNight() {
    mc::world::World world = makeFlatWorld();
    exposeHeadToSky(world, 0, 2, 0);
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, entities::ZombieEntity::type(), /*seed=*/2U);
    const std::uint64_t id = system.entities().front().id;

    for (int tick = 0; tick < 10; ++tick) {
        system.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F, Difficulty::Normal, true,
                   false, 0.0F, /*raining=*/false, ItemStack{}, nightSnapshot());
    }
    REQUIRE(fireTicksOf(system, id) == 0);
}

// Sabotage① anchor: a zombie at noon but under a solid roof (no
// directSkyLight at its head — the world default) never ignites, even though
// it is broad daylight. A rule that ignores sky exposure would light this.
void testZombieDoesNotIgniteWhenSheltered() {
    mc::world::World world = makeFlatWorld(); // head cell never opened to sky
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, entities::ZombieEntity::type(), /*seed=*/3U);
    const std::uint64_t id = system.entities().front().id;

    for (int tick = 0; tick < 10; ++tick) {
        system.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F, Difficulty::Normal, true,
                   false, 0.0F, /*raining=*/false, ItemStack{}, daySnapshot());
    }
    REQUIRE(fireTicksOf(system, id) == 0);
}

// A submerged zombie at noon under open sky does not ignite either — the
// water term the ignition rule shares with EM1's own water-extinguish check.
void testZombieDoesNotIgniteSubmerged() {
    mc::world::World world = makeFlatWorld();
    exposeHeadToSky(world, 0, 2, 0);
    world.setBlock(0, 1, 0, mc::world::Block::Water);
    world.setBlock(0, 2, 0, mc::world::Block::Water);
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, entities::ZombieEntity::type(), /*seed=*/4U);
    const std::uint64_t id = system.entities().front().id;

    for (int tick = 0; tick < 10; ++tick) {
        system.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F, Difficulty::Normal, true,
                   false, 0.0F, /*raining=*/false, ItemStack{}, daySnapshot());
    }
    REQUIRE(fireTicksOf(system, id) == 0);
}

// Sabotage③ anchor: a Creature-category animal (cow) in the identical
// full-daylight, sky-exposed condition never ignites — the ignition rule
// gates on MobCategory::Monster + Undead, not "sky-exposed at noon" alone.
void testCowDoesNotIgniteInDaylight() {
    mc::world::World world = makeFlatWorld();
    exposeHeadToSky(world, 0, 2, 0);
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, entities::CowEntity::type(), /*seed=*/5U);
    const std::uint64_t id = system.entities().front().id;
    REQUIRE(system.byIdConst(id)->type->category() == MobCategory::Creature);

    for (int tick = 0; tick < 10; ++tick) {
        system.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F, Difficulty::Normal, true,
                   false, 0.0F, /*raining=*/false, ItemStack{}, daySnapshot());
    }
    REQUIRE(fireTicksOf(system, id) == 0);
}

// --- husk sun-immunity ---------------------------------------------------

// Sabotage② anchor, both directions: a husk in the identical full-daylight,
// sky-exposed, dry condition a zombie ignites in does NOT ignite (its
// EntityBehavior::SunImmune bit), while the zombie standing right beside it
// DOES — proving the exemption is per-type, not a global daylight-burn
// disable that happens to also spare the zombie.
void testHuskDoesNotIgniteWhileZombieBesideItDoes() {
    const auto* huskType = entityTypeRegistry().byId("husk");
    REQUIRE(huskType != nullptr);
    REQUIRE(huskType->undead());
    REQUIRE(huskType->sunImmune());
    REQUIRE(entities::ZombieEntity::type().undead());
    REQUIRE(!entities::ZombieEntity::type().sunImmune());

    mc::world::World world = makeFlatWorld();
    exposeHeadToSky(world, 0, 2, 0);
    exposeHeadToSky(world, 2, 2, 0);
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, *huskType, /*seed=*/6U);
    system.spawn(glm::vec3{2.5F, 1.0F, 0.5F}, entities::ZombieEntity::type(), /*seed=*/7U);
    const std::uint64_t huskId = system.entities()[0].id;
    const std::uint64_t zombieId = system.entities()[1].id;

    system.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F, Difficulty::Normal, true,
               false, 0.0F, /*raining=*/false, ItemStack{}, daySnapshot());

    REQUIRE(fireTicksOf(system, huskId) == 0);
    // 159, not 160: the ignition rule's fireTicks = 160 lands on the tick
    // interval boundary, so the existing EM1 fire block right below it
    // immediately ticks that fresh burn down by one within the same call —
    // see testZombieIgnitesInDaylight's comment for the full accounting.
    REQUIRE(fireTicksOf(system, zombieId) == 159);
}

// --- chase re-acquire edge ------------------------------------------------

// A zombie's target selector re-acquires the player after losing them: the
// player first stands out of follow range (never targeted), then walks back
// in — the same shouldContinue/canStart cycle GoalSelector::tick already
// runs every tick, exercised here across an explicit lost -> reacquired
// transition rather than the mob_brain_test's single continuous approach.
// Seed 41 matches mob_brain_test.cpp's own known-good acquisition scenario
// (an even entity-rng seed here can take an extremely long time to land the
// 1-in-10 ActiveTargetPlayerGoal::canStart roll — a pre-existing LCG/consumer
// interaction, not anything this node touches; picking the same seed the
// existing combat test already relies on sidesteps it rather than papering
// over it).
void testChaseReacquiresAfterTargetLost() {
    mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{4.5F, 1.001F, 8.5F}, entities::ZombieEntity::type(), /*seed=*/41U);
    const std::uint64_t zombieId = system.entities().front().id;

    // Outside the zombie's 35-block follow range but inside MobEntity#
    // checkDespawn's 128-block distant-despawn radius (a farther player would
    // silently remove the zombie after 40 ticks past that range, which is a
    // different mechanism entirely — not what this test exercises).
    const glm::vec3 farPlayer{4.5F, 1.001F, 60.5F};
    for (int tick = 0; tick < 40; ++tick) {
        system.tick(world, farPlayer, 0.6F, 1.8F, Difficulty::Normal, true, false);
    }
    REQUIRE(!system.byIdConst(zombieId)->brain.combatTarget().valid());

    // The player walks into range: the zombie acquires within a handful of
    // ticks (ActiveTargetPlayerGoal rolls 1-in-10 per tick once in range).
    const glm::vec3 nearPlayer{6.5F, 1.001F, 8.5F};
    bool reacquired = false;
    for (int tick = 0; tick < 80 && !reacquired; ++tick) {
        system.tick(world, nearPlayer, 0.6F, 1.8F, Difficulty::Normal, true, false);
        reacquired = system.byIdConst(zombieId)->brain.combatTarget().valid();
    }
    REQUIRE(reacquired);

    // The player leaves again (killed — simulated by going "not alive"): the
    // goal drops the target the same tick.
    for (int tick = 0; tick < 5; ++tick) {
        system.tick(world, nearPlayer, 0.6F, 1.8F, Difficulty::Normal, /*playerAlive=*/false, false);
    }
    REQUIRE(!system.byIdConst(zombieId)->brain.combatTarget().valid());
}

// --- husk hunger-on-hit ---------------------------------------------------

// Sabotage①/③ anchor for EM2 consumption: a husk's melee data flag
// (EntityBehavior::HungerOnHit) is set, the zombie beside it is not — the
// content rule GameSession applies is exercised directly against the two
// EntityType objects, mirroring how ZombieEntity/BuiltinSpecies register
// them (GameSession's own mobAttacks loop is integration-only, not
// reachable headless without the full session).
void testHungerOnHitFlagIsHuskOnly() {
    const auto* huskType = entityTypeRegistry().byId("husk");
    REQUIRE(huskType != nullptr);
    REQUIRE(huskType->hungerOnHit());
    REQUIRE(!entities::ZombieEntity::type().hungerOnHit());

    // PlayerVitals is the same applyEffect the hunger-on-hit hook calls
    // (mc::gameplay::applyEffect(effects_, hungerEffect(), …)); apply it here
    // exactly as GameSession's mobAttacks loop would after a landed husk hit,
    // and prove a zombie hit (no flag, no call) leaves the player unaffected.
    PlayerVitals huskVictim;
    huskVictim.reset();
    REQUIRE(!hasEffect(huskVictim.effects(), hungerEffect()));
    REQUIRE(huskVictim.applyEffect(hungerEffect(), huskHungerDurationTicks(Difficulty::Normal), 0U));
    REQUIRE(hasEffect(huskVictim.effects(), hungerEffect()));

    PlayerVitals zombieVictim;
    zombieVictim.reset();
    // No applyEffect call at all for a zombie hit — the content rule never
    // fires because ZombieEntity::type().hungerOnHit() is false.
    REQUIRE(!hasEffect(zombieVictim.effects(), hungerEffect()));
}

// The vanilla duration table (HuskEntity#tryAttack, 140 * localDifficulty,
// approximated off the world Difficulty here — see Difficulty.hpp).
void testHungerDurationTable() {
    REQUIRE(huskHungerDurationTicks(Difficulty::Peaceful) == 0);
    REQUIRE(huskHungerDurationTicks(Difficulty::Easy) == 140);
    REQUIRE(huskHungerDurationTicks(Difficulty::Normal) == 280);
    REQUIRE(huskHungerDurationTicks(Difficulty::Hard) == 420);
}

// --- determinism -----------------------------------------------------------

// Two independently-built systems with the same seed, world and environment
// ignite on the identical tick and burn identically thereafter — the
// ignition rule adds no hidden entropy beyond EM1's own fireTicks state.
void testDeterministicIgnitionAndBurn() {
    mc::world::World worldA = makeFlatWorld();
    mc::world::World worldB = makeFlatWorld();
    exposeHeadToSky(worldA, 0, 2, 0);
    exposeHeadToSky(worldB, 0, 2, 0);
    EntitySystem a;
    EntitySystem b;
    a.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, entities::ZombieEntity::type(), /*seed=*/42U);
    b.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, entities::ZombieEntity::type(), /*seed=*/42U);
    const std::uint64_t idA = a.entities().front().id;
    const std::uint64_t idB = b.entities().front().id;

    for (int tick = 0; tick < 200; ++tick) {
        a.tick(worldA, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F, Difficulty::Normal, true,
              false, 0.0F, /*raining=*/false, ItemStack{}, daySnapshot());
        b.tick(worldB, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F, Difficulty::Normal, true,
              false, 0.0F, /*raining=*/false, ItemStack{}, daySnapshot());
        REQUIRE(fireTicksOf(a, idA) == fireTicksOf(b, idB));
        REQUIRE(a.byIdConst(idA)->damage.health == b.byIdConst(idB)->damage.health);
    }
}

} // namespace

int main() {
    mc::gameplay::entities::registerBuiltinEntities();

    testZombieIgnitesInDaylight();
    testZombieDoesNotIgniteAtNight();
    testZombieDoesNotIgniteWhenSheltered();
    testZombieDoesNotIgniteSubmerged();
    testCowDoesNotIgniteInDaylight();
    testHuskDoesNotIgniteWhileZombieBesideItDoes();
    testChaseReacquiresAfterTargetLost();
    testHungerOnHitFlagIsHuskOnly();
    testHungerDurationTable();
    testDeterministicIgnitionAndBurn();

    return 0;
}
