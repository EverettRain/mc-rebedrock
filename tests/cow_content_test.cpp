// AR-A3: cow gameplay completion — milking (Bucket -> MilkBucket), drinking
// milk clears status effects (EM-2's clearEffects), the breeding parameters
// handed to EM-3 (tempt = wheat, baby = calf), tempt following, panic
// (EscapeDangerGoal, already free from AnimalAi — asserted rather than
// reimplemented) and determinism. Headless, no Vulkan.
//
// EM-3 itself (the age/love/breed state machine) is already covered by
// aging_breeding_test.cpp with a synthetic species; this file only proves the
// *cow* type wires into it correctly and that AR-A3's own new content
// (milking, milk's drink-clears-effects) behaves.

#include "gameplay/GameSession.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/StatusEffect.hpp"
#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
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
        throw std::runtime_error{"cow_content_test line " + std::to_string(line) +
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

[[nodiscard]] const gameplay::entities::EntityType& cowType() {
    const auto* type = gameplay::entities::entityTypeRegistry().byId("cow");
    if (type == nullptr) {
        throw std::runtime_error{"cow species not registered"};
    }
    return *type;
}

// Spawns one cow and returns its stable id.
[[nodiscard]] std::uint64_t spawnCow(gameplay::GameSession& session, glm::vec3 position,
                                     std::uint32_t seed = 1U) {
    session.worldEntities().spawn(position, cowType(), seed);
    return session.worldEntities().entities().back().id;
}

// A single right-click-on-entity: press (UseItemOn), tick once so performUse
// actually runs, then release (UseItemStop) and tick a few more times so
// `using_` clears *and* the vanilla-mirroring rightClickDelay (nextUseTick_,
// PlayerInteraction.cpp: 4 ticks) has fully elapsed before the caller's next
// click — without the wait, a second useOnEntity call queued too soon lands
// inside the still-cooling-down window and is silently swallowed.
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

// Holds `item` down for `ticks` simulation ticks (used to drive the drink
// timeline to completion, unlike useOnEntity's single-tick press which is
// only enough to reach performUse once).
void holdUseFor(gameplay::GameSession& session, world::World& world, gameplay::SimulationHost& host,
               int ticks) {
    session.enqueueCommand(gameplay::UseItem{});
    for (int tick = 0; tick < ticks; ++tick) {
        session.tick(world, host);
    }
    session.enqueueCommand(gameplay::UseItemStop{});
    session.tick(world, host);
}

// --- milking ---

void testMilkingSurvivalConsumesBucket() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Survival);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {5.5F, 2.0F, 5.5F});

    const std::uint64_t cowId = spawnCow(session, {5.5F, 2.0F, 6.0F});

    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Bucket};
    session.inventory().selectHotbar(0);

    useOnEntity(session, world, host, cowId);

    // Sabotage anchor ①: survival spends the empty bucket for exactly one
    // milk bucket — net item count is conserved, nothing is conjured.
    const auto& stack = session.inventory().selectedStack();
    REQUIRE(stack.item == &gameplay::items::MilkBucket);
    REQUIRE(stack.count == 1U);
}

void testMilkingCreativeKeepsBucket() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Creative);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {5.5F, 2.0F, 5.5F});

    const std::uint64_t cowId = spawnCow(session, {5.5F, 2.0F, 6.0F});

    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Bucket};
    session.inventory().selectHotbar(0);

    useOnEntity(session, world, host, cowId);

    // Sabotage anchor ①: creative pours without spending anything — the
    // empty bucket must still be the empty bucket, not a free milk bucket.
    const auto& stack = session.inventory().selectedStack();
    REQUIRE(stack.item == &gameplay::items::Bucket);
    REQUIRE(stack.count == 1U);
}

