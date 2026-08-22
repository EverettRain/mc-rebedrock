// EM1: the fire/burning mechanism — fireTicks, the OnFire/InFire/Lava damage
// types, setSecondsOnFire, water/rain extinguishing, and the EntityType
// behaviour bit that exempts a species. All headless: a flat world, a spawned
// creature, and the tick loop.

#include "gameplay/Damage.hpp"
#include "gameplay/DamageType.hpp"
#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/MobBrain.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

using namespace mc::gameplay;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"entity_fire_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

[[nodiscard]] bool nearly(float value, float expected) {
    return std::fabs(value - expected) < 0.001F;
}

// A flat stone world with air above, so a spawned creature stands on ground and
// is not in a fluid. Wide enough that the tick's simulation gate (disabled here)
// and neighbour lookups always resolve to a loaded chunk.
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

// A minimal AI so a locally-built EntityType is complete. The fire mechanism
// runs in baseTick, before any Goal, so an idle brain is enough.
class IdleAi final : public entities::EntityAi {
  public:
    void configureBrain(entities::MobBrain&) const override {}
};

const IdleAi kIdleAi;

// A fire-immune species built purely for the test — the mechanism must skip it
// on the strength of the behaviour bit alone, with no `if (species == …)`. This
// is exactly how a nether native will register once one exists.
const entities::EntityType& fireImmuneType() {
    static const entities::EntityType type =
        entities::EntityType::Builder::create(entities::MobCategory::Creature, kIdleAi)
            .sized(0.9F, 0.9F)
            .health(20.0F)
            .fireImmune()
            .build("test_fire_immune");
    return type;
}

// A plain, non-immune species, likewise local so the test owns its health cap.
const entities::EntityType& mortalType() {
    static const entities::EntityType type =
        entities::EntityType::Builder::create(entities::MobCategory::Creature, kIdleAi)
            .sized(0.9F, 0.9F)
            .health(20.0F)
            .build("test_mortal");
    return type;
}

// The tick that only advances the world entities, at the ground under a creature
// standing well inside a single chunk. No player pusher (default), no simulation
// gate (radius 0), so nothing is frozen.
EntityTickResult tickEntities(EntitySystem& system, const mc::world::World& world,
                              bool raining = false) {
    return system.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
                        Difficulty::Normal, true, false, 0.0F, raining);
}

// The three fire damage types exist, carry IS_FIRE, and only lava costs hunger —
// the mechanical facts EM1 adds to the table.
void testDamageTypeTable() {
    REQUIRE(hasDamageTag(DamageType::OnFire, DamageTag::IsFire));
    REQUIRE(hasDamageTag(DamageType::InFire, DamageTag::IsFire));
    REQUIRE(hasDamageTag(DamageType::Lava, DamageTag::IsFire));
    // Fire is reduced by armor, unlike the world's other sources, so it must not
    // carry BypassesArmor.
    REQUIRE(!hasDamageTag(DamageType::OnFire, DamageTag::BypassesArmor));
    REQUIRE(!hasDamageTag(DamageType::Lava, DamageTag::BypassesArmor));
    // The burn tick does not shove; lava contact and standing in fire are
    // resolved as ordinary hits.
    REQUIRE(hasDamageTag(DamageType::OnFire, DamageTag::NoKnockback));
    // Exhaustion: only lava charges the 0.1 a living attacker's swing does.
    REQUIRE(nearly(damageTypeData(DamageType::OnFire).exhaustion, 0.0F));
    REQUIRE(nearly(damageTypeData(DamageType::InFire).exhaustion, 0.0F));
    REQUIRE(nearly(damageTypeData(DamageType::Lava).exhaustion, 0.1F));
    // The vanilla message ids travel with the data.
    REQUIRE(damageTypeData(DamageType::OnFire).msgId == "onFire");
    REQUIRE(damageTypeData(DamageType::Lava).msgId == "lava");
    // A pre-existing type is untouched by the three new rows.
    REQUIRE(damageTypeData(DamageType::Fall).msgId == "fall");
}

