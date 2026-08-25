// XP-2: experience source wiring — mob kill (xpReward + lastHurtByPlayer
// gate), mining (ore-range roll), smelting (recipe.experience, cashed in on
// withdrawal) and breeding (1-7). All four funnel through XP-1's
// spawnExperienceOrbs; this file proves each source actually reaches it, that
// the environmental/non-player gates hold, and that every random draw is
// deterministic (JavaRandom/LCG stream, never the wall clock). Headless.

#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/FurnaceSystem.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/PlayerExperience.hpp"
#include "gameplay/ScreenHandler.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/MobAi.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::gameplay;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"experience_sources_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

struct TestHost final : SimulationHost {
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
    void playCreatureHurt(const entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const entities::EntityType&, glm::vec3) override {}
    void playFootstep(world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void spawnWaterSplash(glm::vec3) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

void buildFloor(world::World& world) {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
}

// A 5x5-chunk floor, for a test that lets a wandering mob run free for many
// ticks: a single 16x16 chunk lets a zombie's WanderGoal walk it off the edge
// and into the void well before a long fire-burn test finishes (it did, the
// first time this test was written — this is that fix, matching
// aging_breeding_test.cpp's makeFlatWorld()).
void buildWideFloor(world::World& world) {
    for (int chunkZ = -2; chunkZ <= 2; ++chunkZ) {
        for (int chunkX = -2; chunkX <= 2; ++chunkX) {
            world::Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, 0, z, world::Block::Stone);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
}

const entities::AnimalAi kAnimalAi;

// A test-local breedable species, independent of any AR content, so the
// breeding source test owns its own numbers (same shape as
// aging_breeding_test.cpp's wheatAnimal()).
const Item kTestTempt = Item::of("test_tempt");

[[nodiscard]] ItemStack temptStack() { return ItemStack{world::Block::Air, 1U, &kTestTempt}; }

const entities::EntityType& breedableTestAnimal() {
    static const entities::EntityType type =
        entities::EntityType::Builder::create(entities::MobCategory::Creature, kAnimalAi)
            .sized(0.9F, 1.4F)
            .health(10.0F)
            .movementSpeed(0.25F)
            .breedableWith(temptStack())
            .build("test_xp_breed_animal");
    return type;
}

EntityTickResult tickEntities(EntitySystem& system, const world::World& world) {
    return system.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F, Difficulty::Normal,
                       true, false, 0.0F, false);
}

// --- Source 1: mob kill, gated by lastHurtByPlayer. ---

// A player kill (EntitySystem::hurt's default attacker) on a zombie (xpReward
// 5) queues exactly 5 points of pendingExperience at the corpse's position.
void testPlayerKillAwardsXpReward() {
    entities::registerBuiltinEntities();
    const auto* zombieType = entities::entityTypeRegistry().byId("zombie");
    REQUIRE(zombieType != nullptr);
    REQUIRE(zombieType->xpReward() == 5);

    const world::World world;
    EntitySystem system;
    system.spawn({0.0F, 5.0F, 0.0F}, *zombieType, /*seed=*/1U);
    const std::uint64_t id = system.entities().front().id;
    // One infinite-damage hit from the default player attacker kills it outright.
    REQUIRE(system.hurt(id, 1000.0F, {0.0F, 5.0F, 1.0F}));
    REQUIRE(system.byId(id)->dead());
    REQUIRE(system.pendingExperience().size() == 1U);
    REQUIRE(system.pendingExperience().front().second == 5);
}

// #4: a passive animal follows AnimalEntity#getBaseExperienceReward — a player
// kill queues exactly one reward in the 1..3 range, never zero (the old, wrong
// assumption that animals score 0 here). The environmental-death gate is proven
// separately below; here the animal dies to the default player attacker.
void testAnimalAwardsRolledExperience() {
    entities::registerBuiltinEntities();
    const auto* pigType = entities::entityTypeRegistry().byId("pig");
    REQUIRE(pigType != nullptr);
    REQUIRE(pigType->xpReward() == 1);
    REQUIRE(pigType->xpRewardMax() == 3);

    const world::World world;
    EntitySystem system;
    system.spawn({0.0F, 5.0F, 0.0F}, *pigType, /*seed=*/2U);
    const std::uint64_t id = system.entities().front().id;
    REQUIRE(system.hurt(id, 1000.0F, {0.0F, 5.0F, 1.0F}));
    REQUIRE(system.byId(id)->dead());
    REQUIRE(system.pendingExperience().size() == 1U);
    const std::int32_t awarded = system.pendingExperience().front().second;
    REQUIRE(awarded >= 1 && awarded <= 3);
}

// Sabotage ① anchor: a zombie that dies to fire (nobody's hurt() ever ran, so
// lastAttacker is Kind::None) must not pay experience — "environmental kill
// does not drop xp".
void testEnvironmentalDeathAwardsNoXp() {
    entities::registerBuiltinEntities();
    const auto* zombieType = entities::entityTypeRegistry().byId("zombie");
    REQUIRE(zombieType != nullptr);

    const world::World world;
    EntitySystem system;
    system.spawn({0.0F, 5.0F, 0.0F}, *zombieType, /*seed=*/3U);
    const std::uint64_t id = system.entities().front().id;
    SimpleEntity* entity = system.byId(id);
    entity->damage.health = 1.0F;
    REQUIRE(system.setOnFire(id, 20));
    bool died = false;
    for (int tick = 0; tick < 60 && !died; ++tick) {
        tickEntities(system, world);
        const SimpleEntity* live = system.byId(id);
        died = (live == nullptr || live->damage.dead());
    }
    REQUIRE(died);
    // Fire killed it, not a player: no lastAttacker was ever set on this
    // entity, so the lastHurtByPlayer gate in die() must hold it back.
    REQUIRE(system.pendingExperience().empty());
}

// Sabotage ① anchor, other half: a player hit that lands do NOT immediately
// kill, then the mob dies of fire afterward once the attacker-memory window
// (100 ticks) has expired, must not pay out either — a stale attacker
// reference from minutes ago is not a "recent player kill".
void testStaleAttackerWindowExpiresBeforeFireKill() {
    entities::registerBuiltinEntities();
    const auto* zombieType = entities::entityTypeRegistry().byId("zombie");
    REQUIRE(zombieType != nullptr);

    // A wide floor: this test runs 160 ticks total, long enough that a
    // wandering zombie walks off a single 16x16 chunk's edge and free-falls
    // past the -64 despawn floor before the burn ever finishes it.
    world::World world;
    buildWideFloor(world);
    EntitySystem system;
    system.spawn({0.5F, 5.0F, 0.5F}, *zombieType, /*seed=*/4U);
    const std::uint64_t id = system.entities().front().id;
    // A small non-lethal player hit stamps lastAttacker/recentAttackerTicks.
    REQUIRE(system.hurt(id, 1.0F, {0.0F, 5.0F, 1.0F}));
    REQUIRE(!system.byId(id)->dead());
    REQUIRE(system.byId(id)->recentAttackerTicks == 100);
    // Run the 100-tick memory window all the way out with no further hits.
    for (int tick = 0; tick < 100; ++tick) {
        tickEntities(system, world);
    }
    REQUIRE(system.byId(id)->recentAttackerTicks == 0);
    // Now light it and let the burn finish the kill — recentAttackerTicks is
    // already zero, so this must not attribute the death to the stale hit.
    SimpleEntity* entity = system.byId(id);
    entity->damage.health = 1.0F;
    REQUIRE(system.setOnFire(id, 20));
    bool died = false;
    for (int tick = 0; tick < 60 && !died; ++tick) {
        tickEntities(system, world);
        const SimpleEntity* live = system.byId(id);
        died = (live == nullptr || live->damage.dead());
    }
    REQUIRE(died);
    REQUIRE(system.pendingExperience().empty());
}

// A mob-vs-mob hit (a non-player ActorReference) never stamps Kind::Player, so
// even a fatal blow from another creature must not pay experience.
void testMobKillNeverAwardsXp() {
    entities::registerBuiltinEntities();
    const auto* zombieType = entities::entityTypeRegistry().byId("zombie");
    REQUIRE(zombieType != nullptr);

    const world::World world;
    EntitySystem system;
    system.spawn({0.0F, 5.0F, 0.0F}, *zombieType, /*seed=*/5U);
    const std::uint64_t victim = system.entities().front().id;
    system.spawn({0.0F, 5.0F, 2.0F}, *zombieType, /*seed=*/6U);
    const std::uint64_t attacker = system.entities().back().id;
    REQUIRE(system.hurt(victim, 1000.0F, {0.0F, 5.0F, 1.0F},
                        ActorReference::entity(attacker)));
    REQUIRE(system.byId(victim)->dead());
    REQUIRE(system.pendingExperience().empty());
}

// The command-style kill() path (e.g. /kill) never sets an attacker at all,
// so it must not pay experience either, even for a positive-xpReward species.
void testCommandKillAwardsNoXp() {
    entities::registerBuiltinEntities();
    const auto* zombieType = entities::entityTypeRegistry().byId("zombie");
    REQUIRE(zombieType != nullptr);

    EntitySystem system;
    system.spawn({0.0F, 5.0F, 0.0F}, *zombieType, /*seed=*/7U);
    const std::uint64_t id = system.entities().front().id;
    REQUIRE(system.kill(id));
    REQUIRE(system.byId(id)->dead());
    REQUIRE(system.pendingExperience().empty());
}

// --- Source 1, full stack: a real player break through GameSession/
// PlayerInteraction reaches spawnExperienceOrbs and PlayerExperience. ---

void testFullStackPlayerKillReachesPlayerExperience() {
    entities::registerBuiltinEntities();
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    // Right where the zombie spawns, so the orb the kill spawns (right at the
    // corpse) starts inside the 8-block magnet radius instead of drifting
    // there on its own — this test is about the XP-2 wiring, not the orb's
    // physics/magnet reach (that is XP-1's own coverage).
    session.player().setPosition({5.5F, 2.0F, 5.5F});
    const auto* pigType = entities::entityTypeRegistry().byId("zombie");
    REQUIRE(pigType != nullptr);
    session.worldEntities().spawn({5.5F, 2.0F, 5.5F}, *pigType);
    std::uint64_t zombieId = 0U;
    for (const auto& entity : session.worldEntities().entities()) {
        if (entity.type == pigType) {
            zombieId = entity.id;
            break;
        }
    }
    REQUIRE(zombieId != 0U);
    // A single lethal hit (bare-handed damage is 1, so bring health down first,
    // then land the killing blow through the interaction command path).
    session.worldEntities().byId(zombieId)->damage.health = 1.0F;
    PlayerAction action;
    action.kind = PlayerAction::Kind::StartDestroy;
    action.entity = true;
    action.entityId = zombieId;
    session.enqueueCommand(std::move(action));
    const std::int32_t before = session.experience().totalExperience();
    session.tick(world, host);
    REQUIRE(session.worldEntities().byId(zombieId) == nullptr ||
           session.worldEntities().byId(zombieId)->dead());
    // consumeEntityEvents() (called from within tick()) drained pendingExperience
    // into spawnExperienceOrbs already; run a few more ticks so the orb (spawned
    // right at the corpse, well within the 8-block magnet) reaches the player.
    for (int tick = 0; tick < 40; ++tick) {
        session.tick(world, host);
    }
    REQUIRE(session.experience().totalExperience() > before);
}

// --- Source 2: mining, ore range table + PlayerBreak gate. ---

void testOreExperienceRangeTable() {
    const auto coal = oreExperienceRange(world::Block::CoalOre);
    REQUIRE(coal.has_value());
    REQUIRE(coal->minimum == 0U && coal->maximum == 2U);
    const auto redstone = oreExperienceRange(world::Block::RedstoneOre);
    REQUIRE(redstone.has_value());
    REQUIRE(redstone->minimum == 1U && redstone->maximum == 5U);
    const auto lapis = oreExperienceRange(world::Block::LapisOre);
    REQUIRE(lapis.has_value());
    REQUIRE(lapis->minimum == 2U && lapis->maximum == 5U);
    const auto diamond = oreExperienceRange(world::Block::DiamondOre);
    REQUIRE(diamond.has_value());
    REQUIRE(diamond->minimum == 3U && diamond->maximum == 7U);
    const auto emerald = oreExperienceRange(world::Block::EmeraldOre);
    REQUIRE(emerald.has_value());
    REQUIRE(emerald->minimum == 3U && emerald->maximum == 7U);
    const auto quartz = oreExperienceRange(world::Block::NetherQuartzOre);
    REQUIRE(quartz.has_value());
    REQUIRE(quartz->minimum == 2U && quartz->maximum == 5U);
    // Every non-ore block, and the two ores whose value is smelting rather than
    // the raw block, pay nothing.
    REQUIRE(!oreExperienceRange(world::Block::Stone).has_value());
    REQUIRE(!oreExperienceRange(world::Block::IronOre).has_value());
    REQUIRE(!oreExperienceRange(world::Block::GoldOre).has_value());
}

// rollOreExperience always lands inside the stated inclusive range.
void testRollOreExperienceStaysInRange() {
    std::uint64_t state = 0x1234ABCDULL;
    const OreExperienceRange range{3U, 7U};
    for (int i = 0; i < 500; ++i) {
        const auto amount = rollOreExperience(state, range);
        REQUIRE(amount >= 3 && amount <= 7);
    }
}

// Full stack: breaking a diamond ore as a player (through PlayerInteraction's
// real destroy command) awards experience in [3, 7]; breaking stone awards 0
// (the task's own acceptance bar). Creative mode awards nothing (SuppressDrops
// vetoes onDropsRequested entirely, matching vanilla).
void testFullStackMiningAwardsInRange() {
    TestHost host;
    GameSession session;
    // Ore experience is a survival-only reward: SuppressDrops on a creative
    // break vetoes onDropsRequested (and therefore the roll) entirely, the
    // same gate the item drop itself goes through. GameSession defaults to
    // Creative, so survival must be selected explicitly.
    session.setGameMode(GameMode::Survival);
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 1.0F, 5.5F});
    // Ore experience shares the drop's own tool-tier gate (Block#
    // requiresCorrectToolForDrops) — a bare hand breaks the ore but yields
    // neither the gem nor any experience, so an iron pickaxe is required here
    // exactly the way a real miner would carry one.
    session.inventory().mutableSlot(0) = ItemStack{world::Block::Air, 1U, &items::IronPickaxe};
    session.inventory().selectHotbar(0);
    world.setState(5, 1, 5, world::BlockState{world::Block::DiamondOre});
    session.enqueueCommand(
        PlayerAction{PlayerAction::Kind::StartDestroy, glm::ivec3{5, 1, 5}});
    const float duration = miningSeconds(world::Block::DiamondOre,
                                         session.inventory().selectedStack(),
                                         session.player().inWater(), !session.player().onGround());
    const auto ticks = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(duration) * 20.0));
    const std::int32_t before = session.experience().totalExperience();
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        session.tick(world, host);
    }
    REQUIRE(world.block(5, 1, 5) == world::Block::Air);
    // Let the orb (spawned at the ore's centre, right where the player is
    // standing over it) travel into the 8-block magnet and get collected.
    for (int tick = 0; tick < 40; ++tick) {
        session.tick(world, host);
    }
    const std::int32_t gained = session.experience().totalExperience() - before;
    REQUIRE(gained >= 3 && gained <= 7);
}

