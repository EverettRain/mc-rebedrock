#include "gameplay/GameSession.hpp"
#include "gameplay/ItemPlacement.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "gameplay/entities/ZombieEntity.hpp"

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/DayNightCycle.hpp"
#include "world/World.hpp"
#include "world/WorldClock.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

// Exercises GameSession as a headless simulation unit: the fixed-tick loop
// integrates the player physics against a World, a simulated break scatters
// drops, and the death pipeline raises the host callback. No Vulkan, no window
// — this is what the renderer's old inline tick could never be tested as.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"game_session_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

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

// A single stone-floored chunk at the origin, the classic test bed.
void buildFloor(mc::world::World& world) {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
}

} // namespace

int main() {
    using namespace mc;

    // --- The player falls onto a floor and lands through the session's tick. ---
    world::World world;
    buildFloor(world);
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Survival);
    session.player().setPosition({8.5F, 3.0F, 8.5F});
    session.physicsPreviousPosition() = {8.5F, 3.0F, 8.5F};
    session.physicsCurrentPosition() = {8.5F, 3.0F, 8.5F};
    TestHost host;
    for (int tick = 0; tick < 20; ++tick) {
        session.tick(world, host);
    }
    assert(session.player().onGround());
    // Feet rest on the stone's top surface (y = 1), not embedded in it.
    assert(std::abs(session.player().position().y - 1.0F) < 0.05F);
    assert(session.physicsCurrentPosition().y == session.player().position().y);

    // Entity#move adds horizontalDistance * 0.6 to its step accumulator and
    // emits once per integer crossing. This catches the old 0.85-block stride,
    // which made an ordinary walk play almost twice as many footsteps.
    {
        world::World walkingWorld;
        buildFloor(walkingWorld);
        gameplay::GameSession walker;
        walker.setGameMode(gameplay::GameMode::Creative);
        walker.player().setPosition({8.5F, 1.001F, 12.5F});
        walker.physicsPreviousPosition() = walker.player().position();
        walker.physicsCurrentPosition() = walker.player().position();
        walker.input().forward = 1.0F;
        walker.input().lookDirection = {0.0F, 0.0F, -1.0F};
        // input() is the main thread's staging copy; the simulation reads what
        // commitInput() publishes. Without this the walker simply stands still
        // — and because `expected` is derived from the distance actually
        // travelled, the assertion below would still pass while testing
        // nothing at all.
        walker.commitInput();
        TestHost walkingHost;
        const glm::vec3 start = walker.player().position();
        for (int tick = 0; tick < 32; ++tick) {
            walker.tick(walkingWorld, walkingHost);
        }
        const glm::vec2 travelled{
            walker.player().position().x - start.x,
            walker.player().position().z - start.z,
        };
        // So pin that it moved, and that the step count is a real number.
        REQUIRE(glm::length(travelled) > 1.0F);
        const int expected = static_cast<int>(std::floor(glm::length(travelled) * 0.6F));
        REQUIRE(expected > 0);
        walker.drainEvents();
        REQUIRE(walkingHost.footsteps == expected);
    }

    // MobBrain attacks cross the EntitySystem event boundary and enter the
    // same player damage/death pipeline as every other source. REQUIRE remains
    // active under NDEBUG, so Release genuinely executes this integration.
    {
        gameplay::GameSession meleeSession;
        meleeSession.setGameMode(gameplay::GameMode::Survival);
        meleeSession.setDifficulty(gameplay::Difficulty::Normal);
        meleeSession.player().setPosition({8.5F, 1.001F, 8.5F});
        meleeSession.physicsPreviousPosition() = meleeSession.player().position();
        meleeSession.physicsCurrentPosition() = meleeSession.player().position();
        meleeSession.worldEntities().spawn({7.5F, 1.001F, 8.5F},
                                           gameplay::entities::ZombieEntity::type(), 51U);
        TestHost meleeHost;
        // The loop's exit condition reads the host, so each iteration has to
        // drain: events now queue until drained, and a stale count would run
        // the loop to completion and over-accumulate.
        for (int tick = 0; tick < 120 && meleeHost.playerHurts == 0; ++tick) {
            meleeSession.tick(world, meleeHost);
            meleeSession.drainEvents();
        }
        REQUIRE(meleeHost.playerHurts == 1);
        REQUIRE(std::abs(meleeSession.vitals().health() - 17.0F) < 0.001F);
        REQUIRE(meleeSession.vitals().damage().lastSource == gameplay::DamageType::EntityAttack);
    }

    // The in-world difficulty control goes through GameSession::setDifficulty.
    // Switching an already-running world to Peaceful must feed EntitySystem on
    // the next tick: hostile MONSTERs vanish silently while passive creatures
    // remain, and NaturalSpawner sees Peaceful in that same tick.
    {
        gameplay::GameSession peacefulSession;
        peacefulSession.player().setPosition({8.5F, 1.001F, 8.5F});
        peacefulSession.physicsPreviousPosition() = peacefulSession.player().position();
        peacefulSession.physicsCurrentPosition() = peacefulSession.player().position();
        peacefulSession.worldEntities().spawn({7.5F, 1.001F, 8.5F},
                                              gameplay::entities::ZombieEntity::type(), 61U);
        peacefulSession.worldEntities().spawn({9.5F, 1.001F, 8.5F},
                                              gameplay::entities::PigEntity::type(), 62U);
        const std::uint64_t zombieId = peacefulSession.worldEntities().entities()[0].id;
        const std::uint64_t pigId = peacefulSession.worldEntities().entities()[1].id;

        peacefulSession.setDifficulty(gameplay::Difficulty::Peaceful);
        TestHost peacefulHost;
        peacefulSession.tick(world, peacefulHost);

        REQUIRE(peacefulSession.worldEntities().byId(zombieId) == nullptr);
        REQUIRE(peacefulSession.worldEntities().byId(pigId) != nullptr);
    }

    // --- A broken block scatters its loot as item entities. ---
    // Dirt always drops itself even with an empty hand; stone would need a
    // pickaxe, so dirt keeps the drop path deterministic.
    world.setBlock(9, 1, 9, world::Block::Dirt);
    const std::size_t dropsBefore = session.itemEntities().entities().size();
    session.spawnBlockDrops({9, 1, 9}, world::BlockState{world::Block::Dirt},
                            gameplay::ItemStack{});
    assert(session.itemEntities().entities().size() > dropsBefore);

    // A falling block performs two render-critical handoffs: static mesh to
    // entity at takeoff, then entity back to static mesh on landing. Both edits
    // must request the renderer's immediate preview path.
    {
        world::World fallingWorld;
        buildFloor(fallingWorld);
        // Fourteen blocks above a one-layer floor is the first exact discrete
        // fall that the old endpoint-only collision check skipped completely.
        fallingWorld.setBlock(4, 14, 4, world::Block::Sand);
        gameplay::GameSession fallingSession;
        fallingSession.worldSimulation().notifyPlaced({4, 14, 4}, world::Block::Sand);
        TestHost fallingHost;
        for (int tick = 0; tick < 80; ++tick) {
            fallingSession.tick(fallingWorld, fallingHost);
        }
        REQUIRE(fallingWorld.block(4, 1, 4) == world::Block::Sand);
        fallingSession.drainEvents();
        REQUIRE(fallingHost.previewEdits == 2);
    }

    // A non-colliding but non-replaceable state at the landing cell prevents
    // placement. The falling block must become an item without deleting that
    // state or submitting a fake world edit/break effect for it.
    {
        world::World occupiedWorld;
        world::Chunk occupiedChunk;
        occupiedChunk.setBlock(4, 0, 4, world::Block::Farmland);
        occupiedChunk.setBlock(4, 1, 4, world::Block::WheatCrops);
        occupiedChunk.setBlock(4, 14, 4, world::Block::Sand);
        occupiedWorld.setChunk({0, 0}, std::move(occupiedChunk));
        gameplay::GameSession occupiedSession;
        occupiedSession.setGameMode(gameplay::GameMode::Creative);
        occupiedSession.player().setPosition({12.5F, 2.0F, 12.5F});
        occupiedSession.worldSimulation().notifyPlaced({4, 14, 4}, world::Block::Sand);
        TestHost occupiedHost;
        for (int tick = 0; tick < 80; ++tick) {
            occupiedSession.tick(occupiedWorld, occupiedHost);
        }
        REQUIRE(occupiedWorld.block(4, 1, 4) == world::Block::WheatCrops);
        REQUIRE(std::ranges::any_of(
            occupiedSession.itemEntities().entities(), [](const gameplay::ItemEntity& entity) {
                return entity.stack.block == world::Block::Sand;
            }));
        occupiedSession.drainEvents();
        REQUIRE(occupiedHost.blockBreaks == 0);
        occupiedSession.drainEvents();
        REQUIRE(occupiedHost.previewEdits == 1);
    }

    // --- The kill pipeline raises the death host callback. ---
    TestHost deathHost;
    assert(session.hurtPlayer(gameplay::DamageType::OutOfWorld, 1000.0F, deathHost));
    session.drainEvents();
    assert(deathHost.playerDied);

    // --- Eating a meal completes through tickEating inside the tick. ---
    // A creative player runs the full meal without spending food; the eating
    // animation lifecycle still fires on the host. The meal is driven through
    // the interaction now: holding the use button (a UseItem command) starts it,
    // and it stays held until the meal finishes itself.
    TestHost eatHost;
    gameplay::GameSession eater;
    eater.setGameMode(gameplay::GameMode::Creative);
    // The meal must be in hand when it finishes, or the final burst is skipped.
    eater.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Apple};
    eater.inventory().selectHotbar(0);
    eater.enqueueCommand(gameplay::UseItem{});
    world::World eatWorld;
    buildFloor(eatWorld);
    // Hold use through the meal. It lands on the last tick; the release command
    // arrives in that same tick so the held use does not start a second meal.
    for (int tick = 0; tick < gameplay::GameSession::kEatTicks; ++tick) {
        eater.tick(eatWorld, eatHost);
    }
    eater.enqueueCommand(gameplay::UseItemStop{});
    eater.tick(eatWorld, eatHost);
    eater.drainEvents();
    assert(eatHost.eatingStarted == 1);
    // The meal ran to completion and cancelled itself.
    assert(!eater.eating());
    eater.drainEvents();
    assert(eatHost.eatingCancelled == 1);
    // The chew loop played through the meal: six chew ticks (remaining 24..4,
    // every fourth tick) plus the final burst = seven generic.eat sounds, then
    // the burp.
    eater.drainEvents();
    assert(eatHost.eatSounds == 7);

    // --- Buckets: the item resolves collect/pour, and replaceSelected swaps hands. ---
    {
        world::World bucketWorld;
        buildFloor(bucketWorld);
        constexpr int waterX = 3;
        constexpr int waterY = 1;
        constexpr int waterZ = 3;
        bucketWorld.setBlock(waterX, waterY, waterZ, world::Block::Water);
        bucketWorld.setFluidLevel(waterX, waterY, waterZ, 0U);

        gameplay::GameSession bucketSession;
        // Empty bucket on a still water source resolves to CollectWater.
        const world::PlacementContext onWater{{waterX, waterY, waterZ}, {waterX, waterY, waterZ}};
        const auto collect = gameplay::itemUseOn(&gameplay::items::Bucket, bucketWorld, onWater);
        assert(collect.action == gameplay::ItemUseAction::CollectWater);
        // A water bucket on a replaceable cell resolves to PlaceWater.
        const world::PlacementContext ontoAir{{waterX, waterY, waterZ + 2},
                                              {waterX, waterY, waterZ + 3}};
        const auto pour = gameplay::itemUseOn(&gameplay::items::WaterBucket, bucketWorld, ontoAir);
        assert(pour.action == gameplay::ItemUseAction::PlaceWater);
        const auto pourLava = gameplay::itemUseOn(
            &gameplay::items::LavaBucket, bucketWorld, ontoAir);
        assert(pourLava.action == gameplay::ItemUseAction::PlaceLava);
        bucketWorld.setBlock(waterX + 2, waterY, waterZ, world::Block::Lava);
        const world::PlacementContext onLava{{waterX + 2, waterY, waterZ},
                                             {waterX + 2, waterY, waterZ}};
        const auto collectLava = gameplay::itemUseOn(
            &gameplay::items::Bucket, bucketWorld, onLava);
        assert(collectLava.action == gameplay::ItemUseAction::CollectLava);
        // Non-water blocks never collect, and a solid cell never pours.
        const world::PlacementContext onStone{{waterX, 0, waterZ}, {waterX, 0, waterZ}};
        const auto noCollect = gameplay::itemUseOn(&gameplay::items::Bucket, bucketWorld, onStone);
        assert(noCollect.action == gameplay::ItemUseAction::Nothing);

        // replaceSelected swaps the selected hotbar slot in place, the way the
        // survival collect/pour branches turn bucket into water bucket and back.
        auto& inventory = bucketSession.inventory();
        inventory.mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Bucket};
        inventory.replaceSelected({world::Block::Air, 1U, &gameplay::items::WaterBucket});
        assert(inventory.selectedStack().item == &gameplay::items::WaterBucket);
        inventory.replaceSelected({world::Block::Air, 1U, &gameplay::items::Bucket});
        assert(inventory.selectedStack().item == &gameplay::items::Bucket);
    }

    // --- The unified die() raises the death screen exactly once per death. ---
    {
        TestHost onceHost;
        gameplay::GameSession dying;
        dying.setGameMode(gameplay::GameMode::Survival);
        assert(dying.hurtPlayer(gameplay::DamageType::Fall, 1000.0F, onceHost));
        dying.drainEvents();
        assert(onceHost.playerDied);
        // A second lethal source in the same tick is swallowed by the dead()
        // guard, and die() refuses to re-claim the already-claimed death.
        assert(!dying.hurtPlayer(gameplay::DamageType::Drown, 1000.0F, onceHost));
        assert(!dying.die(gameplay::DamageType::Drown, onceHost));
        dying.drainEvents();
        assert(onceHost.playerDied);
    }

    // --- Respawn prefers the /spawnpoint result before the world spawn. ---
    {
        gameplay::GameSession respawner;
        respawner.worldSpawnPosition() = {10.0F, 64.0F, 10.0F};
        respawner.playerSpawnPosition() = {99.0F, 65.0F, 99.0F};
        respawner.hasPlayerSpawn() = true;
        respawner.respawn();
        const auto personal = respawner.player().position();
        assert(personal.x == 99.0F && personal.y == 65.0F && personal.z == 99.0F);
        // Without a personal spawn point, death falls back to the world spawn.
        respawner.hasPlayerSpawn() = false;
        respawner.player().setPosition({1.0F, 1.0F, 1.0F});
        respawner.respawn();
        const auto fallback = respawner.player().position();
        assert(fallback.x == 10.0F && fallback.y == 64.0F && fallback.z == 10.0F);
    }

    // --- Respawn clears the dying body's state, not just the position. ---
    {
        gameplay::GameSession respawner;
        respawner.worldSpawnPosition() = {5.0F, 64.0F, 5.0F};
        respawner.hasPlayerSpawn() = false;
        respawner.player().setPosition({1.0F, 1.0F, 1.0F});
        // The death momentum, fall height and a drained vitals bar all carry
        // into respawn unless the new body is reset.
        respawner.player().applyExternalPush({5.0F, 3.0F, 5.0F});
        respawner.vitals().restore(1.0F, 0, 0.0F, 0);
        respawner.respawn();
        const auto feet = respawner.player().position();
        assert(feet.x == 5.0F && feet.y == 64.0F && feet.z == 5.0F);
        assert(respawner.player().velocity() == glm::vec3{0.0F});
        assert(respawner.player().fallDistance() == 0.0F);
        assert(!respawner.player().flying());
        assert(!respawner.player().sneaking());
        assert(respawner.vitals().health() == gameplay::PlayerVitals::kMaximumHealth);
        assert(respawner.vitals().foodLevel() == gameplay::PlayerVitals::kMaximumFood);
        assert(!respawner.vitals().dead());
    }

    // --- The clocks are separate from the world tick. ---
    // The bug this pins: one gameTimeSeconds used to carry the sun, mining
    // progress, use cooldowns, chat expiry and the cursor blink at once, and it
    // only advanced while doDaylightCycle was on. Turning the sun off therefore
    // froze mining and stranded every cooldown. serverTick answers to nothing.
    {
        gameplay::GameSession clockSession;
        const auto startDay = clockSession.dayTimeTicks();
        REQUIRE(clockSession.serverTick() == 0U);
        // A fresh world opens at morning, not at tick zero.
        REQUIRE(startDay == static_cast<std::uint64_t>(world::DayNightCycle::kNewWorldTick));

        static_cast<void>(
            clockSession.gameRules().set(gameplay::GameRuleId::DoDaylightCycle, false));
        for (int tick = 0; tick < 40; ++tick) {
            clockSession.tick(world, host);
        }
        REQUIRE(clockSession.serverTick() == 40U);
        REQUIRE(clockSession.dayTimeTicks() == startDay);

        // Turning the sun back on resumes it from where it stopped, while the
        // world tick simply carries on.
        static_cast<void>(
            clockSession.gameRules().set(gameplay::GameRuleId::DoDaylightCycle, true));
        for (int tick = 0; tick < 25; ++tick) {
            clockSession.tick(world, host);
        }
        REQUIRE(clockSession.serverTick() == 65U);
        REQUIRE(clockSession.dayTimeTicks() == startDay + 25U);
    }

    // A clock's rate scales only that clock. Half speed halves the sun and
    // leaves the world tick alone; the partial tick carries the remainder so
    // no time is lost to rounding.
    {
        gameplay::GameSession rateSession;
        const auto startDay = rateSession.dayTimeTicks();
        rateSession.clocks().setRate(world::ClockId::Overworld, 0.5F);
        for (int tick = 0; tick < 10; ++tick) {
            rateSession.tick(world, host);
        }
        REQUIRE(rateSession.serverTick() == 10U);
        REQUIRE(rateSession.dayTimeTicks() == startDay + 5U);
    }

    // `/time set day|noon|night|midnight` moves forward to the next occurrence
    // and can never wind a clock backwards — winding back is what used to
    // strand the use cooldown in the future and freeze right-click entirely.
    {
        gameplay::GameSession markerSession;
        auto& clocks = markerSession.clocks();
        clocks.setTotalTicks(world::ClockId::Overworld, 18'000U); // midnight
        clocks.moveToTimeMarker(world::ClockId::Overworld, world::ClockTimeMarker::Day);
        // Morning is 1000, already behind midnight, so it resolves into the
        // next day rather than back to 1000.
        REQUIRE(markerSession.dayTimeTicks() == 25'000U);
        REQUIRE(markerSession.dayTimeTicks() % 24'000U == 1'000U);

        // A marker that sits exactly on the current tick advances a whole day
        // rather than standing still.
        clocks.setTotalTicks(world::ClockId::Overworld, 6'000U);
        clocks.moveToTimeMarker(world::ClockId::Overworld, world::ClockTimeMarker::Noon);
        REQUIRE(markerSession.dayTimeTicks() == 30'000U);

        // /time add never drives a clock below zero.
        clocks.setTotalTicks(world::ClockId::Overworld, 100U);
        clocks.addTicks(world::ClockId::Overworld, -500);
        REQUIRE(markerSession.dayTimeTicks() == 0U);
    }

    std::cout << "game_session: player landing, block drops, death pipeline, eating, buckets, "
                 "spawnpoint, clocks OK\n";
    return 0;
}
