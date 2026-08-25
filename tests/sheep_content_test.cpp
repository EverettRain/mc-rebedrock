// AR-A2: sheep gameplay completion — shearing, EatGrassGoal's wool regrow (via
// the W write path), the breeding parameters handed to EM-3 (tempt = wheat,
// baby = lamb), tempt following and determinism. Headless, no Vulkan.
//
// EM-3 itself (the age/love/breed state machine) is already covered by
// aging_breeding_test.cpp with a synthetic species; this file only proves the
// *sheep* manifest row wires into it correctly and that AR-A2's own new
// content (shears, EatGrassGoal) behaves.

#include "gameplay/GameSession.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <stdexcept>
#include <string>

using namespace mc;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"sheep_content_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

struct TestHost final : gameplay::SimulationHost {
    // AR-A2 sabotage anchor ①: only WorldMutationService's setBlock ends up
    // publishing a WorldEditEvent (GameplayMutationSink::onSectionDirty fires
    // unconditionally on every real change and is the only source of these
    // calls in this test's whole run). A direct block-array poke would leave
    // this counter at zero for the eaten cell — the assertion below is on the
    // *mechanism*, not merely the block's before/after value.
    int worldStateEdits = 0;
    std::optional<glm::ivec3> lastStateEditPosition;

    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int x, int y, int z, world::BlockState) override {
        ++worldStateEdits;
        lastStateEditPosition = glm::ivec3{x, y, z};
    }
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

// A whole-chunk grass_block floor (stone underneath) at y=1, so a wandering
// sheep is always standing over an edible cell no matter which way
// WanderAroundFarGoal takes it — unlike a single grass tile, which the sheep
// can (and reliably does) wander off before the 1-in-1000 canStart roll lands.
void buildGrassFloor(world::World& world) {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
            chunk.setBlock(x, 1, z, world::Block::Grass);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
}

// A whole-chunk short_grass floor: solid stone at y=0 supports the plant, and
// short_grass itself occupies y=1 (the sheep's own feet cell, per
// EatGrassGoal's other branch).
void buildGrassPlantFloor(world::World& world) {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
            chunk.setBlock(x, 1, z, world::Block::GrassPlant);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
}

[[nodiscard]] const gameplay::entities::EntityType& sheepType() {
    const auto* type = gameplay::entities::entityTypeRegistry().byId("sheep");
    if (type == nullptr) {
        throw std::runtime_error{"sheep species not registered"};
    }
    return *type;
}

// Spawns one sheep and returns its stable id.
[[nodiscard]] std::uint64_t spawnSheep(gameplay::GameSession& session, glm::vec3 position,
                                       std::uint32_t seed = 1U) {
    session.worldEntities().spawn(position, sheepType(), seed);
    return session.worldEntities().entities().back().id;
}

// A single right-click-on-entity: press (UseItemOn), tick once so performUse
// actually runs, then release (UseItemStop) and tick a few more times so
// `using_` clears *and* the vanilla-mirroring rightClickDelay (nextUseTick_,
// PlayerInteraction.cpp: 4 ticks) has fully elapsed before the caller's next
// click — without the wait, a second useOnEntity call queued too soon lands
// inside the still-cooling-down window and is silently swallowed (not a
// repeat-fire risk, the opposite: performUse never runs for it at all).
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

// --- shearing ---

void testShearDropsWoolAndMarksSheared() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Survival);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {5.5F, 2.0F, 5.5F});

    const std::uint64_t sheepId = spawnSheep(session, {5.5F, 2.0F, 6.0F});
    REQUIRE(!session.worldEntities().byId(sheepId)->sheared);

    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Shears};
    session.inventory().selectHotbar(0);
    const std::uint16_t damageBefore = session.inventory().selectedStack().damage;

    useOnEntity(session, world, host, sheepId);

    const auto* sheep = session.worldEntities().byId(sheepId);
    REQUIRE(sheep != nullptr);
    REQUIRE(sheep->sheared);
    // The shears spent exactly one durability point (Sheep#mobInteract:
    // itemStack.hurtAndBreak(1, ...)).
    REQUIRE(session.inventory().selectedStack().damage == damageBefore + 1U);

    // A wool item entity landed somewhere near the sheep, 1-3 white wool.
    bool sawWool = false;
    for (const auto& item : session.itemEntities().entities()) {
        if (item.stack.item == gameplay::blockItemFor(world::Block::WhiteWool)) {
            REQUIRE(item.stack.count >= 1U && item.stack.count <= 3U);
            sawWool = true;
        }
    }
    REQUIRE(sawWool);
}