void testFullStackMiningStoneAwardsNothing() {
    TestHost host;
    GameSession session;
    // Survival, matching testFullStackMiningAwardsInRange — this proves stone
    // pays nothing under the same conditions diamond ore pays 3-7 in, not
    // merely because creative already suppresses every drop.
    session.setGameMode(GameMode::Survival);
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 1.0F, 5.5F});
    world.setState(5, 1, 5, world::BlockState{world::Block::Stone});
    session.enqueueCommand(
        PlayerAction{PlayerAction::Kind::StartDestroy, glm::ivec3{5, 1, 5}});
    const float duration = miningSeconds(world::Block::Stone, session.inventory().selectedStack(),
                                         session.player().inWater(), !session.player().onGround());
    const auto ticks = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(duration) * 20.0));
    const std::int32_t before = session.experience().totalExperience();
    for (std::uint64_t tick = 0; tick < ticks + 40; ++tick) {
        session.tick(world, host);
    }
    REQUIRE(world.block(5, 1, 5) == world::Block::Air);
    REQUIRE(session.experience().totalExperience() == before);
}

// --- Source 3: smelting, recipe.experience accumulated then cashed on take. ---

// Sabotage ② anchor: pendingExperience only appears at all once a smelt
// finishes, and popExperience only pays out when clickOutput actually moved
// something — the accumulator itself must never leak points before a take.
void testFurnaceAccumulatesButDoesNotPayUntilTaken() {
    FurnaceSystem furnaces;
    REQUIRE(furnaces.place({0, 64, 0}));
    auto& furnace = *furnaces.find({0, 64, 0});
    furnace.input = ItemStack{world::Block::IronOre, 1U, blockItemFor(world::Block::IronOre)};
    furnace.fuel = ItemStack{world::Block::Air, 1U, &items::Coal};
    for (int tick = 0; tick < 200; ++tick) {
        furnaces.tick();
    }
    REQUIRE(furnace.output.item == &items::IronIngot);
    // iron_ingot_from_smelting bakes experience 0.7; one completed smelt banks
    // exactly that, uncashed.
    REQUIRE(furnace.pendingExperience > 0.69F && furnace.pendingExperience < 0.71F);
    // popExperience before any withdrawal still returns the full amount (the
    // furnace itself never gates on "was it taken" — clickOutput/ScreenHandler
    // own that decision); the bug this guards is a version that pays out on
    // the smelt tick itself rather than waiting for a real click.
    const float popped = furnaces.popExperience({0, 64, 0});
    REQUIRE(popped > 0.69F && popped < 0.71F);
    // A second pop, with nothing smelted since, returns nothing further.
    REQUIRE(furnaces.popExperience({0, 64, 0}) == 0.0F);
}

