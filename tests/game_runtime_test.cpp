// N0's headless full-chain test: proves GameRuntime runs the whole gameplay
// pipeline — create a world, pull its spawn chunk, mutate it through
// WorldMutationService, advance ticks, save, destroy, reload — with no GLFW or
// Vulkan anywhere. This is the functional ancestor of the dedicated server.
//
// The SimulationHost is a recording stub: it drops every world edit into the
// runtime's open save, mirroring how the renderer's host writes
// currentSave->edits, so edits persist across the save/reload boundary.

#include "runtime/GameRuntime.hpp"

#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/PersistentBlockEdit.hpp"
#include "world/WorldMutationService.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

using namespace mc;

namespace {

// The render-side reactions a headless run does not need. Everything except the
// world-edit persistence is a no-op; the two edit methods record into the
// runtime's open save exactly the way the renderer's host does, so a block
// placed through WorldMutationService survives a save and reload.
struct RecordingHost final : public gameplay::SimulationHost {
    std::optional<persistence::SaveGame>* save = nullptr;
    std::size_t submittedEdits = 0U;
    std::size_t submittedStateEdits = 0U;

    void remember(world::PersistentBlockEdit edit) {
        if (save == nullptr || !save->has_value()) {
            return;
        }
        auto& edits = (*save)->edits;
        for (auto& existing : edits) {
            if (existing.x == edit.x && existing.y == edit.y && existing.z == edit.z) {
                existing = edit;
                return;
            }
        }
        edits.push_back(edit);
    }

    void submitWorldEdit(int x, int y, int z, world::Block block, std::uint8_t fluidLevel,
                         std::optional<world::BlockOrientation> orientation) override {
        const auto resolved = orientation.value_or(world::defaultOrientation(block));
        remember({x, y, z, world::BlockState{block, resolved, fluidLevel}});
        ++submittedEdits;
    }
    void submitWorldStateEdit(int x, int y, int z, world::BlockState state) override {
        remember({x, y, z, state});
        ++submittedStateEdits;
    }
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
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
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

// Applies a streamer batch to the runtime world, the headless analogue of the
// renderer's queueStreamBatch: chunk data lands on the gameplay world, and
// state updates only land when the CAS expectation still holds.
void applyBatch(runtime::GameRuntime& runtime, const world::ChunkStreamBatch& batch) {
    if (batch.worldEpoch != runtime.worldEpoch()) {
        return;
    }
    for (const auto& update : batch.chunkUpdates) {
        if (update.remove) {
            runtime.world().removeChunk(update.position);
        } else {
            runtime.world().setChunk(update.position, update.chunk);
        }
    }
    for (const auto& update : batch.stateUpdates) {
        if (runtime.world().state(update.worldX, update.y, update.worldZ) == update.expected) {
            static_cast<void>(
                runtime.world().setState(update.worldX, update.y, update.worldZ, update.state));
        }
    }
}

}  // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();

    const auto saveRoot = std::filesystem::temp_directory_path() / "game-runtime-test";
    std::filesystem::remove_all(saveRoot);
    std::filesystem::create_directories(saveRoot);

    std::string worldId;
    std::uint64_t savedServerTick = 0U;
    world::ClockState savedOverworldClock{};
    std::size_t savedChestCount = 0U;
    std::size_t savedEntityCount = 0U;
    std::size_t serverResident = 0U;

    // Phase 1: create, load, mutate, tick, save.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();

