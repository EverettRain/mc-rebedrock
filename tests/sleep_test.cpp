// SLP-2/3/4: sleeping in a bed — the eight-step chain, the night skip and the
// spawn point.
//
// The step whose ORDER matters and which nothing about the code's shape would
// reveal: vanilla sets the spawn point BEFORE deciding whether you may actually
// sleep. Clicking a bed in daylight refuses the sleep and still re-homes you.
// A chain that returned early on "it is daytime" would look perfectly
// reasonable and be wrong, so that case gets its own assertion.
//
// The other two are the ones this build had no mechanism for at all before:
// a hundred ticks in bed moves the clock to daybreak and resets the weather
// (`WeatherSystem::resetWeather` had existed with no caller since the weather
// system landed), and waking for any other reason must NOT skip the night.

#include "gameplay/GameSession.hpp"
#include "gameplay/Sleep.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/WorldClock.hpp"

#include <cassert>
#include <iostream>
#include <utility>

namespace {

using mc::gameplay::BedSleepProblem;
using mc::gameplay::SleepConditions;
using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockPos;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;
using mc::world::attribute::BedRule;

// The same do-nothing host game_session_test uses; SimulationHost is a wide
// interface and this test cares about none of it.
struct TestHost final : mc::gameplay::SimulationHost {
    int worldEdits = 0;
    int blockBreaks = 0;
    int itemPickups = 0;
    int footsteps = 0;
    int previewEdits = 0;
    bool playerDied = false;
    int furnaceChanges = 0;
    int eatingStarted = 0;
    int eatingCancelled = 0;
    int eatSounds = 0;
    int playerHurts = 0;