// clickOutput reports whether anything actually left the slot, which is
// XP-2's cue to cash in — a full inventory (nothing moves) must not pop.
void testClickOutputReportsWhetherAnythingMoved() {
    FurnaceSystem furnaces;
    REQUIRE(furnaces.place({0, 64, 0}));
    auto& furnace = *furnaces.find({0, 64, 0});
    furnace.output = ItemStack{world::Block::Air, 1U, &items::IronIngot};
    Inventory inventory;
    // Fill the cursor with a full stack of a *different* item (coal), through
    // the real click path (an empty-cursor left click swaps the whole clicked
    // stack in) — mergeIntoCursor then refuses the iron ingot: sameItem fails,
    // so nothing can be taken.
    inventory.mutableSlot(0) = ItemStack{
        world::Block::Air,
        itemMaximumStackSize(ItemStack{world::Block::Air, 1U, &items::Coal}), &items::Coal};
    inventory.clickSlot(0, InventoryMouseButton::Left, false);
    REQUIRE(!inventory.cursorStack().empty());
    REQUIRE(inventory.cursorStack().item == &items::Coal);
    const bool moved = furnaces.clickOutput({0, 64, 0}, inventory, /*shiftHeld=*/false);
    REQUIRE(!moved);
    REQUIRE(!furnace.output.empty());  // nothing was taken

    Inventory empty;
    const bool movedNow = furnaces.clickOutput({0, 64, 0}, empty, /*shiftHeld=*/false);
    REQUIRE(movedNow);
    REQUIRE(furnace.output.empty());
}