        const auto seed = std::uint64_t{0x1234'5678'9ABC'DEF0ULL};
        auto save = runtime.createWorld("headless", seed, gameplay::GameMode::Creative);
        worldId = save.summary.identifier;
        runtime.loadWorld(std::move(save), /*viewDistanceChunks=*/4);

        // Pull the spawn chunk synchronously (player defaults to 24,?,24 -> chunk 1,1).
        const auto batch = streamer.requestSync({1, 1}, std::chrono::seconds(10));
        assert(batch.has_value());
        applyBatch(runtime, *batch);
        assert(runtime.world().chunkCount() >= 1U);

        // Place a stone block and a chest through the mutation service, the same
        // path a player break/place uses.
        world::WorldMutationService mutations;
        gameplay::GameplayMutationSink sink{runtime.world(), runtime.gameSession()};
        static_cast<void>(mutations.setBlock(
            runtime.world(), {24, 80, 24}, world::BlockState{world::Block::Stone},
            world::MutationFlags::All, world::MutationCause::PlayerPlace, sink));
        static_cast<void>(mutations.setBlock(
            runtime.world(), {24, 81, 24}, world::BlockState{world::Block::Chest},
            world::MutationFlags::All, world::MutationCause::PlayerPlace, sink));

        // A live creature, restored the way loadWorld restores a saved herd.
        const auto* pigType = gameplay::entities::entityTypeRegistry().byId("pig");
        assert(pigType != nullptr);
        runtime.gameSession().worldEntities().restore({24.0F, 83.0F, 24.0F}, *pigType, 0.0F,
                                                      {0.0F, 0.0F, 0.0F}, 10.0F, 0, 0, 12345U);

        // Flush the queued world-edit events to the host (under the write
        // section, the same discipline the renderer's drain pass keeps).
        {
            const auto drainWrite = runtime.lock().write();
            static_cast<void>(runtime.gameSession().drainEvents());
        }
        assert(host.submittedStateEdits >= 2U);

        // Advance a handful of ticks synchronously (headless drives tick()
        // directly; the threaded form is exercised by the smoke test).
        for (int tick = 0; tick < 10; ++tick) {
            runtime.tick();
        }

        runtime.save();
        assert(runtime.currentSave().serverTick > 0U);
        savedServerTick = runtime.currentSave().serverTick;
        savedOverworldClock =
            runtime.currentSave().clocks[static_cast<std::size_t>(world::ClockId::Overworld)];
        savedChestCount = runtime.currentSave().chests.size();
        savedEntityCount = runtime.currentSave().entities.size();
        assert(savedChestCount == 1U);
        assert(savedEntityCount == 1U);

        // N-Mem, first piece: measure the server-side resident bytes headless
        // (no render allocation exists here) and pin an upper bound.
        serverResident = runtime.serverResidentBytes();
        const auto chunks = runtime.world().chunkCount();
        std::cout << "serverResidentBytes=" << serverResident << " over " << chunks << " chunks "
                  << "(~" << (chunks > 0U ? serverResident / chunks : 0U) << " B/chunk)\n";
        assert(serverResident > 0U);
        assert(chunks >= 1U);
        // A single-chunk headless world is ~40 KB; a megabyte catches a gross
        // leak without being sensitive to terrain variation.
        assert(serverResident < 1024U * 1024U);

        runtime.stopSimulation();
    }  // runtime + streamer destroyed here.

    // Phase 2: a fresh runtime reloads the same save and the mutations survived.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();

        auto save = runtime.saveRepository().load(worldId);
        runtime.loadWorld(std::move(save), /*viewDistanceChunks=*/4);
        const auto batch = streamer.requestSync({1, 1}, std::chrono::seconds(10));
        assert(batch.has_value());
        applyBatch(runtime, *batch);

        // The placed stone block survived the save/reload.
        assert(runtime.world().state(24, 80, 24).block() == world::Block::Stone);

        // The chest block entity survived.
        bool foundChest = false;
        for (const auto& chest : runtime.gameSession().chestSystem().entities()) {
            if (chest.position == gameplay::ChestPosition{24, 81, 24}) {
                foundChest = true;
                break;
            }
        }
        assert(foundChest);
        assert(runtime.currentSave().chests.size() == savedChestCount);

        // The creature survived with its species resolved.
        bool foundPig = false;
        for (const auto& entity : runtime.gameSession().worldEntities().entities()) {
            if (entity.type != nullptr && std::string{entity.type->id().path} == "pig") {
                foundPig = true;
                break;
            }
        }
        assert(foundPig);
        assert(runtime.currentSave().entities.size() == savedEntityCount);

        // World time round-tripped exactly.
        assert(runtime.gameSession().serverTick() == savedServerTick);
        assert(runtime.gameSession().clocks().state(world::ClockId::Overworld) ==
               savedOverworldClock);

        // The reloaded runtime still drives ticks headless.
        runtime.tick();
        assert(runtime.gameSession().serverTick() == savedServerTick + 1U);

        runtime.stopSimulation();
    }

    // A chat command runs server-side on the runtime's dispatcher.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();
        auto save = runtime.createWorld("chat", 42U, gameplay::GameMode::Creative);
        runtime.loadWorld(std::move(save), 4);
        runtime.enqueueChat("/gamemode survival");
        runtime.tick();
        const auto result = runtime.takeChatResult();
        assert(result.has_value());
        assert(result->success);
        assert(runtime.gameSession().gameMode() == gameplay::GameMode::Survival);
        // A /give command hands the requested stack over.
        runtime.enqueueChat("/give 0 1");
        runtime.tick();
        const auto giveResult = runtime.takeChatResult();
        assert(giveResult.has_value());
        assert(giveResult->success);
        const bool hasGrass = std::ranges::any_of(
            runtime.gameSession().inventory().slots(), [](const gameplay::ItemStack& stack) {
                return stack.block == world::Block::Grass && stack.count >= 1U;
            });
        assert(hasGrass);
        // The weather command tree includes all three vanilla modes. Thunder
        // must raise both flags and convert the optional seconds to game ticks.
        runtime.enqueueChat("/weather thunder 10");
        runtime.tick();
        const auto thunderResult = runtime.takeChatResult();
        assert(thunderResult.has_value());
        assert(thunderResult->success);
        assert(runtime.gameSession().weatherSystem().state().raining);
        assert(runtime.gameSession().weatherSystem().state().thundering);
        assert(runtime.gameSession().weatherSystem().state().rainTime == 200);
        assert(runtime.gameSession().weatherSystem().state().thunderTime == 200);
        runtime.stopSimulation();
    }

    std::filesystem::remove_all(saveRoot);
    std::cout << "PASS: game_runtime_test\n";
    return 0;
}