// setOnFire(5) burns for a hundred ticks and deals exactly five points, one per
// second, then stops — the vanilla number. This is the sabotage-2 anchor: if the
// burn skips the interval throttle the total exceeds five.
void testBurnDealsOnePointPerSecond() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, mortalType(), /*seed=*/1U);
    const std::uint64_t id = system.entities().front().id;

    REQUIRE(system.setOnFire(id, 5));
    REQUIRE(system.byId(id)->fireTicks == 100);
    const float startHealth = system.byId(id)->damage.health;

    // Run the whole burn out. Health drops one per second (twenty ticks) for
    // five seconds, then holds.
    float previousHealth = startHealth;
    int burnDamageEvents = 0;
    for (int tick = 0; tick < 130; ++tick) {
        tickEntities(system, world);
        const SimpleEntity* entity = system.byId(id);
        REQUIRE(entity != nullptr);
        if (entity->damage.health < previousHealth - 0.001F) {
            ++burnDamageEvents;
            // Each burn event is exactly one point.
            REQUIRE(nearly(previousHealth - entity->damage.health, 1.0F));
            previousHealth = entity->damage.health;
        }
    }
    REQUIRE(burnDamageEvents == 5);
    REQUIRE(nearly(startHealth - system.byId(id)->damage.health, 5.0F));
    // The fire has gone out on its own.
    REQUIRE(system.byId(id)->fireTicks == 0);
    // The killing damage type, had it been fatal, is OnFire — checked directly
    // below with a low-health entity.
}

// A creature that dies to fire records OnFire as its last damage source and
// leaves loot exactly once, through the same die() path fall damage uses.
void testFireDeathSource() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, mortalType(), /*seed=*/2U);
    const std::uint64_t id = system.entities().front().id;
    // Knock it down to two hearts so the burn kills it quickly.
    SimpleEntity* entity = system.byId(id);
    entity->damage.health = 2.0F;

    REQUIRE(system.setOnFire(id, 10));
    bool died = false;
    for (int tick = 0; tick < 60 && !died; ++tick) {
        tickEntities(system, world);
        const SimpleEntity* live = system.byId(id);
        if (live == nullptr || live->damage.dead()) {
            died = true;
        }
    }
    REQUIRE(died);
    // The creature is still resolvable during its death animation; its last
    // source is the burn.
    const SimpleEntity* corpse = system.byId(id);
    REQUIRE(corpse != nullptr);
    REQUIRE(corpse->damage.lastSource == DamageType::OnFire);
}

// Standing in water puts the fire out the same tick and stops the burn — the
// sabotage-3 anchor.
void testWaterExtinguishes() {
    mc::world::World world = makeFlatWorld();
    // A water column where the creature stands.
    world.setBlock(0, 1, 0, mc::world::Block::Water);
    world.setBlock(0, 2, 0, mc::world::Block::Water);

    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, mortalType(), /*seed=*/3U);
    const std::uint64_t id = system.entities().front().id;
    REQUIRE(system.setOnFire(id, 10));
    REQUIRE(system.byId(id)->fireTicks == 200);
    const float startHealth = system.byId(id)->damage.health;

    tickEntities(system, world);
    // The very first tick in water snuffs it before any burn damage.
    REQUIRE(system.byId(id)->fireTicks == 0);
    REQUIRE(nearly(system.byId(id)->damage.health, startHealth));
}

// Rain under open sky puts the fire out; rain with no sky above does not (the
// creature is sheltered). The world's directSkyLight drives the "open sky" test.
void testRainExtinguishes() {
    mc::world::World open = makeFlatWorld();
    // Sky reaches the creature's head cell.
    open.setDirectSkyLight(0, 2, 0, 15U);
    EntitySystem exposed;
    exposed.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, mortalType(), /*seed=*/4U);
    const std::uint64_t exposedId = exposed.entities().front().id;
    REQUIRE(exposed.setOnFire(exposedId, 10));
    tickEntities(exposed, open, /*raining=*/true);
    REQUIRE(exposed.byId(exposedId)->fireTicks == 0);

    // Sheltered: no sky above the head cell, so rain does not reach it.
    mc::world::World sheltered = makeFlatWorld();
    sheltered.setDirectSkyLight(0, 2, 0, 0U);
    EntitySystem covered;
    covered.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, mortalType(), /*seed=*/5U);
    const std::uint64_t coveredId = covered.entities().front().id;
    REQUIRE(covered.setOnFire(coveredId, 10));
    tickEntities(covered, sheltered, /*raining=*/true);
    REQUIRE(covered.byId(coveredId)->fireTicks > 0);
}