// Full stack: smelting an iron ore and then withdrawing it through
// ScreenHandler::click (the real furnace-output slot path) reaches
// PlayerExperience via spawnExperienceOrbs. Iron's 0.7 experience means the
// gain is 0 or 1 depending on the fractional roll, so the test only pins that
// a completed, withdrawn smelt is capable of paying — determinism is proven
// separately (testFurnaceExperienceIsDeterministic below) rather than pinned
// to one literal outcome here.
void testFullStackFurnaceWithdrawalReachesPlayerExperience() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    world.setBlock(5, 1, 5, world::Block::Furnace);
    REQUIRE(session.furnaceSystem().place({5, 1, 5}));
    auto& furnace = *session.furnaceSystem().find({5, 1, 5});
    furnace.input = ItemStack{world::Block::IronOre, 1U, blockItemFor(world::Block::IronOre)};
    furnace.fuel = ItemStack{world::Block::Air, 1U, &items::Coal};
    for (int tick = 0; tick < 200; ++tick) {
        session.furnaceSystem().tick();
    }
    REQUIRE(furnace.output.item == &items::IronIngot);
    REQUIRE(furnace.pendingExperience > 0.0F);

    ScreenContext context;
    context.screen = ContainerScreen::Furnace;
    context.furnace = {5, 1, 5};
    SlotView outputSlot{{}, nullptr, SlotKind::FurnaceOutput, 0U};
    ScreenHandler::click(session, context, outputSlot, InventoryMouseButton::Left,
                        /*shiftHeld=*/false);
    // The accumulator was popped (cashed in or discarded by the fractional
    // roll) the instant the click moved the item, never left sitting banked.
    REQUIRE(session.furnaceSystem().find({5, 1, 5})->pendingExperience == 0.0F);
    REQUIRE(!session.inventory().cursorStack().empty());
}

