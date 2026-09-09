// AR-M5: the villager's closed loop — claim a composter, farm, compost, trade.
//
// One profession (farmer), one workstation (composter), spawned by egg rather
// than by a village, which is the scope this task was given. What is worth
// pinning is the part that is a MECHANISM rather than a number:
//
//   * a workstation is claimed EXCLUSIVELY — two villagers must never share
//     one, which is the whole content of vanilla's POI ticket;
//   * losing the workstation loses the profession, so a broken composter really
//     does un-employ the farmer standing at it;
//   * a trade is gated on the villager's LEVEL, and the level is raised by the
//     trading experience the offers themselves carry — the "trade to unlock"
//     loop, not a fixed list;
//   * the goods are handed over BEFORE the payment is taken, so a full
//     inventory cannot eat the payment.

#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameCommand.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/Villager.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

using namespace mc;
using gameplay::entities::VillagerProfession;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"villager_test line " + std::to_string(line) +
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

[[nodiscard]] world::World makeFlatWorld() {
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

// How many of `item` the inventory holds, across every slot — there is no
// counter on Inventory itself, and a trade's goods land wherever they fit.
[[nodiscard]] std::size_t countHeld(const gameplay::Inventory& inventory,
                                    const gameplay::Item* item) {
    std::size_t total = 0U;
    for (const auto& slot : inventory.slots()) {
        if (slot.item == item) {
            total += slot.count;
        }
    }
    return total;
}

[[nodiscard]] const gameplay::entities::EntityType& villagerType() {
    const auto* type = gameplay::entities::entityTypeRegistry().byId("villager");
    if (type == nullptr) {
        throw std::runtime_error{"villager species not registered"};
    }
    return *type;
}

// --- 1) the manifest row ---------------------------------------------------

void testManifestRowMatchesVanilla() {
    const auto& type = villagerType();
    // MobCategory::Misc is vanilla's own: it is why a villager never spawns
    // naturally, never despawns and survives Peaceful.
    REQUIRE(type.category() == gameplay::entities::MobCategory::Misc);
    REQUIRE(!gameplay::entities::mobCategoryTraits(type.category()).naturalSpawn);
    REQUIRE(!gameplay::entities::mobCategoryTraits(type.category()).despawnsWhenDistant);
    REQUIRE(type.attributes().get(gameplay::entities::Attribute::MaxHealth) == 20.0F);
    // 0.5 is the one attribute Villager.createAttributes() sets itself; every
    // other mob in this roster is slower, so an exact compare is meaningful.
    REQUIRE(type.attributes().get(gameplay::entities::Attribute::MovementSpeed) == 0.5F);
    REQUIRE(type.dimensions().width == 0.6F);
    REQUIRE(type.dimensions().height == 1.95F);
    // A killed villager gives no experience and no loot: its value is its trades.
    REQUIRE(type.xpReward() == 0 && type.xpRewardMax() == 0);
    REQUIRE(type.hasSpawnEgg());
    REQUIRE(type.villager());
    REQUIRE(!type.undead());
}

// --- 2) levels and trading experience --------------------------------------

// VillagerData's thresholds {0, 10, 70, 150, 250}, exercised through the
// after-trade rule. Exact levels, and a single big gain that crosses several
// thresholds at once — the case a plain `if (xp >= next) ++level` gets wrong.
void testLevelThresholds() {
    using gameplay::entities::villagerAfterTrade;
    REQUIRE(villagerAfterTrade(1, 0, 2).level == 1);
    REQUIRE(villagerAfterTrade(1, 0, 2).xp == 2);
    REQUIRE(villagerAfterTrade(1, 8, 1).level == 1);   // 9 < 10
    REQUIRE(villagerAfterTrade(1, 8, 2).level == 2);   // 10 >= 10
    REQUIRE(villagerAfterTrade(2, 10, 59).level == 2); // 69 < 70
    REQUIRE(villagerAfterTrade(2, 10, 60).level == 3); // 70 >= 70
    // One trade worth 300 carries a novice all the way to the ceiling.
    REQUIRE(villagerAfterTrade(1, 0, 300).level == 5);
    // And the ceiling holds: level 5 never climbs, however much it earns.
    REQUIRE(villagerAfterTrade(5, 250, 1000).level == 5);
    REQUIRE(gameplay::entities::villagerXpToNextLevel(5) == 0);
}

// The farmer's table: unlocked by level, and level 2/3 really are locked at
// level 1 (the assertion that a "return every offer" implementation fails).
void testOffersUnlockByLevel() {
    const auto offers = gameplay::entities::offersFor(VillagerProfession::Farmer);
    REQUIRE(!offers.empty());
    int atLevel1 = 0;
    int atLevel3 = 0;
    for (const auto& offer : offers) {
        if (gameplay::entities::offerAvailable(offer, 1, 0U)) ++atLevel1;
        if (gameplay::entities::offerAvailable(offer, 3, 0U)) ++atLevel3;
        REQUIRE(offer.level >= 1 && offer.level <= 5);
        REQUIRE(offer.maxUses > 0);
    }
    REQUIRE(atLevel1 == 4);              // farmer/1 has four transcribed offers
    REQUIRE(atLevel3 == offers.size());  // by level 3 the whole table is open
    REQUIRE(atLevel1 < atLevel3);
    // An offer at its use ceiling is unavailable however high the level.
    const auto& first = offers[0];
    REQUIRE(!gameplay::entities::offerAvailable(first, 5,
                                                static_cast<std::uint8_t>(first.maxUses)));
    // The unemployed sell nothing.
    REQUIRE(gameplay::entities::offersFor(VillagerProfession::None).empty());
}

// --- 3) claiming a job site ------------------------------------------------

// Drives the entity pass with no player nearby, which is all the work goal needs.
void tickEntities(gameplay::EntitySystem& entities, const world::World& world, int count) {
    for (int tick = 0; tick < count; ++tick) {
        static_cast<void>(entities.tick(world));
    }
}

void testClaimsComposterAndBecomesFarmer() {
    world::World world = makeFlatWorld();
    world.setBlock(8, 1, 8, world::Block::Composter);
    gameplay::EntitySystem entities;
    entities.spawn({10.0F, 1.001F, 8.0F}, villagerType(), 5U);
    const std::uint64_t id = entities.entities().front().id;
    REQUIRE(entities.byId(id)->villagerProfession == VillagerProfession::None);
    REQUIRE(!entities.byId(id)->hasJobSite);

    tickEntities(entities, world, 40);
    REQUIRE(entities.byId(id)->hasJobSite);
    const glm::ivec3 expectedSite{8, 1, 8};
    REQUIRE(entities.byId(id)->jobSite == expectedSite);
    REQUIRE(entities.byId(id)->villagerProfession == VillagerProfession::Farmer);

    // ValidateNearbyPoi + ResetProfession: mine the composter and the farmer is
    // unemployed again. Not merely "job site cleared" — the profession has to go
    // with it, or the villager keeps selling wheat with no place to work.
    world.setBlock(8, 1, 8, world::Block::Air);
    tickEntities(entities, world, 5);
    REQUIRE(!entities.byId(id)->hasJobSite);
    REQUIRE(entities.byId(id)->villagerProfession == VillagerProfession::None);
}

// The POI ticket, which is the one thing about claiming that is not obvious:
// two villagers beside ONE composter must not both claim it.
void testOneComposterIsClaimedByOneVillager() {
    world::World world = makeFlatWorld();
    world.setBlock(8, 1, 8, world::Block::Composter);
    gameplay::EntitySystem entities;
    entities.spawn({9.0F, 1.001F, 8.0F}, villagerType(), 5U);
    entities.spawn({7.0F, 1.001F, 8.0F}, villagerType(), 9U);
    const std::uint64_t first = entities.entities()[0].id;
    const std::uint64_t second = entities.entities()[1].id;

    tickEntities(entities, world, 60);
    const bool firstClaimed = entities.byId(first)->hasJobSite;
    const bool secondClaimed = entities.byId(second)->hasJobSite;
    REQUIRE(firstClaimed != secondClaimed);  // exactly one, not both, not neither
    const auto* employed = firstClaimed ? entities.byId(first) : entities.byId(second);
    const auto* idle = firstClaimed ? entities.byId(second) : entities.byId(first);
    REQUIRE(employed->villagerProfession == VillagerProfession::Farmer);
    REQUIRE(idle->villagerProfession == VillagerProfession::None);

    // A second composter employs the other one too — the exclusion is per
    // workstation, not "one farmer in the world".
    world.setBlock(4, 1, 8, world::Block::Composter);
    tickEntities(entities, world, 60);
    REQUIRE(entities.byId(first)->hasJobSite && entities.byId(second)->hasJobSite);
    REQUIRE(entities.byId(first)->jobSite != entities.byId(second)->jobSite);
}

// --- 4) farming ------------------------------------------------------------

// Reap and replant: the crop goes back to age 0 (never to air) and the produce
// lands in the villager's carry slot.
void testHarvestsMatureCropAndReplants() {
    TestHost host;
    world::World world = makeFlatWorld();
    world.setBlock(8, 1, 8, world::Block::Composter);
    world.setBlock(9, 1, 8, world::Block::Farmland);
    world.setState(9, 2, 8,
                   world::BlockState{world::Block::WheatCrops}.withAge(7));
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Survival);
    session.player().setPosition({8.5F, 1.0F, 20.5F});
    session.worldEntities().spawn({9.5F, 2.001F, 8.5F}, villagerType(), 5U);
    const std::uint64_t id = session.worldEntities().entities().front().id;

    for (int tick = 0; tick < 80; ++tick) {
        session.tick(world, host);
        if (session.worldEntities().byId(id)->villagerCarryCount > 0U) {
            break;
        }
    }
    const auto* villager = session.worldEntities().byId(id);
    REQUIRE(villager->villagerCarryCount > 0U);
    REQUIRE(villager->villagerCarryItem == &gameplay::items::Wheat);
    // Replanted, not broken: the field is still wheat and it is young again.
    const auto after = world.state(9, 2, 8);
    REQUIRE(after.block() == world::Block::WheatCrops);
    REQUIRE(after.age() == 0);
}

