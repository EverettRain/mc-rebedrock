// AR-A4: chicken gameplay completion — egg-laying (EntitySystem's shared
// laysEggs() scheduler, chicken's own EggLayProfile), the breeding parameters
// handed to EM-3 (tempt = wheat seeds, baby = chick), tempt following, and
// fall-immunity (EntityType::fallImmune(), the EntityBehavior family EM1
// built). Headless, no Vulkan.
//
// EM-3 itself (the age/love/breed state machine) is already covered by
// aging_breeding_test.cpp with a synthetic species; this file only proves the
// *chicken* manifest row wires into it correctly and that AR-A4's own new
// content (egg timer, fall immunity) behaves.

#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

using namespace mc;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"chicken_content_test line " + std::to_string(line) +
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

void buildStoneFloor(world::World& world) {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
}

// A flat stone world wide enough that a fall from height and normal wander
// never leave a loaded chunk, matching entity_fire_test.cpp's makeFlatWorld.
world::World makeWideFlatWorld() {
    world::World world;
    for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
        for (int chunkX = -1; chunkX <= 1; ++chunkX) {
            world::Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, 0, z, world::Block::Stone);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

[[nodiscard]] const gameplay::entities::EntityType& chickenType() {
    const auto* type = gameplay::entities::entityTypeRegistry().byId("chicken");
    if (type == nullptr) {
        throw std::runtime_error{"chicken species not registered"};
    }
    return *type;
}

// Spawns one chicken and returns its stable id.
[[nodiscard]] std::uint64_t spawnChicken(gameplay::GameSession& session, glm::vec3 position,
                                         std::uint32_t seed = 1U) {
    session.worldEntities().spawn(position, chickenType(), seed);
    return session.worldEntities().entities().back().id;
}

// A single right-click-on-entity: press (UseItemOn), tick once so performUse
// actually runs, then release (UseItemStop) and tick a few more times so
// `using_` clears and the vanilla-mirroring rightClickDelay has fully
// elapsed — see cow_content_test.cpp's identical helper.
void useOnEntity(gameplay::GameSession& session, world::World& world, gameplay::SimulationHost& host,
                 std::uint64_t entityId) {
    gameplay::UseItemOn use;
    use.entity = true;
    use.entityId = entityId;
    session.enqueueCommand(use);
    session.tick(world, host);
    session.enqueueCommand(gameplay::UseItemStop{});
    for (int tick = 0; tick < 5; ++tick) {
        session.tick(world, host);
    }
}

// --- egg-laying ---

// Sabotage anchor ①'s target: the countdown reaches zero, drops exactly one
// Egg, and rerolls into vanilla's 6000-12000 tick window — not a constant, not
// re-firing every tick.
void testEggLayDropsExactlyOneEggAndRerolls() {
    world::World world = makeWideFlatWorld();
    gameplay::EntitySystem entities;
    entities.spawn({8.0F, 1.001F, 8.0F}, chickenType(), 7U);
    const std::uint64_t id = entities.entities().front().id;

    // A freshly spawned chicken already rolled a 6000-12000 first countdown
    // (EntitySystem::spawn) off its own rng stream.
    REQUIRE(entities.byId(id)->eggLayTimer >= gameplay::kEggLayBaseTicks);
    REQUIRE(entities.byId(id)->eggLayTimer <
            gameplay::kEggLayBaseTicks + gameplay::kEggLayRandomTicks);

    // Force the timer to the brink so the very next tick lays.
    entities.byId(id)->eggLayTimer = 1;
    entities.tick(world);

    REQUIRE(entities.pendingDrops().size() == 1U);
    const auto& [position, drops] = entities.pendingDrops().front();
    static_cast<void>(position);
    REQUIRE(drops.view().size() == 1U);
    REQUIRE(drops.view()[0].item == &gameplay::items::Egg);
    REQUIRE(drops.view()[0].count == 1U);

    // The reroll landed back inside vanilla's 6000-12000 window.
    const int rerolled = entities.byId(id)->eggLayTimer;
    REQUIRE(rerolled >= gameplay::kEggLayBaseTicks);
    REQUIRE(rerolled < gameplay::kEggLayBaseTicks + gameplay::kEggLayRandomTicks);

    // One more tick at a large remaining timer must not lay again — a single
    // egg per interval, not one per tick (sabotage anchor ①'s other half).
    entities.clearPendingDrops();
    entities.tick(world);
    REQUIRE(entities.pendingDrops().empty());

    // Sabotage anchor ①'s "reset to a fixed constant" half: drive a second,
    // differently-seeded chicken through the identical brink-then-lay
    // sequence. A true reroll draws a fresh value off its own (now-diverged)
    // rng stream, so the two post-lay timers differ; a reset to a constant
    // makes them identical regardless of seed.
    gameplay::EntitySystem others;
    others.spawn({8.0F, 1.001F, 8.0F}, chickenType(), 8U);
    const std::uint64_t otherId = others.entities().front().id;
    others.byId(otherId)->eggLayTimer = 1;
    others.tick(world);
    REQUIRE(others.pendingDrops().size() == 1U);
    REQUIRE(others.byId(otherId)->eggLayTimer != rerolled);
}

// Two chickens seeded identically roll the same first countdown; two chickens
// seeded differently do not (with overwhelming probability) — the countdown
// draws off the entity's own deterministic rng stream, not wall-clock or a
// shared generator.
void testEggLayTimerDeterministic() {
    world::World world = makeWideFlatWorld();

    gameplay::EntitySystem first;
    first.spawn({8.0F, 1.001F, 8.0F}, chickenType(), 99U);
    const int firstTimer = first.entities().front().eggLayTimer;

    gameplay::EntitySystem second;
    second.spawn({8.0F, 1.001F, 8.0F}, chickenType(), 99U);
    const int secondTimer = second.entities().front().eggLayTimer;

    REQUIRE(firstTimer == secondTimer);

    gameplay::EntitySystem third;
    third.spawn({8.0F, 1.001F, 8.0F}, chickenType(), 100U);
    const int thirdTimer = third.entities().front().eggLayTimer;
    REQUIRE(thirdTimer != firstTimer);
}

// --- breeding params (EM-3 mechanism, AR-A4 parameters) ---

// Sabotage anchor ②'s target: the chicken type states tempt = wheat seeds
// (not raw wheat, which is the cow/sheep tempt item), breedable = true.
void testChickenBreedingParams() {
    const auto& type = chickenType();
    REQUIRE(type.breedable());
    REQUIRE(gameplay::sameItem(
        type.breeding().temptItem,
        gameplay::ItemStack{world::Block::Air, 1U, &gameplay::items::WheatSeeds}));
    REQUIRE(!gameplay::sameItem(type.breeding().temptItem,
                                gameplay::ItemStack{world::Block::Air, 1U, &gameplay::items::Wheat}));
}

// Two adult chickens fed wheat seeds via the use-on-entity path enter love
// and, given time to close the distance, produce one chick (age < 0).
void testFeedingSeedsBreedsChick() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Survival);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});  // out of the way

    const std::uint64_t first = spawnChicken(session, {5.5F, 2.0F, 5.5F}, 21U);
    const std::uint64_t second = spawnChicken(session, {6.5F, 2.0F, 5.5F}, 22U);

    session.inventory().mutableSlot(0) = {world::Block::Air, 8U, &gameplay::items::WheatSeeds};
    session.inventory().selectHotbar(0);

    useOnEntity(session, world, host, first);
    REQUIRE(session.inventory().selectedStack().count == 7U);
    REQUIRE(session.worldEntities().byId(first)->inLove());

    useOnEntity(session, world, host, second);
    REQUIRE(session.inventory().selectedStack().count == 6U);

    bool bred = false;
    for (int tick = 0; tick < 200 && !bred; ++tick) {
        for (const auto& entity : session.worldEntities().entities()) {
            if (entity.type == &chickenType() && entity.baby()) {
                bred = true;
                break;
            }
        }
        if (bred) {
            break;
        }
        session.tick(world, host);
    }
    REQUIRE(bred);
    int chicks = 0;
    for (const auto& entity : session.worldEntities().entities()) {
        if (entity.type == &chickenType() && entity.baby()) {
            ++chicks;
        }
    }
    REQUIRE(chicks == 1);
}