// A fire-immune species never catches, and never takes burn damage — the
// sabotage-1 anchor. The mechanism reads the behaviour bit, not the species.
void testFireImmunity() {
    REQUIRE(fireImmuneType().fireImmune());
    REQUIRE(!fireImmuneType().sunImmune());
    REQUIRE(!mortalType().fireImmune());

    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, fireImmuneType(), /*seed=*/6U);
    const std::uint64_t id = system.entities().front().id;
    const float startHealth = system.byId(id)->damage.health;

    // setOnFire refuses to light it, and reports it is not ablaze.
    REQUIRE(!system.setOnFire(id, 10));
    REQUIRE(system.byId(id)->fireTicks == 0);
    for (int tick = 0; tick < 60; ++tick) {
        tickEntities(system, world);
    }
    REQUIRE(nearly(system.byId(id)->damage.health, startHealth));
}

// setOnFire only ever lengthens a burn, exactly like vanilla's max().
void testSetOnFireTakesMax() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, mortalType(), /*seed=*/7U);
    const std::uint64_t id = system.entities().front().id;
    REQUIRE(system.setOnFire(id, 5));
    REQUIRE(system.byId(id)->fireTicks == 100);
    // A shorter request cannot shorten a longer burn.
    REQUIRE(system.setOnFire(id, 2));
    REQUIRE(system.byId(id)->fireTicks == 100);
    // A longer one extends it.
    REQUIRE(system.setOnFire(id, 8));
    REQUIRE(system.byId(id)->fireTicks == 160);
    // A non-positive request is a no-op.
    REQUIRE(system.setOnFire(id, 0));
    REQUIRE(system.byId(id)->fireTicks == 160);
}

// The burn is deterministic: two systems with the same seed and the same
// ignition burn identically, tick for tick.
void testDeterministicBurn() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem a;
    EntitySystem b;
    a.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, mortalType(), /*seed=*/42U);
    b.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, mortalType(), /*seed=*/42U);
    const std::uint64_t idA = a.entities().front().id;
    const std::uint64_t idB = b.entities().front().id;
    REQUIRE(a.setOnFire(idA, 5));
    REQUIRE(b.setOnFire(idB, 5));
    for (int tick = 0; tick < 120; ++tick) {
        tickEntities(a, world);
        tickEntities(b, world);
        REQUIRE(a.byId(idA)->fireTicks == b.byId(idB)->fireTicks);
        REQUIRE(nearly(a.byId(idA)->damage.health, b.byId(idB)->damage.health));
    }
}

// The player half: the same state machine on PlayerVitals. setOnFire lights,
// the burn costs one point per second through the shared pipeline (OnFire), and
// water/rain put it out.
void testPlayerFire() {
    PlayerVitals vitals;
    vitals.reset();
    vitals.setOnFire(5);
    REQUIRE(vitals.fireTicks() == 100);

    VitalsInput dry{};
    dry.onGround = true;
    const float start = vitals.health();
    int burnEvents = 0;
    float previous = start;
    for (int tick = 0; tick < 130; ++tick) {
        const VitalsTickResult outcome = vitals.tick(dry);
        if (outcome.cause == DamageType::OnFire) {
            ++burnEvents;
        }
        if (vitals.health() < previous - 0.001F) {
            previous = vitals.health();
        }
    }
    // Five seconds, five points (natural regeneration is off at full food only
    // when hurt; here regen may claw some back, so assert the burn *events*).
    REQUIRE(burnEvents == 5);
    REQUIRE(vitals.fireTicks() == 0);

    // Water puts it out with no burn.
    PlayerVitals wet;
    wet.reset();
    wet.setOnFire(10);
    VitalsInput inWater{};
    inWater.inWater = true;
    static_cast<void>(wet.tick(inWater));
    REQUIRE(wet.fireTicks() == 0);

    // Rain under open sky puts it out.
    PlayerVitals rained;
    rained.reset();
    rained.setOnFire(10);
    VitalsInput underRain{};
    underRain.onGround = true;
    underRain.rainedOn = true;
    static_cast<void>(rained.tick(underRain));
    REQUIRE(rained.fireTicks() == 0);

    // A respawn clears any lingering burn.
    PlayerVitals respawned;
    respawned.setOnFire(10);
    respawned.reset();
    REQUIRE(respawned.fireTicks() == 0);
}

} // namespace

int main() {
    testDamageTypeTable();
    testBurnDealsOnePointPerSecond();
    testFireDeathSource();
    testWaterExtinguishes();
    testRainExtinguishes();
    testFireImmunity();
    testSetOnFireTakesMax();
    testDeterministicBurn();
    testPlayerFire();
    return 0;
}
