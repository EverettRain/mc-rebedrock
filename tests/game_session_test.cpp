#include "gameplay/GameSession.hpp"
#include "gameplay/ItemPlacement.hpp"

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
    int eatSounds = 0;

    void submitWorldEdit(int, int, int, mc::world::Block, std::uint8_t,
                         std::optional<mc::world::BlockOrientation>) override {
        ++worldEdits;
    }
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(mc::world::Block, glm::vec3) override { ++blockBreaks; }
    void playItemPickup(glm::vec3) override { ++itemPickups; }
    void playEat(glm::vec3) override { ++eatSounds; }
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
    // The meal must be in hand when it finishes, or the final burst is skipped.
    eater.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Apple};
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
    // The chew loop played through the meal: six chew ticks (remaining 24..4,
    // every fourth tick) plus the final burst = seven generic.eat sounds, then
    // the burp.
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
        const world::PlacementContext onWater{
            {waterX, waterY, waterZ}, {waterX, waterY, waterZ}};
        const auto collect =
            gameplay::itemUseOn(&gameplay::items::Bucket, bucketWorld, onWater);
        assert(collect.action == gameplay::ItemUseAction::CollectWater);
        // A water bucket on a replaceable cell resolves to PlaceWater.
        const world::PlacementContext ontoAir{
            {waterX, waterY, waterZ + 2}, {waterX, waterY, waterZ + 3}};
        const auto pour =
            gameplay::itemUseOn(&gameplay::items::WaterBucket, bucketWorld, ontoAir);
        assert(pour.action == gameplay::ItemUseAction::PlaceWater);
        // Non-water blocks never collect, and a solid cell never pours.
        const world::PlacementContext onStone{{waterX, 0, waterZ}, {waterX, 0, waterZ}};
        const auto noCollect =
            gameplay::itemUseOn(&gameplay::items::Bucket, bucketWorld, onStone);
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
        assert(dying.hurtPlayer(gameplay::DamageSource::Fall, 1000.0F, onceHost));
        assert(onceHost.playerDied);
        // A second lethal source in the same tick is swallowed by the dead()
        // guard, and die() refuses to re-claim the already-claimed death.
        assert(!dying.hurtPlayer(gameplay::DamageSource::Drown, 1000.0F, onceHost));
        assert(!dying.die(gameplay::DamageSource::Drown, onceHost));
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

    std::cout << "game_session: player landing, block drops, death pipeline, eating, buckets, spawnpoint OK\n";
    return 0;
}