// Feeding raw wheat (not the chicken's tempt item) never starts love.
void testFeedingWheatDoesNothing() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});

    const std::uint64_t chickenId = spawnChicken(session, {5.5F, 2.0F, 5.5F}, 31U);
    session.inventory().mutableSlot(0) = {world::Block::Air, 8U, &gameplay::items::Wheat};
    session.inventory().selectHotbar(0);

    useOnEntity(session, world, host, chickenId);

    REQUIRE(!session.worldEntities().byId(chickenId)->inLove());
    REQUIRE(session.inventory().selectedStack().count == 8U);  // nothing consumed
}

// --- tempt ---

// Holding wheat seeds draws a nearby chicken toward the player.
void testTemptFollowsSeeds() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 2.0F, 5.5F});
    const std::uint64_t chickenId = spawnChicken(session, {8.5F, 2.0F, 5.5F}, 41U);

    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::WheatSeeds};
    session.inventory().selectHotbar(0);

    const float startDistance =
        std::fabs(session.worldEntities().byId(chickenId)->position.x - 0.5F);
    for (int tick = 0; tick < 60; ++tick) {
        session.tick(world, host);
    }
    const float endDistance =
        std::fabs(session.worldEntities().byId(chickenId)->position.x - 0.5F);
    REQUIRE(endDistance < startDistance - 0.5F);
}