// Sabotage ② anchor, full stack: a click on the output slot that moves
// nothing (the player's cursor is already full of something else) must leave
// the banked experience untouched — ScreenHandler must gate the cash-in on
// clickOutput's own report, not pop unconditionally on every click. This is
// the exact path a version that "pays out on any furnace-output click" would
// slip past testClickOutputReportsWhetherAnythingMoved (which calls
// FurnaceSystem directly, bypassing ScreenHandler entirely).
void testScreenHandlerDoesNotCashInWhenNothingMoved() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    world.setBlock(5, 1, 5, world::Block::Furnace);
    REQUIRE(session.furnaceSystem().place({5, 1, 5}));
    auto& furnace = *session.furnaceSystem().find({5, 1, 5});
    furnace.input = ItemStack{world::Block::GoldOre, 1U, blockItemFor(world::Block::GoldOre)};
    furnace.fuel = ItemStack{world::Block::Air, 1U, &items::Coal};
    for (int tick = 0; tick < 200; ++tick) {
        session.furnaceSystem().tick();
    }
    REQUIRE(furnace.output.item == &items::GoldIngot);
    REQUIRE(furnace.pendingExperience > 0.99F && furnace.pendingExperience < 1.01F);

    // Block the cursor with a full stack of a different item, through the
    // real click path, so clickOutput's mergeIntoCursor cannot take anything.
    session.inventory().mutableSlot(1) = ItemStack{
        world::Block::Air, itemMaximumStackSize(ItemStack{world::Block::Air, 1U, &items::Coal}),
        &items::Coal};
    session.inventory().clickSlot(1, InventoryMouseButton::Left, false);
    REQUIRE(session.inventory().cursorStack().item == &items::Coal);

    ScreenContext context;
    context.screen = ContainerScreen::Furnace;
    context.furnace = {5, 1, 5};
    SlotView outputSlot{{}, nullptr, SlotKind::FurnaceOutput, 0U};
    ScreenHandler::click(session, context, outputSlot, InventoryMouseButton::Left,
                        /*shiftHeld=*/false);
    // Nothing left the output slot (the cursor was blocked), so the banked
    // experience must still be sitting there, uncashed.
    REQUIRE(!session.furnaceSystem().find({5, 1, 5})->output.empty());
    REQUIRE(session.furnaceSystem().find({5, 1, 5})->pendingExperience > 0.99F);
}