// Sabotage anchor ③: an already-sheared sheep yields nothing on a second shear
// (no wool, no durability spent, no-op).
void testShearAlreadyShearedIsNoOp() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {5.5F, 2.0F, 5.5F});

    const std::uint64_t sheepId = spawnSheep(session, {5.5F, 2.0F, 6.0F});
    REQUIRE(session.worldEntities().shear(sheepId));  // pre-shear directly
    REQUIRE(session.worldEntities().byId(sheepId)->sheared);

    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Shears};
    session.inventory().selectHotbar(0);
    const std::uint16_t damageBefore = session.inventory().selectedStack().damage;
    const std::size_t itemsBefore = session.itemEntities().entities().size();

    useOnEntity(session, world, host, sheepId);

    REQUIRE(session.worldEntities().byId(sheepId)->sheared);  // unchanged
    REQUIRE(session.inventory().selectedStack().damage == damageBefore);  // no durability spent
    REQUIRE(session.itemEntities().entities().size() == itemsBefore);     // nothing dropped
}

// --- eat-grass (the W write path) ---

// A sheared sheep standing on grass_block eventually eats it (grass_block ->
// dirt) through WorldMutationService, and regrows its wool. This is sabotage
// anchor ① territory: a direct block-array write would still flip the block,
// but the neighbour-update/section-dirty contract only fires through the
// mutation service, and the assertion below is on the *outcome* the service
// alone produces — the block having actually changed via setBlock/setState.
void testEatGrassRegrowsWoolThroughMutationService() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    // A whole-chunk grass floor: wherever WanderAroundFarGoal takes the sheep,
    // the cell below its feet is grass_block, so the 1-in-1000 canStart roll is
    // never blocked on the sheep happening to stand in the right spot.
    buildGrassFloor(world);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});  // far away

    const std::uint64_t sheepId = spawnSheep(session, {5.5F, 2.0F, 5.5F}, 7U);
    REQUIRE(session.worldEntities().shear(sheepId));
    REQUIRE(session.worldEntities().byId(sheepId)->sheared);

    bool ate = false;
    // Generous bound: canStart is a 1-in-1000 roll each tick, so this may take
    // a while; the eat animation itself is 40 ticks once it starts. 20000
    // ticks is comfortably past the expected value with headroom.
    for (int tick = 0; tick < 20000 && !ate; ++tick) {
        session.tick(world, host);
        static_cast<void>(session.drainEvents());  // feeds TestHost::worldStateEdits
        const auto* sheep = session.worldEntities().byId(sheepId);
        if (sheep == nullptr) {
            break;
        }
        if (!sheep->sheared) {
            ate = true;
        }
    }
    REQUIRE(ate);
    // At least one grass_block cell in the floor became dirt.
    bool sawDirt = false;
    for (int z = 0; z < 16 && !sawDirt; ++z) {
        for (int x = 0; x < 16 && !sawDirt; ++x) {
            if (world.block(x, 1, z) == world::Block::Dirt) {
                sawDirt = true;
            }
        }
    }
    REQUIRE(sawDirt);
    // Sabotage anchor ①: the eat's block change must have travelled through
    // GameplayMutationSink::onSectionDirty (the only source of
    // submitWorldStateEdit calls anywhere in this run) rather than a direct
    // array poke, which would leave the host oblivious to the edit entirely.
    REQUIRE(host.worldStateEdits > 0);
}

// A sheared sheep over short_grass eats the grass itself (short_grass -> air),
// the other EatGrassGoal branch.
void testEatGrassPlantVariant() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildGrassPlantFloor(world);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});

    // The sheep's feet must occupy a short_grass cell, which the whole-chunk
    // plant floor guarantees no matter where it wanders to.
    const std::uint64_t sheepId = spawnSheep(session, {5.5F, 1.0F, 5.5F}, 9U);
    REQUIRE(session.worldEntities().shear(sheepId));

    bool ate = false;
    for (int tick = 0; tick < 20000 && !ate; ++tick) {
        session.tick(world, host);
        const auto* sheep = session.worldEntities().byId(sheepId);
        if (sheep == nullptr) {
            break;
        }
        if (!sheep->sheared) {
            ate = true;
        }
    }
    REQUIRE(ate);
    REQUIRE(!session.worldEntities().byId(sheepId)->sheared);
}

// --- breeding params (EM-3 mechanism, AR-A2 parameters) ---

// The sheep manifest row states tempt = wheat, breedable = true, baby scale
// 0.5 (the lamb). The state machine itself (love/cooldown/spawn) is EM-3's own
// tested territory (aging_breeding_test.cpp); this only proves sheep's params
// reach it — sabotage anchor ②'s target.
void testSheepBreedingParams() {
    const auto& type = sheepType();
    REQUIRE(type.breedable());
    REQUIRE(gameplay::sameItem(type.breeding().temptItem,
                               gameplay::ItemStack{world::Block::Air, 1U, &gameplay::items::Wheat}));
    REQUIRE(!gameplay::sameItem(
        type.breeding().temptItem,
        gameplay::ItemStack{world::Block::Air, 1U, &gameplay::items::WheatSeeds}));
    REQUIRE(type.breeding().babyScale == 0.5F);
}

