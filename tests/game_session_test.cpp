#include "gameplay/GameSession.hpp"

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <optional>
#include <utility>

// Exercises GameSession as a headless simulation unit: the fixed-tick loop
// integrates the player physics against a World, a simulated break scatters
// drops, and the death pipeline raises the host callback. No Vulkan, no window
// — this is what the renderer's old inline tick could never be tested as.

namespace {

struct TestHost final : mc::gameplay::SimulationHost {
    int worldEdits = 0;
    int blockBreaks = 0;
    int itemPickups = 0;
    int footsteps = 0;
    bool playerDied = false;
    int furnaceChanges = 0;
    int eatingStarted = 0;
    int eatingCancelled = 0;

    void submitWorldEdit(int, int, int, mc::world::Block, std::uint8_t,
                         std::optional<mc::world::BlockOrientation>) override {
        ++worldEdits;
    }
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(mc::world::Block, glm::vec3) override { ++blockBreaks; }
    void playItemPickup(glm::vec3) override { ++itemPickups; }
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, float) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(glm::vec3) override {}
    void playCreatureDeath(glm::vec3) override {}
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

    // --- A broken block scatters its loot as item entities. ---
    // Dirt always drops itself even with an empty hand; stone would need a
    // pickaxe, so dirt keeps the drop path deterministic.
    world.setBlock(9, 1, 9, world::Block::Dirt);
    const std::size_t dropsBefore = session.itemEntities().entities().size();
    session.spawnBlockDrops({9, 1, 9}, world::Block::Dirt, gameplay::ItemStack{});
    assert(session.itemEntities().entities().size() > dropsBefore);

    // --- The kill pipeline raises the death host callback. ---
    TestHost deathHost;
    assert(session.hurtPlayer(gameplay::DamageSource::OutOfWorld, 1000.0F, deathHost));
    assert(deathHost.playerDied);

    // --- Eating a meal completes through tickEating inside the tick. ---
    // A creative player runs the full meal without spending food; the eating
    // animation lifecycle still fires on the host.
    TestHost eatHost;
    gameplay::GameSession eater;
    eater.setGameMode(gameplay::GameMode::Creative);
    eater.beginEating(&gameplay::items::Apple, eatHost);
    assert(eatHost.eatingStarted == 1);
    world::World eatWorld;
    buildFloor(eatWorld);
    for (int tick = 0; tick < gameplay::GameSession::kEatTicks + 1; ++tick) {
        eater.tick(eatWorld, eatHost);
    }
    // The meal ran to completion and cancelled itself.
    assert(!eater.eating());
    assert(eatHost.eatingCancelled == 1);

    std::cout << "game_session: player landing, block drops, death pipeline, eating OK\n";
    return 0;
}