// A high enough per-craft experience value (gold's 1.0) guarantees at least
// one orb every single time, so the full stack can be pinned exactly: smelt
// three gold ingots (3.0 banked, no fractional roll ever needed) and confirm
// the player gains exactly 3.
void testFullStackFurnaceGoldExperienceIsExact() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 1.0F, 5.5F});
    world.setBlock(5, 1, 5, world::Block::Furnace);
    REQUIRE(session.furnaceSystem().place({5, 1, 5}));
    auto& furnace = *session.furnaceSystem().find({5, 1, 5});
    furnace.input = ItemStack{world::Block::GoldOre, 3U, blockItemFor(world::Block::GoldOre)};
    furnace.fuel = ItemStack{world::Block::Air, 3U, &items::Coal};
    for (int tick = 0; tick < 601; ++tick) {
        session.furnaceSystem().tick();
    }
    REQUIRE(furnace.output.item == &items::GoldIngot);
    REQUIRE(furnace.output.count == 3U);
    REQUIRE(furnace.pendingExperience > 2.99F && furnace.pendingExperience < 3.01F);

    ScreenContext context;
    context.screen = ContainerScreen::Furnace;
    context.furnace = {5, 1, 5};
    SlotView outputSlot{{}, nullptr, SlotKind::FurnaceOutput, 0U};
    const std::int32_t before = session.experience().totalExperience();
    ScreenHandler::click(session, context, outputSlot, InventoryMouseButton::Left,
                        /*shiftHeld=*/false);
    for (int tick = 0; tick < 40; ++tick) {
        session.tick(world, host);
    }
    REQUIRE(session.experience().totalExperience() - before == 3);
}

