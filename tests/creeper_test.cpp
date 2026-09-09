// EXP-3: the creeper — the mob whose whole behaviour is an explosion.
//
// What is worth testing here is NOT "does a creeper exist". It is the three
// things that make a creeper a creeper rather than a zombie with a bigger
// number attached:
//
//   1. the fuse is INTEGRATED from a level, not triggered by an event, so
//      backing away winds it back DOWN rather than merely pausing it;
//   2. the blast is raised at the creature's FEET (Creeper#explodeCreeper uses
//      getY(), not the eye or the box centre) — the same half-block that
//      EXP-2 got wrong for TNT and had to correct after a field report that
//      the crater reached too far;
//   3. it is DISCARDED, not killed — no loot, no experience, no death sound.
//
// Plus the mob_griefing gate, which is the observable difference between
// ExplosionInteraction.MOB and the player-lit TNT beside it.

#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <variant>
#include <string>

using namespace mc;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"creeper_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

struct TestHost final : gameplay::SimulationHost {
    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
    void playBlockHit(world::Block, glm::vec3) override {}
    void playBlockPlace(world::Block, glm::vec3) override {}
    void playItemBreak(glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playExplode(glm::vec3) override {}
    void playCreatureHurt(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void spawnWaterSplash(glm::vec3) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

// A 3x3-chunk stone floor at y=0..8, so a blast at the surface has material
// under and beside it and nothing above.
[[nodiscard]] world::World makeFlatWorld() {
    world::World world;
    for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
        for (int chunkX = -1; chunkX <= 1; ++chunkX) {
            world::Chunk chunk;
            for (int y = 0; y <= 8; ++y) {
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        chunk.setBlock(x, y, z, world::Block::Stone);
                    }
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

[[nodiscard]] const gameplay::entities::EntityType& creeperType() {
    const auto* type = gameplay::entities::entityTypeRegistry().byId("creeper");
    if (type == nullptr) {
        throw std::runtime_error{"creeper species not registered"};
    }
    return *type;
}

// --- 1) the manifest row -------------------------------------------------

// Creeper.createAttributes() is Monster.createMonsterAttributes() plus
// MOVEMENT_SPEED 0.25; EntityType.CREEPER is sized(0.6, 1.7). Exact values, not
// ranges: a range cannot tell 0.25 from the zombie's 0.23, which is the one
// number Creeper.java actually overrides.
void testManifestRowMatchesVanilla() {
    const auto& type = creeperType();
    REQUIRE(type.category() == gameplay::entities::MobCategory::Monster);
    REQUIRE(type.attributes().get(gameplay::entities::Attribute::MaxHealth) == 20.0F);
    REQUIRE(type.attributes().get(gameplay::entities::Attribute::MovementSpeed) == 0.25F);
    REQUIRE(type.attributes().get(gameplay::entities::Attribute::AttackDamage) == 2.0F);
    REQUIRE(type.attributes().get(gameplay::entities::Attribute::FollowRange) == 16.0F);
    REQUIRE(type.dimensions().width == 0.6F);
    REQUIRE(type.dimensions().height == 1.7F);
    REQUIRE(type.xpReward() == 5 && type.xpRewardMax() == 5);
    REQUIRE(type.hasSpawnEgg());
    // A creeper is not undead: it does not burn in daylight and Smite does not
    // hit it. The husk beside it in the manifest carries both bits, so this is
    // a real distinction between two rows of the same table, not a tautology.
    REQUIRE(!type.undead());
    REQUIRE(!type.sunImmune());
    REQUIRE(type.behaviorFlags() == 0U);
}

// creeper.json: 0-2 gunpowder, uniformly. Asserted as "every count in 0..2
// occurs and nothing outside it does" over a long run — a `count <= 2` check
// alone would pass a table that always drops zero.
void testLootIsZeroToTwoGunpowder() {
    const auto& type = creeperType();
    bool sawZero = false;
    bool sawOne = false;
    bool sawTwo = false;
    std::uint64_t rng = 4242U;
    for (int roll = 0; roll < 400; ++roll) {
        const auto drops = type.rollLoot(rng);
        if (drops.view().empty()) {
            sawZero = true;
            continue;
        }
        REQUIRE(drops.view().size() == 1U);
        REQUIRE(drops.view()[0].item == &gameplay::items::Gunpowder);
        const auto count = drops.view()[0].count;
        REQUIRE(count == 1U || count == 2U);
        sawOne = sawOne || count == 1U;
        sawTwo = sawTwo || count == 2U;
    }
    REQUIRE(sawZero && sawOne && sawTwo);
}

// --- 2) the fuse ---------------------------------------------------------

// Drives an EntitySystem tick with the player standing at `player`, which is
// what makes ActiveTargetPlayerGoal acquire and SwellGoal see a target.
void tickWithPlayerAt(gameplay::EntitySystem& entities, const world::World& world,
                      glm::vec3 player) {
    static_cast<void>(entities.tick(world, player, 0.6F, 1.8F, gameplay::Difficulty::Normal, true,
                                    false, 0.0F));
}

// A creeper standing next to the player winds its fuse up one per tick and
// detonates on exactly the 30th — vanilla's maxSwell. Asserted as an exact tick
// count from a known start, so a fuse that ran at two per tick or that fired
// early would fail; "it eventually explodes" would not.
void testFuseReachesThirtyThenDetonates() {
    const world::World world = makeFlatWorld();
    gameplay::EntitySystem entities;
    const glm::vec3 player{8.5F, 9.0F, 8.5F};
    entities.spawn({10.0F, 9.0F, 8.5F}, creeperType(), 3U);
    const std::uint64_t id = entities.entities().front().id;

    // Let the target goal acquire and the swell goal start.
    int warmup = 0;
    while (!entities.byId(id)->brain.swelling() && warmup < 40) {
        tickWithPlayerAt(entities, world, player);
        ++warmup;
    }
    REQUIRE(entities.byId(id)->brain.swelling());
    const int swellAtStart = entities.byId(id)->swell;
    REQUIRE(swellAtStart < gameplay::entities::kCreeperMaxSwell);

    // From here it is exactly (30 - swell) more ticks, one per tick.
    const int remaining = gameplay::entities::kCreeperMaxSwell - swellAtStart;
    for (int step = 1; step < remaining; ++step) {
        const auto result = entities.tick(world, player, 0.6F, 1.8F,
                                          gameplay::Difficulty::Normal, true, false, 0.0F);
        REQUIRE(result.detonations.empty());
        REQUIRE(entities.byId(id) != nullptr);
        REQUIRE(entities.byId(id)->swell == swellAtStart + step);
    }
    const glm::vec3 feet = entities.byId(id)->position;
    const auto blast = entities.tick(world, player, 0.6F, 1.8F, gameplay::Difficulty::Normal,
                                     true, false, 0.0F);
    REQUIRE(blast.detonations.size() == 1U);
    REQUIRE(blast.detonations.front().radius == gameplay::entities::kCreeperExplosionRadius);
    // Creeper#explodeCreeper: getX()/getY()/getZ() — the FEET. Exact equality on
    // y, so a centre lifted to the box middle (a plausible-looking
    // `+ height * 0.5F`) fails here rather than silently widening every crater.
    REQUIRE(blast.detonations.front().center.x == feet.x);
    REQUIRE(blast.detonations.front().center.y == feet.y);
    REQUIRE(blast.detonations.front().center.z == feet.z);
    // Discarded, not killed: gone from the world, and it rolled no loot and no
    // experience on the way out.
    REQUIRE(entities.byId(id) == nullptr);
    REQUIRE(entities.pendingDrops().empty());
}

// Walking out of range winds the fuse back DOWN. This is the assertion that
// separates "swelling is a level the goal writes every tick" from "swelling is
// an event that latches": a latched fuse would hold its value (or keep
// counting) once the player left.
void testBackingAwayWindsTheFuseDown() {
    const world::World world = makeFlatWorld();
    gameplay::EntitySystem entities;
    const glm::vec3 near{8.5F, 9.0F, 8.5F};
    entities.spawn({10.0F, 9.0F, 8.5F}, creeperType(), 3U);
    const std::uint64_t id = entities.entities().front().id;

    int warmup = 0;
    while (entities.byId(id)->swell < 5 && warmup < 60) {
        tickWithPlayerAt(entities, world, near);
        ++warmup;
    }
    const int peak = entities.byId(id)->swell;
    REQUIRE(peak >= 5);

    // Now stand 20 blocks away — well past the 7-block give-up radius.
    const glm::vec3 far{28.5F, 9.0F, 8.5F};
    int previous = peak;
    for (int step = 0; step < peak; ++step) {
        const auto result = entities.tick(world, far, 0.6F, 1.8F, gameplay::Difficulty::Normal,
                                          true, false, 0.0F);
        REQUIRE(result.detonations.empty());
        const int now = entities.byId(id)->swell;
        REQUIRE(now == previous - 1 || (previous == 0 && now == 0));
        previous = now;
    }
    REQUIRE(entities.byId(id)->swell == 0);
    REQUIRE(entities.byId(id) != nullptr); // it walked it off and lived
}

// --- 3) the mob_griefing gate -------------------------------------------

// The rule itself: 26.1 renamed it to snake_case along with the rest, and it
// defaults on.
void testMobGriefingRuleIsRegistered() {
    REQUIRE(gameplay::gameRuleIdFromName("mob_griefing") == gameplay::GameRuleId::MobGriefing);
    const auto& definition = gameplay::kGameRuleDefinitions[static_cast<std::size_t>(
        gameplay::GameRuleId::MobGriefing)];
    REQUIRE(definition.name == "mob_griefing");
    REQUIRE(definition.type == gameplay::GameRuleType::Boolean);
    REQUIRE(std::get<bool>(definition.defaultValue));
}

// ExplosionInteraction.MOB: with the rule off the crater is not dug, but the
// player is still hurt. Both halves asserted from the SAME blast, so a change
// that turned the whole explosion off would fail the damage half.
void testMobGriefingGatesTheCraterButNotTheDamage() {
    TestHost host;
    for (const bool griefing : {true, false}) {
        world::World world = makeFlatWorld();
        gameplay::GameSession session;
        session.setGameMode(gameplay::GameMode::Survival);
        session.player().setPosition({8.5F, 9.0F, 8.5F});
        REQUIRE(session.gameRules().set<bool>(gameplay::GameRuleId::MobGriefing, griefing));

        const float healthBefore = session.vitals().health();
        // Straight to the fuse's end: spawn the creeper beside the player and
        // hand it a swell one short of maxSwell, so the very next session tick
        // raises the blast through the real GameSession path.
        session.worldEntities().spawn({9.5F, 9.0F, 8.5F}, creeperType(), 5U);
        auto* creeper = session.worldEntities().byId(
            session.worldEntities().entities().front().id);
        creeper->swell = gameplay::entities::kCreeperMaxSwell - 1;
        creeper->brain.setSwelling(true);
        session.tick(world, host);

        // The floor directly under the creeper: dug when griefing is on, intact
        // when it is off.
        const bool floorGone = world.block(9, 8, 8) == world::Block::Air;
        REQUIRE(floorGone == griefing);
        // Hurt either way — a creeper with mob_griefing off is not harmless.
        REQUIRE(session.vitals().health() < healthBefore);
    }
}

} // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();
    testManifestRowMatchesVanilla();
    testLootIsZeroToTwoGunpowder();
    testFuseReachesThirtyThenDetonates();
    testBackingAwayWindsTheFuseDown();
    testMobGriefingRuleIsRegistered();
    testMobGriefingGatesTheCraterButNotTheDamage();
    return 0;
}