// Two adult sheep fed wheat via the use-on-entity path enter love and, given
// time to close the distance, produce one lamb (age < 0).
void testFeedingWheatBreedsLamb() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Survival);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});  // out of the way

    const std::uint64_t first = spawnSheep(session, {5.5F, 2.0F, 5.5F}, 21U);
    const std::uint64_t second = spawnSheep(session, {6.5F, 2.0F, 5.5F}, 22U);

    session.inventory().mutableSlot(0) = {world::Block::Air, 8U, &gameplay::items::Wheat};
    session.inventory().selectHotbar(0);

    // Feed the first and check it went into love before feeding the second —
    // once *both* are in love and they are already within breeding range (one
    // block apart here), AnimalMateGoal can settle and breed within the few
    // ticks useOnEntity's own cooldown wait spends, which would clear love on
    // both before a single post-loop check could observe it.
    useOnEntity(session, world, host, first);
    REQUIRE(session.inventory().selectedStack().count == 7U);
    REQUIRE(session.worldEntities().byId(first)->inLove());

    useOnEntity(session, world, host, second);
    REQUIRE(session.inventory().selectedStack().count == 6U);

    // useOnEntity's own tail ticks (waiting out rightClickDelay) are already
    // enough for two adjacent in-love sheep to settle and breed, so the lamb
    // may already exist by this point — check for one directly rather than a
    // pre/post entity-count delta racing against a breed that already
    // happened during the wait above.
    bool bred = false;
    for (int tick = 0; tick < 200 && !bred; ++tick) {
        for (const auto& entity : session.worldEntities().entities()) {
            if (entity.type == &sheepType() && entity.baby()) {
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
    int lambs = 0;
    for (const auto& entity : session.worldEntities().entities()) {
        if (entity.type == &sheepType() && entity.baby()) {
            ++lambs;
        }
    }
    REQUIRE(lambs == 1);
}

// AR-A5 #9: feeding a *lamb* its tempt item speeds its growth (ageUp) instead
// of putting it in love — the baby half of Animal#mobInteract, driven end to
// end through the use-on-entity interaction path (not the direct EntitySystem
// API the aging test uses). One wheat is spent, the arm swings, the lamb is
// meaningfully closer to adult, and it never enters love.
void testFeedingLambSpeedsGrowth() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Survival);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});

    const std::uint64_t lambId = spawnSheep(session, {5.5F, 2.0F, 5.5F}, 51U);
    // Make it a full newborn so a single feed's 120s (2400 ticks) shows up well
    // clear of the aiStep's own -1/tick natural growth over the handful of ticks
    // useOnEntity spends.
    REQUIRE(session.worldEntities().setAge(lambId, -24000));
    REQUIRE(session.worldEntities().byId(lambId)->baby());
    const int ageBefore = session.worldEntities().byId(lambId)->age;

    session.inventory().mutableSlot(0) = {world::Block::Air, 8U, &gameplay::items::Wheat};
    session.inventory().selectHotbar(0);

    useOnEntity(session, world, host, lambId);

    const auto* lamb = session.worldEntities().byId(lambId);
    REQUIRE(lamb != nullptr);
    // A lamb never enters love, and the feed did grow it: age jumped by the
    // 120s * 20 ageUp grant (minus the few natural-growth ticks useOnEntity
    // spends), so it is at least ~2000 ticks closer to adult than a lone feed's
    // natural drift could account for.
    REQUIRE(!lamb->inLove());
    REQUIRE(lamb->age > ageBefore + 2000);
    REQUIRE(lamb->baby());  // one feed does not fully raise a newborn
    // One wheat spent (usePlayerItem), like the adult love feed.
    REQUIRE(session.inventory().selectedStack().count == 7U);
}