// --- Source 4: breeding, flat 1-7 roll. ---

void testBreedingAwardsOneToSeven() {
    const world::World world = [] {
        world::World w;
        world::Chunk chunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                chunk.setBlock(x, 0, z, world::Block::Stone);
            }
        }
        w.setChunk({0, 0}, std::move(chunk));
        return w;
    }();
    EntitySystem system;
    system.spawn({0.5F, 1.0F, 0.5F}, breedableTestAnimal(), 10U);
    system.spawn({1.2F, 1.0F, 0.5F}, breedableTestAnimal(), 11U);
    const std::uint64_t a = system.entities()[0].id;
    const std::uint64_t b = system.entities()[1].id;
    REQUIRE(system.setInLove(a));
    REQUIRE(system.setInLove(b));

    bool bred = false;
    for (int tick = 0; tick < 40 && !bred; ++tick) {
        tickEntities(system, world);
        if (!system.pendingExperience().empty()) {
            bred = true;
        }
    }
    REQUIRE(bred);
    REQUIRE(system.pendingExperience().size() == 1U);
    const std::int32_t amount = system.pendingExperience().front().second;
    REQUIRE(amount >= 1 && amount <= 7);
}

// Determinism: the same seed pair breeds and rolls the identical amount every
// replay.
void testBreedingExperienceIsDeterministic() {
    const world::World world = [] {
        world::World w;
        world::Chunk chunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                chunk.setBlock(x, 0, z, world::Block::Stone);
            }
        }
        w.setChunk({0, 0}, std::move(chunk));
        return w;
    }();

    const auto runOnce = [&]() -> std::int32_t {
        EntitySystem system;
        system.spawn({0.5F, 1.0F, 0.5F}, breedableTestAnimal(), 20U);
        system.spawn({1.2F, 1.0F, 0.5F}, breedableTestAnimal(), 21U);
        REQUIRE(system.setInLove(system.entities()[0].id));
        REQUIRE(system.setInLove(system.entities()[1].id));
        for (int tick = 0; tick < 40; ++tick) {
            tickEntities(system, world);
            if (!system.pendingExperience().empty()) {
                return system.pendingExperience().front().second;
            }
        }
        return -1;
    };
    const std::int32_t first = runOnce();
    const std::int32_t second = runOnce();
    REQUIRE(first >= 1 && first <= 7);
    REQUIRE(first == second);
}

// --- Sabotage ③ anchor, general: mining's roll is a caller-owned LCG stream,
// never the wall clock / a global RNG, so the same seed reproduces the same
// sequence across independent MiningSystem-only calls (no World/GameSession
// involved at all — this isolates the RNG contract itself from the block
// break plumbing above). ---
void testOreRollIsDeterministicAcrossIndependentStreams() {
    std::uint64_t stateA = 777ULL;
    std::uint64_t stateB = 777ULL;
    const OreExperienceRange range{2U, 5U};
    std::vector<std::int32_t> sequenceA;
    std::vector<std::int32_t> sequenceB;
    for (int i = 0; i < 20; ++i) {
        sequenceA.push_back(rollOreExperience(stateA, range));
        sequenceB.push_back(rollOreExperience(stateB, range));
    }
    REQUIRE(sequenceA == sequenceB);
}

} // namespace

int main() {
    testPlayerKillAwardsXpReward();
    testAnimalAwardsRolledExperience();
    testEnvironmentalDeathAwardsNoXp();
    testStaleAttackerWindowExpiresBeforeFireKill();
    testMobKillNeverAwardsXp();
    testCommandKillAwardsNoXp();
    testFullStackPlayerKillReachesPlayerExperience();
    testOreExperienceRangeTable();
    testRollOreExperienceStaysInRange();
    testFullStackMiningAwardsInRange();
    testFullStackMiningStoneAwardsNothing();
    testFurnaceAccumulatesButDoesNotPayUntilTaken();
    testClickOutputReportsWhetherAnythingMoved();
    testFullStackFurnaceWithdrawalReachesPlayerExperience();
    testScreenHandlerDoesNotCashInWhenNothingMoved();
    testFullStackFurnaceGoldExperienceIsExact();
    testBreedingAwardsOneToSeven();
    testBreedingExperienceIsDeterministic();
    testOreRollIsDeterministicAcrossIndependentStreams();
    return 0;
}