// A full (water) bucket right-clicked on a cow produces no milk — only the
// plain empty Bucket item reaches the milking branch.
void testNonEmptyBucketYieldsNoMilk() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Survival);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {5.5F, 2.0F, 5.5F});

    const std::uint64_t cowId = spawnCow(session, {5.5F, 2.0F, 6.0F});

    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::WaterBucket};
    session.inventory().selectHotbar(0);

    useOnEntity(session, world, host, cowId);

    const auto& stack = session.inventory().selectedStack();
    REQUIRE(stack.item == &gameplay::items::WaterBucket);  // unchanged
    REQUIRE(stack.count == 1U);
}

// A non-bucket item (wheat, the cow's own tempt item) right-clicked on a cow
// enters the tempt/breed branch, not milking — no milk bucket appears.
void testNonBucketItemYieldsNoMilk() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Survival);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {5.5F, 2.0F, 5.5F});

    const std::uint64_t cowId = spawnCow(session, {5.5F, 2.0F, 6.0F});

    session.inventory().mutableSlot(0) = {world::Block::Air, 8U, &gameplay::items::Wheat};
    session.inventory().selectHotbar(0);

    useOnEntity(session, world, host, cowId);

    REQUIRE(session.inventory().selectedStack().item == &gameplay::items::Wheat);
    // No MilkBucket anywhere in the hotbar slot used.
    REQUIRE(session.inventory().selectedStack().item != &gameplay::items::MilkBucket);
}

// No cooldown: the same bucket, once refilled, can milk again immediately —
// unlike shears, which needs a durability/no-op guard between shears.
void testMilkingHasNoCooldown() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Survival);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {5.5F, 2.0F, 5.5F});

    const std::uint64_t cowId = spawnCow(session, {5.5F, 2.0F, 6.0F});
    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Bucket};
    session.inventory().selectHotbar(0);

    useOnEntity(session, world, host, cowId);
    REQUIRE(session.inventory().selectedStack().item == &gameplay::items::MilkBucket);

    // Refill the bucket (as if it had been emptied elsewhere) and milk again
    // right away — no per-cow or per-bucket cooldown blocks the second milk.
    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Bucket};
    useOnEntity(session, world, host, cowId);
    REQUIRE(session.inventory().selectedStack().item == &gameplay::items::MilkBucket);
}

// A baby cow (calf) cannot be milked, mirroring AbstractCow#mobInteract's
// `!this.isBaby()` gate.
void testBabyCowCannotBeMilked() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Survival);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {5.5F, 2.0F, 5.5F});

    const std::uint64_t cowId = spawnCow(session, {5.5F, 2.0F, 6.0F});
    REQUIRE(session.worldEntities().setAge(cowId, -1000));
    REQUIRE(session.worldEntities().byId(cowId)->baby());

    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Bucket};
    session.inventory().selectHotbar(0);

    useOnEntity(session, world, host, cowId);

    REQUIRE(session.inventory().selectedStack().item == &gameplay::items::Bucket);
}

// --- drinking milk clears status effects (EM-2) ---

void testDrinkingMilkClearsEffects() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Survival);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {5.5F, 2.0F, 5.5F});

    REQUIRE(session.primaryPlayer().vitals.applyEffect(gameplay::poisonEffect(), 200, 0U));
    REQUIRE(session.primaryPlayer().vitals.hasEffect(gameplay::poisonEffect()));

    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::MilkBucket};
    session.inventory().selectHotbar(0);

    // The drink timeline is the same 32-tick window as eating; hold well past it.
    holdUseFor(session, world, host, 40);

    REQUIRE(!session.primaryPlayer().vitals.hasEffect(gameplay::poisonEffect()));
    // Survival spends the milk bucket, reverting to an empty Bucket.
    REQUIRE(session.inventory().selectedStack().item == &gameplay::items::Bucket);
}

