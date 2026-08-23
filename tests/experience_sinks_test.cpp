// XP-4: the consume/sink side of the experience currency — canAfford/
// consumeLevels (the stable interface the future enchanting table/anvil will
// call), the death-drop pipeline (min(7*level,100) experience -> orbs via
// XP-1's spawnExperienceOrbs, then a full reset to zero) and the
// keepInventory gamerule sharing the same on/off switch experience and
// inventory both key off of. Headless (no Vulkan, no window) — this is
// GameSession::onPlayerDeath and PlayerExperience::consumeLevels/canAfford
// exercised as plain simulation state.

#include "gameplay/GameRules.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/PlayerExperience.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/gen/JavaRandom.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::gameplay;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"experience_sinks_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

struct TestHost final : SimulationHost {
    bool playerDied = false;
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
    void onPlayerDied() override { playerDied = true; }
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

// --- Sink 1: canAfford/consumeLevels is the stable enchant/anvil interface —
// sufficient levels: atomic success; insufficient: refuse, leave untouched. ---
void testConsumeLevelsSufficientAndInsufficient() {
    PlayerExperience xp;
    xp.giveExperienceLevels(10);

    REQUIRE(xp.canAfford(3));
    REQUIRE(xp.consumeLevels(3));
    REQUIRE(xp.level() == 7);

    // Sabotage③ target: canAfford false must leave the level untouched — a
    // consumeLevels that forgot the canAfford gate would drive the level
    // negative (or, worse, wrap in an unsigned reading) here.
    REQUIRE(!xp.canAfford(100));
    REQUIRE(!xp.consumeLevels(100));
    REQUIRE(xp.level() == 7);
    std::cout << "testConsumeLevelsSufficientAndInsufficient OK\n";
}

// --- Sink 2: death drop = min(7*level, 100), scattered as orbs at the death
// position, and the player's experience resets to zero afterward. ---
void testDeathDropsClampedExperienceAndZeroesPlayer() {
    // level=10: 7*10=70, under the 100 cap -> the raw formula value drops.
    {
        TestHost host;
        GameSession session;
        session.setGameMode(GameMode::Survival);
        world::World world;
        buildFloor(world);
        session.player().setPosition({5.5F, 1.0F, 5.5F});
        session.experience().giveExperienceLevels(10);
        REQUIRE(session.experience().level() == 10);
        REQUIRE(session.experienceOrbs().entities().empty());

        REQUIRE(session.hurtPlayer(kPrimaryPlayerId, DamageType::Fall, 1000.0F, host));
        session.drainEvents();
        REQUIRE(host.playerDied);

        // The death drop lands as orbs whose point values sum to exactly 70 —
        // sabotage① target ("level=30 dies -> 100, not 210") is the >100 case
        // exercised below, this is the plain sub-cap case.
        std::int32_t totalOrbValue = 0;
        for (const auto& orb : session.experienceOrbs().entities()) {
            totalOrbValue += orb.value * orb.count;
        }
        REQUIRE(totalOrbValue == 70);
        REQUIRE(session.experience().level() == 0);
        REQUIRE(session.experience().totalExperience() == 0);
        REQUIRE(session.experience().pointsIntoLevel() == 0);
    }

    // level=20: 7*20=140, clamped to vanilla's 100-point cap. This is the
    // task's own worked example and the sabotage① assertion: a formula that
    // forgot the min(...,100) clamp would drop 140, not 100.
    {
        TestHost host;
        GameSession session;
        session.setGameMode(GameMode::Survival);
        world::World world;
        buildFloor(world);
        session.player().setPosition({5.5F, 1.0F, 5.5F});
        session.experience().giveExperienceLevels(20);
        REQUIRE(session.experience().level() == 20);

        REQUIRE(session.hurtPlayer(kPrimaryPlayerId, DamageType::Fall, 1000.0F, host));
        session.drainEvents();
        REQUIRE(host.playerDied);

        std::int32_t totalOrbValue = 0;
        for (const auto& orb : session.experienceOrbs().entities()) {
            totalOrbValue += orb.value * orb.count;
        }
        REQUIRE(totalOrbValue == 100);
        REQUIRE(session.experience().level() == 0);
    }

    // level=30: 7*30=210, also clamped to 100 — the exact sabotage①
    // assertion from the task card ("level=30 dies -> drops 100, not 210").
    {
        TestHost host;
        GameSession session;
        session.setGameMode(GameMode::Survival);
        world::World world;
        buildFloor(world);
        session.player().setPosition({5.5F, 1.0F, 5.5F});
        session.experience().giveExperienceLevels(30);

        REQUIRE(session.hurtPlayer(kPrimaryPlayerId, DamageType::Fall, 1000.0F, host));
        session.drainEvents();

        std::int32_t totalOrbValue = 0;
        for (const auto& orb : session.experienceOrbs().entities()) {
            totalOrbValue += orb.value * orb.count;
        }
        REQUIRE(totalOrbValue == 100);
        REQUIRE(totalOrbValue != 210);
    }
    std::cout << "testDeathDropsClampedExperienceAndZeroesPlayer OK\n";
}

// --- Sink 3: level=0 death drops nothing (no orb, no negative/zero-value
// spawn) — the min(7*0,100)==0 edge the drop-amount guard must skip. ---
void testDeathAtLevelZeroDropsNoOrbs() {
    TestHost host;
    GameSession session;
    session.setGameMode(GameMode::Survival);
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 1.0F, 5.5F});
    REQUIRE(session.experience().level() == 0);

    REQUIRE(session.hurtPlayer(kPrimaryPlayerId, DamageType::Fall, 1000.0F, host));
    session.drainEvents();
    REQUIRE(host.playerDied);
    REQUIRE(session.experienceOrbs().entities().empty());
    REQUIRE(session.experience().level() == 0);
    std::cout << "testDeathAtLevelZeroDropsNoOrbs OK\n";
}