// --- fall immunity ---

// Ticks `entities` (a single spawned creature standing over a stone floor at
// y=1) until it lands, returning its health after landing. `startHeight`
// gives it enough fallDistance to clear the "no damage under 3 blocks" floor
// vanilla's ceil(fallDistance - 3) formula has.
float healthAfterFalling(gameplay::EntitySystem& entities, const world::World& world,
                         std::uint64_t id) {
    float minHealth = entities.byId(id)->damage.health;
    for (int tick = 0; tick < 200; ++tick) {
        entities.tick(world);
        minHealth = std::min(minHealth, entities.byId(id)->damage.health);
        if (entities.byId(id)->onGround && tick > 5) {
            break;
        }
    }
    return minHealth;
}

// Sabotage anchor ③'s target: a chicken dropped from height takes zero fall
// damage, while a non-immune species (pig) at the identical fallDistance
// takes the ordinary ceil(fallDistance - 3) damage — proving the mechanism
// reads a per-type flag, not a global "nobody takes fall damage" regression.
void testChickenFallImmuneOthersDo() {
    REQUIRE(chickenType().fallImmune());
    REQUIRE(!gameplay::entities::builtinSpecies("pig").fallImmune());

    const world::World world = makeWideFlatWorld();

    gameplay::EntitySystem chickens;
    chickens.spawn({8.0F, 20.0F, 8.0F}, chickenType(), 51U);
    const std::uint64_t chickenId = chickens.entities().front().id;
    const float chickenMaxHealth = chickens.byId(chickenId)->damage.maxHealth;
    const float chickenMinHealth = healthAfterFalling(chickens, world, chickenId);
    REQUIRE(chickenMinHealth == chickenMaxHealth);  // no damage at all

    gameplay::EntitySystem pigs;
    pigs.spawn({8.0F, 20.0F, 8.0F}, gameplay::entities::builtinSpecies("pig"), 52U);
    const std::uint64_t pigId = pigs.entities().front().id;
    const float pigMaxHealth = pigs.byId(pigId)->damage.maxHealth;
    const float pigMinHealth = healthAfterFalling(pigs, world, pigId);
    REQUIRE(pigMinHealth < pigMaxHealth);  // the regression guard: a plain
                                            // species from the same height
                                            // does take fall damage
}

} // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();

    testEggLayDropsExactlyOneEggAndRerolls();
    testEggLayTimerDeterministic();
    testChickenBreedingParams();
    testFeedingSeedsBreedsChick();
    testFeedingWheatDoesNothing();
    testTemptFollowsSeeds();
    testChickenFallImmuneOthersDo();
    return 0;
}