void testDrinkingMilkCreativeKeepsBucket() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Creative);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {5.5F, 2.0F, 5.5F});

    REQUIRE(session.primaryPlayer().vitals.applyEffect(gameplay::speedEffect(), 200, 0U));

    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::MilkBucket};
    session.inventory().selectHotbar(0);

    holdUseFor(session, world, host, 40);

    REQUIRE(!session.primaryPlayer().vitals.hasEffect(gameplay::speedEffect()));
    // Creative never spends the stack.
    REQUIRE(session.inventory().selectedStack().item == &gameplay::items::MilkBucket);
}

// Milk does not restore hunger — it is not classified as food.
void testMilkIsNotFood() {
    REQUIRE(!gameplay::isFood(&gameplay::items::MilkBucket));
    REQUIRE(gameplay::isDrinkable(&gameplay::items::MilkBucket));
}

// --- registry ---

void testMilkBucketRegistered() {
    const auto* found = gameplay::itemFromIdentifier("milk_bucket");
    REQUIRE(found == &gameplay::items::MilkBucket);
}

// --- breeding params (EM-3 mechanism, AR-A3 parameters) ---

// Sabotage anchor ②'s target: the cow type states tempt = wheat, breedable =
// true, baby scale 0.5 (the calf). The state machine itself (love/cooldown/
// spawn) is EM-3's own tested territory (aging_breeding_test.cpp).
void testCowBreedingParams() {
    const auto& type = cowType();
    REQUIRE(type.breedable());
    REQUIRE(gameplay::sameItem(type.breeding().temptItem,
                               gameplay::ItemStack{world::Block::Air, 1U, &gameplay::items::Wheat}));
    REQUIRE(!gameplay::sameItem(
        type.breeding().temptItem,
        gameplay::ItemStack{world::Block::Air, 1U, &gameplay::items::WheatSeeds}));
    REQUIRE(type.breeding().babyScale == 0.5F);
}

// Two adult cows fed wheat via the use-on-entity path enter love and, given
// time to close the distance, produce one calf (age < 0).
void testFeedingWheatBreedsCalf() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.setGameMode(gameplay::GameMode::Survival);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});  // out of the way

    const std::uint64_t first = spawnCow(session, {5.5F, 2.0F, 5.5F}, 21U);
    const std::uint64_t second = spawnCow(session, {6.5F, 2.0F, 5.5F}, 22U);

    session.inventory().mutableSlot(0) = {world::Block::Air, 8U, &gameplay::items::Wheat};
    session.inventory().selectHotbar(0);

    // Feed the first and check it went into love before feeding the second —
    // once *both* are in love and they are already within breeding range,
    // AnimalMateGoal can settle and breed within the few ticks useOnEntity's
    // own cooldown wait spends.
    useOnEntity(session, world, host, first);
    REQUIRE(session.inventory().selectedStack().count == 7U);
    REQUIRE(session.worldEntities().byId(first)->inLove());

    useOnEntity(session, world, host, second);
    REQUIRE(session.inventory().selectedStack().count == 6U);

    bool bred = false;
    for (int tick = 0; tick < 200 && !bred; ++tick) {
        for (const auto& entity : session.worldEntities().entities()) {
            if (entity.type == &cowType() && entity.baby()) {
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
    int calves = 0;
    for (const auto& entity : session.worldEntities().entities()) {
        if (entity.type == &cowType() && entity.baby()) {
            ++calves;
        }
    }
    REQUIRE(calves == 1);
}

// Feeding seeds (not the cow's tempt item) never starts love.
void testFeedingSeedsDoesNothing() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});

    const std::uint64_t cowId = spawnCow(session, {5.5F, 2.0F, 5.5F}, 31U);
    session.inventory().mutableSlot(0) = {world::Block::Air, 8U, &gameplay::items::WheatSeeds};
    session.inventory().selectHotbar(0);

    useOnEntity(session, world, host, cowId);

    REQUIRE(!session.worldEntities().byId(cowId)->inLove());
    REQUIRE(session.inventory().selectedStack().count == 8U);  // nothing consumed
}