// --- Sink 4: keepInventory true keeps BOTH the inventory and the experience
// — vanilla ties them to the same switch (XP-experience/REGULAR.md #8). This
// is the sabotage② target: "keepInventory on -> experience unchanged". ---
void testKeepInventoryPreservesExperience() {
    TestHost host;
    GameSession session;
    session.setGameMode(GameMode::Survival);
    REQUIRE(session.gameRules().set<bool>(GameRuleId::KeepInventory, true));
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 1.0F, 5.5F});
    session.experience().giveExperienceLevels(15);
    session.inventory().mutableSlot(0) = ItemStack{world::Block::Stone, 5U};

    REQUIRE(session.hurtPlayer(kPrimaryPlayerId, DamageType::Fall, 1000.0F, host));
    session.drainEvents();
    REQUIRE(host.playerDied);

    // Neither orbs nor a level reset — the whole death-drop branch is skipped.
    REQUIRE(session.experienceOrbs().entities().empty());
    REQUIRE(session.experience().level() == 15);
    REQUIRE(!session.inventory().slot(0).empty());
    std::cout << "testKeepInventoryPreservesExperience OK\n";
}

// --- Sink 5: keepInventory false (the default) actually clears the
// inventory alongside the experience drop, proving the two remain wired to
// the one shared gamerule rather than two independently-checked flags that
// could drift apart. ---
void testKeepInventoryOffDropsBothInventoryAndExperience() {
    TestHost host;
    GameSession session;
    session.setGameMode(GameMode::Survival);
    REQUIRE(!session.gameRules().get<bool>(GameRuleId::KeepInventory));
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 1.0F, 5.5F});
    session.experience().giveExperienceLevels(5);
    session.inventory().mutableSlot(0) = ItemStack{world::Block::Stone, 5U};

    REQUIRE(session.hurtPlayer(kPrimaryPlayerId, DamageType::Fall, 1000.0F, host));
    session.drainEvents();

    REQUIRE(!session.experienceOrbs().entities().empty());
    REQUIRE(session.experience().level() == 0);
    REQUIRE(session.inventory().slot(0).empty());
    std::cout << "testKeepInventoryOffDropsBothInventoryAndExperience OK\n";
}