// An unripe field is left alone. Its own test rather than an extra crop in the
// one above, because "it harvested the ripe one" passes just as well when the
// maturity check is missing entirely — only a field with NOTHING ripe in it can
// tell the two apart.
void testUnripeCropIsLeftAlone() {
    TestHost host;
    world::World world = makeFlatWorld();
    world.setBlock(8, 1, 8, world::Block::Composter);
    world.setBlock(9, 1, 8, world::Block::Farmland);
    world.setState(9, 2, 8, world::BlockState{world::Block::WheatCrops}.withAge(3));
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Survival);
    session.player().setPosition({8.5F, 1.0F, 20.5F});
    session.worldEntities().spawn({9.5F, 2.001F, 8.5F}, villagerType(), 5U);
    const std::uint64_t id = session.worldEntities().entities().front().id;

    for (int tick = 0; tick < 120; ++tick) {
        session.tick(world, host);
    }
    REQUIRE(session.worldEntities().byId(id)->villagerCarryCount == 0U);
    REQUIRE(session.worldEntities().byId(id)->villagerCarryItem == nullptr);
    // And the crop is exactly as it was — not reaped, not reset, not grown by
    // this pass (the session's own random tick could grow it, so the assertion
    // is "at least as old", never "younger").
    REQUIRE(world.state(9, 2, 8).block() == world::Block::WheatCrops);
    REQUIRE(world.state(9, 2, 8).age() >= 3);
}