// AR-A5 #11: a lamb (never sheared — Sheep#readyForShearing bars babies) still
// eats grass, and Sheep#ate ages it up 60s. Proves the sheared full-gate is
// gone (a wooled lamb can graze) and that ate()'s ageUp half fires through the
// real eat-grass -> WorldMutationService -> ate() relay. A lamb rolls the
// EatBlockGoal ~20x more often than an adult (bound 50 vs 1000), so this eats
// quickly. Detection is by the age jump, not by sheared (a lamb has none).
void testLambEatsGrassAndGrows() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildGrassFloor(world);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});

    const std::uint64_t lambId = spawnSheep(session, {5.5F, 2.0F, 5.5F}, 61U);
    // A full newborn so the 60s (1200 tick) grant is unmistakable against the
    // natural +1/tick drift over however long it takes to graze.
    REQUIRE(session.worldEntities().setAge(lambId, -24000));
    REQUIRE(!session.worldEntities().byId(lambId)->sheared);  // a lamb has no wool to shear
    const int ageStart = session.worldEntities().byId(lambId)->age;

    // A lamb's aiStep grows its age by exactly +1/tick naturally, so after N
    // ticks an *unfed* lamb sits at ageStart + N. Sheep#ate adds a 1200-tick
    // jump on top of that on the tick it grazes; the instant age exceeds the
    // pure-natural baseline by more than a couple of ticks' slack, the ate()
    // ageUp half must have landed. (Detected by the age jump, not a sheared
    // flag — a lamb has none.)
    bool grew = false;
    for (int tick = 0; tick < 20000 && !grew; ++tick) {
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        const auto* sheep = session.worldEntities().byId(lambId);
        if (sheep == nullptr) {
            break;
        }
        const int naturalBaseline = ageStart + (tick + 1);
        if (sheep->age > naturalBaseline + 100) {
            grew = true;
        }
    }
    REQUIRE(grew);
    const auto* lamb = session.worldEntities().byId(lambId);
    REQUIRE(lamb != nullptr);
    REQUIRE(lamb->baby());  // still a lamb, just grown a bit
}

// Feeding seeds (not the sheep's tempt item) never starts love.
void testFeedingSeedsDoesNothing() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});

    const std::uint64_t sheepId = spawnSheep(session, {5.5F, 2.0F, 5.5F}, 31U);
    session.inventory().mutableSlot(0) = {world::Block::Air, 8U, &gameplay::items::WheatSeeds};
    session.inventory().selectHotbar(0);

    useOnEntity(session, world, host, sheepId);

    REQUIRE(!session.worldEntities().byId(sheepId)->inLove());
    REQUIRE(session.inventory().selectedStack().count == 8U);  // nothing consumed
}

// --- tempt ---

// Holding wheat draws a nearby sheep toward the player; putting it away stops
// the approach.
void testTemptFollowsWheat() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 2.0F, 5.5F});
    const std::uint64_t sheepId = spawnSheep(session, {8.5F, 2.0F, 5.5F}, 41U);

    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Wheat};
    session.inventory().selectHotbar(0);

    const float startDistance =
        std::fabs(session.worldEntities().byId(sheepId)->position.x - 0.5F);
    for (int tick = 0; tick < 60; ++tick) {
        session.tick(world, host);
    }
    const float endDistance =
        std::fabs(session.worldEntities().byId(sheepId)->position.x - 0.5F);
    REQUIRE(endDistance < startDistance - 0.5F);
}

// --- determinism ---

void testDeterministicEatGrass() {
    const auto run = [](std::uint32_t seed) {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildGrassFloor(world);
        session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});
        const std::uint64_t sheepId = spawnSheep(session, {5.5F, 2.0F, 5.5F}, seed);
        REQUIRE(session.worldEntities().shear(sheepId));
        int ateAtTick = -1;
        glm::vec3 finalPosition{0.0F};
        for (int tick = 0; tick < 20000; ++tick) {
            session.tick(world, host);
            const auto* sheep = session.worldEntities().byId(sheepId);
            if (sheep == nullptr) {
                break;
            }
            if (!sheep->sheared) {
                ateAtTick = tick;
                finalPosition = sheep->position;
                break;
            }
        }
        return std::pair{ateAtTick, finalPosition};
    };
    const auto [firstTick, firstPosition] = run(13U);
    const auto [secondTick, secondPosition] = run(13U);
    REQUIRE(firstTick >= 0);
    REQUIRE(firstTick == secondTick);
    // Tick-for-tick identical: the sheep wandered to the same place by the
    // same tick in both runs, not merely "ate at some point".
    REQUIRE(std::fabs(firstPosition.x - secondPosition.x) < 0.0001F);
    REQUIRE(std::fabs(firstPosition.z - secondPosition.z) < 0.0001F);
}

} // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();

    testShearDropsWoolAndMarksSheared();
    testShearAlreadyShearedIsNoOp();
    testEatGrassRegrowsWoolThroughMutationService();
    testEatGrassPlantVariant();
    testSheepBreedingParams();
    testFeedingWheatBreedsLamb();
    testFeedingLambSpeedsGrowth();
    testLambEatsGrassAndGrows();
    testFeedingSeedsDoesNothing();
    testTemptFollowsWheat();
    testDeterministicEatGrass();
    return 0;
}