// --- Determinism: two sessions, same seed / same call sequence, must scatter
// the death-drop orbs identically (position + velocity), never touching the
// wall clock. Mirrors player_experience_test's enchantmentSeed determinism
// check and experience_orb_test's scatter determinism, applied to the death
// path specifically. ---
void testDeathDropScatterIsDeterministic() {
    auto runOnce = [](std::uint64_t seed) {
        TestHost host;
        GameSession session;
        session.setGameMode(GameMode::Survival);
        session.setWorldSeed(seed);
        world::World world;
        buildFloor(world);
        session.player().setPosition({5.5F, 1.0F, 5.5F});
        session.experience().giveExperienceLevels(12);
        REQUIRE(session.hurtPlayer(kPrimaryPlayerId, DamageType::Fall, 1000.0F, host));
        session.drainEvents();
        std::vector<glm::vec3> velocities;
        for (const auto& orb : session.experienceOrbs().entities()) {
            velocities.push_back(orb.velocity);
        }
        return velocities;
    };

    const auto first = runOnce(42ULL);
    const auto second = runOnce(42ULL);
    REQUIRE(first.size() == second.size());
    REQUIRE(!first.empty());
    for (std::size_t i = 0; i < first.size(); ++i) {
        REQUIRE(first[i] == second[i]);
    }

    // A different seed is not guaranteed (and not required) to reproduce the
    // exact same scatter — this just guards against a stub that ignores the
    // seed entirely and always returns the same fixed sequence.
    const auto differentSeed = runOnce(1337ULL);
    bool sawDifference = differentSeed.size() != first.size();
    if (!sawDifference) {
        for (std::size_t i = 0; i < first.size(); ++i) {
            if (differentSeed[i] != first[i]) {
                sawDifference = true;
                break;
            }
        }
    }
    REQUIRE(sawDifference);
    std::cout << "testDeathDropScatterIsDeterministic OK\n";
}

// --- Sink 6: enchantmentSeed reroll/consume interface — confirms the XP-0
// seed field XP-4 is meant to hand to the future enchant table is reachable
// and reroll-stable from a player-owned deterministic stream (not a fresh
// one constructed ad hoc), the way GameSession would thread a per-player
// seed source through in the ENCH follow-up. ---
void testEnchantmentSeedRerollAccessibleFromPlayerExperience() {
    PlayerExperience xp;
    REQUIRE(xp.enchantmentSeed() == 0);
    world::gen::JavaRandom rng(99ULL);
    xp.rerollEnchantmentSeed(rng);
    const auto firstSeed = xp.enchantmentSeed();

    // Re-run the identical stream from scratch: same seed value out.
    PlayerExperience xpAgain;
    world::gen::JavaRandom rngAgain(99ULL);
    xpAgain.rerollEnchantmentSeed(rngAgain);
    REQUIRE(xpAgain.enchantmentSeed() == firstSeed);

    // Consuming levels (a future enchant purchase) does not disturb the seed
    // — the two are independent fields on the same component.
    xp.giveExperienceLevels(10);
    REQUIRE(xp.consumeLevels(4));
    REQUIRE(xp.enchantmentSeed() == firstSeed);
    std::cout << "testEnchantmentSeedRerollAccessibleFromPlayerExperience OK\n";
}

}  // namespace

int main() {
    testConsumeLevelsSufficientAndInsufficient();
    testDeathDropsClampedExperienceAndZeroesPlayer();
    testDeathAtLevelZeroDropsNoOrbs();
    testKeepInventoryPreservesExperience();
    testKeepInventoryOffDropsBothInventoryAndExperience();
    testDeathDropScatterIsDeterministic();
    testEnchantmentSeedRerollAccessibleFromPlayerExperience();
    std::cout << "experience_sinks_test: all tests passed\n";
    return 0;
}