// What it reaped goes into its own composter — the loop closing.
void testCarriedProduceGoesIntoTheComposter() {
    TestHost host;
    world::World world = makeFlatWorld();
    world.setBlock(8, 1, 8, world::Block::Composter);
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Survival);
    session.player().setPosition({8.5F, 1.0F, 20.5F});
    session.worldEntities().spawn({9.0F, 1.001F, 8.0F}, villagerType(), 5U);
    const std::uint64_t id = session.worldEntities().entities().front().id;
    // Let it claim the composter first, then hand it a full carry slot.
    for (int tick = 0; tick < 40; ++tick) {
        session.tick(world, host);
    }
    REQUIRE(session.worldEntities().byId(id)->hasJobSite);
    session.worldEntities().byId(id)->villagerCarryItem = &gameplay::items::Wheat;
    session.worldEntities().byId(id)->villagerCarryCount = 8U;

    for (int tick = 0; tick < 200; ++tick) {
        session.tick(world, host);
        if (world.state(8, 1, 8).composterLevel() > 0) {
            break;
        }
    }
    REQUIRE(world.state(8, 1, 8).composterLevel() > 0);
    // And it spent what it was carrying to do it.
    REQUIRE(session.worldEntities().byId(id)->villagerCarryCount < 8U);
}