// --- tempt ---

// Holding wheat draws a nearby cow toward the player; sabotage anchor ②
// territory (a mis-set tempt item would leave the cow motionless).
void testTemptFollowsWheat() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 2.0F, 5.5F});
    const std::uint64_t cowId = spawnCow(session, {8.5F, 2.0F, 5.5F}, 41U);

    session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Wheat};
    session.inventory().selectHotbar(0);

    const float startDistance =
        std::fabs(session.worldEntities().byId(cowId)->position.x - 0.5F);
    for (int tick = 0; tick < 60; ++tick) {
        session.tick(world, host);
    }
    const float endDistance =
        std::fabs(session.worldEntities().byId(cowId)->position.x - 0.5F);
    REQUIRE(endDistance < startDistance - 0.5F);
}

// --- panic (already free via AnimalAi's EscapeDangerGoal) ---

// A hurt cow accelerates away from where it was hurt — no AR-A3 code needed,
// this only confirms CowAi (which is AnimalAi) already installs
// EscapeDangerGoal, exactly like every other AnimalAi species.
void testHurtCowFleesFaster() {
    TestHost host;
    gameplay::GameSession session;
    world::World world;
    buildStoneFloor(world);
    session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 5.0F, 0.5F});
    const std::uint64_t cowId = spawnCow(session, {5.5F, 2.0F, 5.5F}, 51U);

    // A few ticks of ordinary wander before the hit, so the escape distance
    // comparison below is not measuring the very first tick's spawn settle.
    for (int tick = 0; tick < 5; ++tick) {
        session.tick(world, host);
    }
    const glm::vec3 hurtPosition = session.worldEntities().byId(cowId)->position;
    REQUIRE(session.worldEntities().hurt(cowId, 1.0F, {0.5F, 2.0F, 5.5F}));

    float maxDistance = 0.0F;
    for (int tick = 0; tick < 40; ++tick) {
        session.tick(world, host);
        const auto* cow = session.worldEntities().byId(cowId);
        REQUIRE(cow != nullptr);
        const float distance = glm::length(cow->position - hurtPosition);
        maxDistance = std::max(maxDistance, distance);
    }
    // EscapeDangerGoal's 2.0x multiplier should carry the cow well clear of
    // its hurt position within this window.
    REQUIRE(maxDistance > 1.0F);
}

// --- determinism ---

void testDeterministicTempt() {
    const auto run = [](std::uint32_t seed) {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildStoneFloor(world);
        session.teleportPlayer(gameplay::kPrimaryPlayerId, {0.5F, 2.0F, 5.5F});
        const std::uint64_t cowId = spawnCow(session, {8.5F, 2.0F, 5.5F}, seed);
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Wheat};
        session.inventory().selectHotbar(0);
        for (int tick = 0; tick < 60; ++tick) {
            session.tick(world, host);
        }
        return session.worldEntities().byId(cowId)->position;
    };
    const auto firstPosition = run(41U);
    const auto secondPosition = run(41U);
    REQUIRE(std::fabs(firstPosition.x - secondPosition.x) < 0.0001F);
    REQUIRE(std::fabs(firstPosition.z - secondPosition.z) < 0.0001F);
}

} // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();

    testMilkingSurvivalConsumesBucket();
    testMilkingCreativeKeepsBucket();
    testNonEmptyBucketYieldsNoMilk();
    testNonBucketItemYieldsNoMilk();
    testMilkingHasNoCooldown();
    testBabyCowCannotBeMilked();
    testDrinkingMilkClearsEffects();
    testDrinkingMilkCreativeKeepsBucket();
    testMilkIsNotFood();
    testMilkBucketRegistered();
    testCowBreedingParams();
    testFeedingWheatBreedsCalf();
    testFeedingSeedsDoesNothing();
    testTemptFollowsWheat();
    testHurtCowFleesFaster();
    testDeterministicTempt();
    return 0;
}