    void submitWorldEdit(int, int, int, mc::world::Block, std::uint8_t,
                         std::optional<mc::world::BlockOrientation>) override {
        ++worldEdits;
    }
    void submitWorldStateEdit(int, int, int, mc::world::BlockState) override { ++worldEdits; }
    void previewBlockEdit(int, int, int) override { ++previewEdits; }
    void playBlockBreak(mc::world::Block, glm::vec3) override { ++blockBreaks; }
    void playItemPickup(glm::vec3) override { ++itemPickups; }
    void playEat(glm::vec3) override { ++eatSounds; }
    void playPlayerHurt(glm::vec3) override { ++playerHurts; }
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playExplode(glm::vec3) override {}
    void playCreatureHurt(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(mc::world::Block, glm::vec3, float) override { ++footsteps; }
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, mc::world::Block) override {}
    void onPlayerDied() override { playerDied = true; }
    void onFurnaceStateChanged() override { ++furnaceChanges; }
    void onEatingStarted() override { ++eatingStarted; }
    void onEatingCancelled() override { ++eatingCancelled; }
};

[[nodiscard]] World floored() {
    World world;
    Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

// A bed with its foot at (8,1,8) pointing north, so the head is (8,1,7).
void layBed(World& world) {
    world.setState(8, 1, 8,
                   BlockState{Block::RedBed, BlockOrientation::North}.withBedHead(false));
    world.setState(8, 1, 7, BlockState{Block::RedBed, BlockOrientation::North}.withBedHead(true));
}

[[nodiscard]] SleepConditions nightConditions() {
    SleepConditions conditions;
    conditions.rule = BedRule::CanSleepWhenDark;
    conditions.darkOutside = true;
    conditions.playerPosition = {8.5F, 1.0F, 8.5F};
    return conditions;
}

[[nodiscard]] BedSleepProblem evaluate(const World& world, const SleepConditions& conditions) {
    return mc::gameplay::evaluateSleep(world, BlockPos{8, 1, 8}, world.state(8, 1, 8), conditions)
        .problem;
}

} // namespace

int main() {
    mc::gameplay::entities::registerBuiltinEntities();

    // ---------------------------------------------------------------------
    // 1) The chain, step by step.
    // ---------------------------------------------------------------------
    {
        World world = floored();
        layBed(world);

        // The healthy case.
        assert(evaluate(world, nightConditions()) == BedSleepProblem::None);

        // Already asleep, or dead.
        auto asleep = nightConditions();
        asleep.alreadySleeping = true;
        assert(evaluate(world, asleep) == BedSleepProblem::OtherProblem);
        auto dead = nightConditions();
        dead.alive = false;
        assert(evaluate(world, dead) == BedSleepProblem::OtherProblem);

        // The dimension refuses outright (vanilla's exploding bed; this build
        // has no explosion, so it simply says no — a registered deviation).
        auto nether = nightConditions();
        nether.rule = BedRule::Explodes;
        const auto explodes = mc::gameplay::evaluateSleep(world, BlockPos{8, 1, 8},
                                                          world.state(8, 1, 8), nether);
        assert(explodes.problem == BedSleepProblem::NotPossibleHere);
        assert(!explodes.setsSpawn); // and it does not re-home you either

        // Too far: the reach box is 3 horizontally, 2 vertically, and BOTH cells
        // of the bed count — so standing beside the head is fine.
        auto far = nightConditions();
        far.playerPosition = {8.5F, 1.0F, 13.0F};
        assert(evaluate(world, far) == BedSleepProblem::TooFarAway);
        auto besideHead = nightConditions();
        besideHead.playerPosition = {8.5F, 1.0F, 5.0F}; // 2 from the head, 3 from the foot
        assert(evaluate(world, besideHead) == BedSleepProblem::None);

        // Obstructed above either half.
        World blocked = floored();
        layBed(blocked);
        blocked.setBlock(8, 2, 7, Block::Stone); // above the head
        assert(evaluate(blocked, nightConditions()) == BedSleepProblem::Obstructed);

        // Monsters, unless creative.
        auto monsters = nightConditions();
        monsters.monstersNearby = true;
        assert(evaluate(world, monsters) == BedSleepProblem::NotSafe);
        auto creative = monsters;
        creative.creative = true;
        assert(evaluate(world, creative) == BedSleepProblem::None);

        // Someone is already in it.
        World taken = floored();
        taken.setState(8, 1, 8, BlockState{Block::RedBed, BlockOrientation::North}
                                    .withBedHead(false)
                                    .withOccupied(true));
        taken.setState(8, 1, 7, BlockState{Block::RedBed, BlockOrientation::North}
                                    .withBedHead(true)
                                    .withOccupied(true));
        assert(evaluate(taken, nightConditions()) == BedSleepProblem::OtherProblem);
    }

    // ---------------------------------------------------------------------
    // 2) ★ Daylight refuses the sleep and STILL sets the spawn point. This is
    //    vanilla's order (the spawn write sits between the obstruction check
    //    and the can-sleep gate) and the one thing a "reasonable" rewrite of
    //    this chain would get wrong.
    // ---------------------------------------------------------------------
    {
        World world = floored();
        layBed(world);
        auto day = nightConditions();
        day.darkOutside = false;
        const auto decision =
            mc::gameplay::evaluateSleep(world, BlockPos{8, 1, 8}, world.state(8, 1, 8), day);
        assert(decision.problem == BedSleepProblem::NotPossibleNow);
        assert(decision.setsSpawn);
    }

    // ---------------------------------------------------------------------
    // 3) The monster box is 8 horizontal by 5 vertical, from the bed's centre.
    // ---------------------------------------------------------------------
    {
        const glm::vec3 centre{8.5F, 1.0F, 7.5F};
        assert(mc::gameplay::withinMonsterWakeBox(centre, {16.4F, 1.0F, 7.5F}));
        assert(!mc::gameplay::withinMonsterWakeBox(centre, {16.6F, 1.0F, 7.5F}));
        assert(mc::gameplay::withinMonsterWakeBox(centre, {8.5F, 5.9F, 7.5F}));
        assert(!mc::gameplay::withinMonsterWakeBox(centre, {8.5F, 6.1F, 7.5F}));
    }

    // ---------------------------------------------------------------------
    // 4) End to end through the session: lie down, both halves go OCCUPIED, the
    //    spawn moves, and a hundred ticks later it is morning.
    // ---------------------------------------------------------------------
    {
        World world = floored();
        layBed(world);
        mc::gameplay::GameSession session;
        session.setGameMode(mc::gameplay::GameMode::Survival);
        session.player().setPosition({8.5F, 1.0F, 8.5F});
        TestHost host;
        // Midnight: 18000 ticks in, which is well past skyDarken 4.
        session.clocks().setTotalTicks(mc::world::ClockId::Overworld, 18000U);
        session.tick(world, host); // resolve the environment for this tick
        assert(session.environment().ambientDarkness >= 4);

        const auto problem = session.trySleepInBed(world, host, {8, 1, 8});
        assert(problem == BedSleepProblem::None);
        assert(session.playerSleeping());
        // Both halves know.
        assert(world.state(8, 1, 8).occupied());
        assert(world.state(8, 1, 7).occupied());
        // And the bed is now home.
        assert(session.hasPlayerSpawn());
        assert(std::abs(session.playerSpawnPosition().z - 8.5F) < 0.001F);

        // A hundred ticks in bed and the night is over.
        const double before = session.dayTimeTicks();
        for (int tick = 0; tick < mc::gameplay::kSleepTicksBeforeSkip + 2; ++tick) {
            session.tick(world, host);
        }
        assert(!session.playerSleeping());
        assert(!world.state(8, 1, 8).occupied());
        assert(!world.state(8, 1, 7).occupied());
        assert(session.dayTimeTicks() != before);
        assert(session.environment().ambientDarkness < 4); // it is day now
    }

    // ---------------------------------------------------------------------
    // 5) Waking for any other reason must NOT skip the night: walk away and the
    //    clock keeps whatever it had.
    // ---------------------------------------------------------------------
    {
        World world = floored();
        layBed(world);
        mc::gameplay::GameSession session;
        session.setGameMode(mc::gameplay::GameMode::Survival);
        session.player().setPosition({8.5F, 1.0F, 8.5F});
        TestHost host;
        session.clocks().setTotalTicks(mc::world::ClockId::Overworld, 18000U);
        session.tick(world, host);
        assert(session.trySleepInBed(world, host, {8, 1, 8}) == BedSleepProblem::None);

        // Two ticks in, step well out of reach.
        session.tick(world, host);
        session.player().setPosition({8.5F, 1.0F, 15.5F});
        const double before = session.dayTimeTicks();
        session.tick(world, host);
        assert(!session.playerSleeping());
        assert(!world.state(8, 1, 8).occupied());
        // The clock advanced by the ordinary one tick, not by a night.
        assert(session.dayTimeTicks() - before < 5.0);
    }

    std::cout << "sleep_test passed\n";
    return 0;
}