// --- 5) trading ------------------------------------------------------------

// One right-click with a payable stack: goods in, payment out, use counted,
// experience earned.
void testTradeExchangesAndEarnsExperience() {
    TestHost host;
    world::World world = makeFlatWorld();
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Survival);
    session.player().setPosition({8.5F, 1.0F, 8.5F});
    session.worldEntities().spawn({9.0F, 1.001F, 8.5F}, villagerType(), 5U);
    auto* villager = session.worldEntities().byId(
        session.worldEntities().entities().front().id);
    const std::uint64_t id = villager->id;
    villager->villagerProfession = VillagerProfession::Farmer;

    // 20 wheat -> 1 emerald, farmer/1, xp 2.
    session.inventory().replaceSelected(
        gameplay::ItemStack{world::Block::Air, 40U, &gameplay::items::Wheat});
    gameplay::UseItemOn use;
    use.entity = true;
    use.entityId = id;
    session.enqueueCommand(use);
    session.tick(world, host);
    session.enqueueCommand(gameplay::UseItemStop{});
    session.tick(world, host);

    REQUIRE(session.inventory().selectedStack().count == 20U);  // 40 - 20
    REQUIRE(countHeld(session.inventory(), &gameplay::items::Emerald) == 1U);
    REQUIRE(session.worldEntities().byId(id)->villagerTradeXp == 2);
    REQUIRE(session.worldEntities().byId(id)->villagerOfferUses[0] == 1U);
    // Still a novice: one wheat trade is 2 of the 10 needed.
    REQUIRE(session.worldEntities().byId(id)->villagerLevel == 1U);
}

// A locked offer stays locked, and unlocks when the level catches up. The
// assertion that separates "level gates the table" from "the table is a list".
void testLockedOfferUnlocksWithLevel() {
    TestHost host;
    world::World world = makeFlatWorld();
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Survival);
    session.player().setPosition({8.5F, 1.0F, 8.5F});
    session.worldEntities().spawn({9.0F, 1.001F, 8.5F}, villagerType(), 5U);
    const std::uint64_t id = session.worldEntities().entities().front().id;
    session.worldEntities().byId(id)->villagerProfession = VillagerProfession::Farmer;

    const auto click = [&] {
        gameplay::UseItemOn use;
        use.entity = true;
        use.entityId = id;
        session.enqueueCommand(use);
        session.tick(world, host);
        session.enqueueCommand(gameplay::UseItemStop{});
        for (int tick = 0; tick < 5; ++tick) {
            session.tick(world, host);
        }
    };

    // Six pumpkins buy an emerald — but only from a level-2 farmer.
    session.inventory().replaceSelected(gameplay::ItemStack{world::Block::Pumpkin, 12U});
    click();
    REQUIRE(session.inventory().selectedStack().count == 12U);   // refused
    REQUIRE(countHeld(session.inventory(), &gameplay::items::Emerald) == 0U);

    // Promote it exactly the way trading would.
    session.worldEntities().byId(id)->villagerLevel = 2U;
    click();
    REQUIRE(session.inventory().selectedStack().count == 6U);    // paid
    REQUIRE(countHeld(session.inventory(), &gameplay::items::Emerald) == 1U);
    REQUIRE(session.worldEntities().byId(id)->villagerTradeXp == 10);
}

} // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();
    testManifestRowMatchesVanilla();
    testLevelThresholds();
    testOffersUnlockByLevel();
    testClaimsComposterAndBecomesFarmer();
    testOneComposterIsClaimedByOneVillager();
    testHarvestsMatureCropAndReplants();
    testUnripeCropIsLeftAlone();
    testCarriedProduceGoesIntoTheComposter();
    testTradeExchangesAndEarnsExperience();
    testLockedOfferUnlocksWithLevel();
    return 0;
}
