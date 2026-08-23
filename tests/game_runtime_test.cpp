// N0's headless full-chain test: proves GameRuntime runs the whole gameplay
// pipeline — create a world, pull its spawn chunk, mutate it through
// WorldMutationService, advance ticks, save, destroy, reload — with no GLFW or
// Vulkan anywhere. This is the functional ancestor of the dedicated server.
//
// The SimulationHost is a recording stub: it drops every world edit into the
// runtime's open save, mirroring how the renderer's host writes
// currentSave->edits, so edits persist across the save/reload boundary.

#include "runtime/GameRuntime.hpp"

#include "client/ClientMirror.hpp"
#include "net/LoopbackTransport.hpp"
#include "net/NetMessage.hpp"
#include "net/Transport.hpp"

#include <glm/geometric.hpp>

#include "gameplay/Difficulty.hpp"
#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/NaturalSpawner.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/PersistentBlockEdit.hpp"
#include "world/WorldMutationService.hpp"
#include "world/gen/Biome.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
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
    // A distinctive, non-default position the player is moved to before saving,
    // so a reloaded world's live and snapshot positions are both pinned here.
    const glm::vec3 savedPlayerPos{25.5F, 65.0F, 20.5F};

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

        // Stage C slice 1b: the client's intent travels the in-process loopback
        // channel, not a direct enqueueCommand. Ship a hotbar change on the
        // client end; the next tick drains the server end into the session and
        // applies it, with no added latency (the frame is already queued when the
        // tick runs). A command enqueued between ticks lands on the next one.
        assert(runtime.gameSession().inventory().selectedHotbarSlot() != 4U);
        runtime.enqueueClientCommand(gameplay::SwapSlot{4U});
        // Not yet consumed: no tick has drained the channel.
        assert(runtime.gameSession().inventory().selectedHotbarSlot() != 4U);
        runtime.tick();
        assert(runtime.gameSession().inventory().selectedHotbarSlot() == 4U);

        // CMD2: a chat command runs against a CommandSource built from the primary
        // player (op4 owner). `/spawnpoint ~ ~ ~` resolves the relative axes
        // through the one resolve() against that source's authoritative position,
        // proving the source is threaded into the handler and the feedback routes
        // back to the chat result.
        {
            const glm::vec3 playerPos = runtime.gameSession().player().position();
            runtime.enqueueChat("/spawnpoint ~ ~ ~");
            runtime.tick();
            const auto spawnResult = runtime.takeChatResult();
            assert(spawnResult.has_value() && spawnResult->success);
            const glm::vec3 spawn = runtime.gameSession().playerSpawnPosition();
            assert(std::fabs(spawn.x - playerPos.x) < 1e-3F &&
                   std::fabs(spawn.y - playerPos.y) < 1e-3F &&
                   std::fabs(spawn.z - playerPos.z) < 1e-3F);

            // Feedback is gated by sendCommandFeedback: a successful command is
            // silent when the rule is off, but a failure always reports.
            static_cast<void>(runtime.gameSession().gameRules().set<bool>(
                gameplay::GameRuleId::SendCommandFeedback, false));
            runtime.enqueueChat("/spawnpoint ~ ~ ~");
            runtime.tick();
            assert(!runtime.takeChatResult().has_value()); // a success is silenced
            runtime.enqueueChat("/notacommand");
            runtime.tick();
            const auto failResult = runtime.takeChatResult();
            assert(failResult.has_value() && !failResult->success); // failures still report
            static_cast<void>(runtime.gameSession().gameRules().set<bool>(
                gameplay::GameRuleId::SendCommandFeedback, true)); // restore the default
        }

        // Advance a handful of ticks synchronously (headless drives tick()
        // directly; the threaded form is exercised by the smoke test).
        for (int tick = 0; tick < 10; ++tick) {
            runtime.tick();
        }

        // Stage C slice 1b-2: the server encodes the player/world snapshots onto
        // the channel every tick; a client mirror decodes the newest into itself,
        // the ClientLevel-equivalent the renderer will read from. Pumping the
        // client end drains the ticks above and keeps the latest, which must
        // match what the authoritative session just published — byte round trip,
        // no direct snapshot read. Metering the encoded size pins the per-tick
        // serialization cost so a regression that would drag the tick is caught.
        {
            // Place a block so a WorldEditEvent is guaranteed on the channel this
            // tick — the mirror pump must carry events (C-1b-3), not just
            // snapshots. The edit reaches the host through the channel, decoded
            // and applied by applyGameEvent instead of a direct drainEvents.
            static_cast<void>(mutations.setBlock(
                runtime.world(), {26, 80, 26}, world::BlockState{world::Block::Stone},
                world::MutationFlags::All, world::MutationCause::PlayerPlace, sink));
            runtime.tick();

            RecordingHost mirrorHost;  // events apply here, isolated from the save's host
            client::ClientMirror mirror;
            const auto applied = mirror.pump(runtime.clientChannel(), mirrorHost);
            assert(applied >= 3U);  // the edit event + the last tick's player + world
            // The world edit round-tripped the channel and reached the host.
            assert(mirrorHost.submittedStateEdits >= 1U);
            assert(mirror.player() == runtime.gameSession().playerTickSnapshot());
            assert(mirror.world() == runtime.gameSession().worldSnapshot());
            // The entity snapshot (the restored pig) round-tripped into the
            // mirror too: same creatures the session published this tick.
            assert(mirror.entities().entities() ==
                   runtime.gameSession().entitySnapshot().entities());
            assert(!mirror.entities().entities().empty());  // the pig is there
            // The per-tick snapshot encoding is non-trivial but bounded (the
            // player POD plus the world's weather/clocks/container display).
            assert(runtime.lastSnapshotEncodedBytes() > 0U);
            assert(runtime.lastSnapshotEncodedBytes() < 64U * 1024U);
            std::cout << "snapshotEncodedBytes/tick=" << runtime.lastSnapshotEncodedBytes()
                      << "\n";
            // A drained channel yields nothing more.
            assert(mirror.pump(runtime.clientChannel(), mirrorHost) == 0U);
        }

        // Move the player to a distinctive position first, so the save carries
        // clearly non-default coordinates for the reload assertions below.
        runtime.gameSession().teleportPlayer(gameplay::kPrimaryPlayerId, savedPlayerPos);
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
        // Per chunk, not in total: how many chunks the streaming worker has
        // finished by this line is a race (1 on an idle machine, the full 24 of
        // the view window under load), so an absolute ceiling failed roughly one
        // run in six on a busy box. A chunk is ~82 KB here; a 256 KB per-chunk
        // bound still catches a gross leak and no longer depends on the worker's
        // timing.
        assert(serverResident / chunks < 256U * 1024U);

        // M-Chunk B-5 delta principle: the edits the simulation publishes carry
        // the full state, so a consumer (the renderer's client chunk cache) fed
        // the same drained events reconstructs the same world the server ticks.
        // Without the state reaching the cache, the render mesh and the server
        // would silently diverge. The cache starts from the generated chunks (as
        // the renderer's does, fed by the streamer batches) then applies edits.
        {
            world::World cacheProbe;
            for (int cx = -1; cx <= 1; ++cx) {
                for (int cz = -1; cz <= 1; ++cz) {
                    if (const auto* chunk = runtime.world().chunk({cx, cz}); chunk != nullptr) {
                        cacheProbe.setChunk({cx, cz}, *chunk);
                    }
                }
            }
            for (const auto& edit : runtime.currentSave().edits) {
                static_cast<void>(cacheProbe.setState(edit.x, edit.y, edit.z, edit.state));
            }
            assert(cacheProbe.state(24, 80, 24).block() == world::Block::Stone);
            assert(cacheProbe.state(24, 81, 24).block() == world::Block::Chest);
            assert(cacheProbe.state(24, 80, 24).block() ==
                   runtime.world().state(24, 80, 24).block());
        }

        // M-2b side-split memory: the world's resident measures its chunk data
        // (states + light + biomes), and the client cache fed from the same
        // batches and edits mirrors the server's chunk budget exactly.
        assert(runtime.world().residentBytes() > 64U * 1024U);
        assert(runtime.world().residentBytes() < 8U * 1024U * 1024U);
        {
            world::World mirrorProbe;
            // Every chunk the server holds, not a hardcoded 3x3 window: how far
            // the streaming worker has got by this line is a race, and mirroring
            // fewer chunks than the server holds made the byte comparison below
            // fail on a busy machine.
            for (const auto position : runtime.world().positions()) {
                if (const auto* chunk = runtime.world().chunk(position); chunk != nullptr) {
                    mirrorProbe.setChunk(position, *chunk);
                }
            }
            for (const auto& edit : runtime.currentSave().edits) {
                static_cast<void>(mirrorProbe.setState(edit.x, edit.y, edit.z, edit.state));
            }
            // Within a small tolerance: the two worlds hold the same chunk data,
            // and only map/light padding differs by a few dozen bytes.
            assert(mirrorProbe.residentBytes() + 256U >= runtime.world().residentBytes());
            assert(runtime.world().residentBytes() + 256U >= mirrorProbe.residentBytes());
        }

        runtime.stopSimulation();
    }  // runtime + streamer destroyed here.

    // Phase 1.5 (D0 movement over the channel): isolated in its own runtime so
    // the extra ticks it drives do not perturb Phase 1's server-resident bound.
    // The client's continuous movement intent travels the channel, but the server
    // stages it on the player *before* the tick rather than queuing it for the
    // late command drain — the renderer used to write gameSession().input()
    // directly, which a cross-process client has no session to do. A forward walk
    // moves the player in its look direction, and the server derives the gated
    // fields (flightAllowed from the creative game mode, sprintAllowed from the
    // food level) itself: MovementInput carries neither, so the walk proves the
    // authority moved server-side.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();
        auto save = runtime.createWorld("movement", 7U, gameplay::GameMode::Creative);
        runtime.loadWorld(std::move(save), /*viewDistanceChunks=*/4);
        // The player's column must be loaded or the controller bails before it
        // moves (columnLoaded early-out). Pull the spawn chunk (24,?,24 -> 1,1).
        const auto batch = streamer.requestSync({1, 1}, std::chrono::seconds(10));
        assert(batch.has_value());
        applyBatch(runtime, *batch);

        // Fly above the terrain so the move is unobstructed. Teleport high, then
        // toggle creative flight with a jump double-tap (two jump edges within the
        // toggle window) — each edge is sent as its own MovementInput, exactly as
        // a client would sample the key.
        runtime.gameSession().teleportPlayer(gameplay::kPrimaryPlayerId,
                                             glm::vec3{24.5F, 120.0F, 24.5F});
        gameplay::MovementInput jump;
        jump.jumpPressed = true;
        runtime.sendClientMovement(jump);
        runtime.tick();
        runtime.sendClientMovement(jump);
        runtime.tick();
        assert(runtime.gameSession().player().flying());  // flight toggled on

        gameplay::MovementInput fly;
        fly.forward = 1.0F;
        fly.lookDirection = glm::vec3{1.0F, 0.0F, 0.0F};  // face +X
        fly.sprintHeld = true;
        runtime.sendClientMovement(fly);

        const auto before = runtime.gameSession().player().position();
        for (int tick = 0; tick < 20; ++tick) {
            runtime.tick();
        }
        const auto after = runtime.gameSession().player().position();
        const glm::vec3 horizontal{after.x - before.x, 0.0F, after.z - before.z};
        assert(glm::length(horizontal) > 0.1F);  // it flew along the intent
        assert(after.x > before.x);               // in the look direction (+X)

        // The intent was staged and the gated fields were set by the server, not
        // the client (which never sent them).
        const auto& applied = runtime.gameSession().input();
        assert(applied.forward == 1.0F);
        assert(applied.sprintHeld);
        assert(applied.flightAllowed);   // creative -> server-derived
        assert(applied.sprintAllowed);   // creative -> server-derived

        // Double-tap-forward sprint must survive several frame-sends landing
        // between two ticks. Regression (D0-b2 first cut): the forwardPressed edge
        // was written level-triggered into sharedInput, so a following no-press
        // send in the same interval overwrote it to false and the tap was lost —
        // only Ctrl (sprintHeld) could sprint. The fix OR-accumulates the edge
        // server-side (like the jump edge), so a false send never erases it. Still
        // flying at altitude here, so no terrain collision cancels the sprint.
        {
            // Re-center over the loaded spawn chunk: the sprint-fly above may have
            // carried the player past the one loaded column, where the controller
            // early-outs before the sprint logic would run.
            runtime.gameSession().teleportPlayer(gameplay::kPrimaryPlayerId,
                                                 glm::vec3{24.5F, 120.0F, 24.5F});
            gameplay::MovementInput tap;
            tap.forward = 1.0F;
            tap.lookDirection = glm::vec3{1.0F, 0.0F, 0.0F};
            // First tap, then a no-press send in the same between-tick interval.
            tap.forwardPressed = true;
            runtime.sendClientMovement(tap);
            tap.forwardPressed = false;
            runtime.sendClientMovement(tap);  // would erase the tap under the bug
            runtime.tick();
            assert(!runtime.gameSession().player().sprinting());  // one tap: not yet
            // Second tap within the double-tap window, same overwrite pattern.
            tap.forwardPressed = true;
            runtime.sendClientMovement(tap);
            tap.forwardPressed = false;
            runtime.sendClientMovement(tap);
            runtime.tick();
            assert(runtime.gameSession().player().sprinting());  // double-tap sprint
        }

        // D0 session commands over the channel: a game-mode switch and a respawn
        // travel as SessionCommands (not GameSession method calls the cross-process
        // client cannot make). SetGameMode applies on the next tick's drain.
        assert(runtime.gameSession().gameMode() == gameplay::GameMode::Creative);
        runtime.sendClientSessionCommand(gameplay::SetGameMode{gameplay::GameMode::Survival});
        runtime.tick();
        assert(runtime.gameSession().gameMode() == gameplay::GameMode::Survival);

        // Respawn is issued while the simulation is paused (the death screen), so
        // it is applied through applyClientCommandsNow() rather than a tick. It
        // returns the player to a fixed spawn regardless of where it was, and
        // republishes so the client mirror reflects the spawn this frame.
        runtime.gameSession().teleportPlayer(gameplay::kPrimaryPlayerId,
                                             glm::vec3{200.0F, 100.0F, 200.0F});
        runtime.sendClientSessionCommand(gameplay::Respawn{});
        runtime.applyClientCommandsNow();
        const auto spawnA = runtime.gameSession().player().position();
        assert(glm::length(spawnA - glm::vec3{200.0F, 100.0F, 200.0F}) > 1.0F);  // moved off

        // The republished spawn reached the channel: a mirror pumped now shows it.
        RecordingHost respawnMirrorHost;
        client::ClientMirror respawnMirror;
        assert(respawnMirror.pump(runtime.clientChannel(), respawnMirrorHost) > 0U);
        assert(respawnMirror.player() == runtime.gameSession().playerTickSnapshot());

        // A respawn from a different spot lands at the same fixed spawn.
        runtime.gameSession().teleportPlayer(gameplay::kPrimaryPlayerId,
                                             glm::vec3{-150.0F, 90.0F, -150.0F});
        runtime.sendClientSessionCommand(gameplay::Respawn{});
        runtime.applyClientCommandsNow();
        const auto spawnB = runtime.gameSession().player().position();
        assert(glm::length(spawnB - spawnA) < 0.01F);  // same spawn both times
    }

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

        // The saved position survives the reload in BOTH the live controller and
        // the published snapshot, before the first simulation tick runs. This is
        // the cold-start regression: the snapshot used to sit at (0,0,0) until a
        // tick published it, and the world-ready re-anchor teleported the player
        // back to the origin, overwriting the restored position.
        const glm::vec3 restoredPos = runtime.gameSession().player().position();
        assert(glm::length(restoredPos - savedPlayerPos) < 0.01F);
        const auto& restoredSnap = runtime.gameSession().playerTickSnapshot();
        assert(glm::length(restoredSnap.physicsCurrent - savedPlayerPos) < 0.01F);
        assert(glm::length(restoredSnap.physicsPrevious - savedPlayerPos) < 0.01F);

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

    // Switching worlds must not leak one world's snapshot into the next:
    // resetWorldState drops the old snapshots and loadWorld republishes the
    // fresh default, so a second world reads its own position, never the first
    // world's leftover.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();

        // World A restores its saved position.
        auto saveA = runtime.saveRepository().load(worldId);
        runtime.loadWorld(std::move(saveA), 4);
        assert(glm::length(runtime.gameSession().player().position() - savedPlayerPos) < 0.01F);
        assert(glm::length(runtime.gameSession().playerTickSnapshot().physicsCurrent -
                           savedPlayerPos) < 0.01F);

        // Tear world A down the way the renderer's world switch does.
        runtime.gameSession().resetWorldState();
        // The reset dropped the old snapshots; neither still carries world A's
        // state (this used to leave the position behind, making a hot reload
        // look "correct" by residual state).
        const auto& clearedSnap = runtime.gameSession().playerTickSnapshot();
        assert(clearedSnap.physicsCurrent == glm::vec3{0.0F});
        assert(runtime.gameSession().worldSnapshot().dayTimeTicks == 0.0);

        // World B opens fresh: its published snapshots are its own default spawn
        // and time, not world A's leftovers.
        auto saveB = runtime.createWorld("second", 99U, gameplay::GameMode::Creative);
        runtime.loadWorld(std::move(saveB), 4);
        const auto& secondSnap = runtime.gameSession().playerTickSnapshot();
        assert(secondSnap.physicsCurrent.y > 0.0F);  // not the cleared default
        assert(glm::length(secondSnap.physicsCurrent - savedPlayerPos) > 1.0F);
        assert(glm::length(runtime.gameSession().player().position() -
                           secondSnap.physicsCurrent) < 0.01F);
        // B's world snapshot is its own default spawn, not world A's saved
        // position nor the cleared (0,0,0).
        assert(glm::length(runtime.gameSession().worldSnapshot().worldSpawnPosition -
                           glm::vec3{24.0F, 76.38F, 24.0F}) < 0.01F);

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

    // M-3 C5: a chunk leaving the simulation radius persists its edits and
    // creatures to the chunk's region file and drops the creatures from the
    // simulation; a later stream of the chunk back restores them. This is the
    // chunk-owned entity lifecycle — a herd outside the radius lives on disk,
    // not in a chunk that no longer exists — and a save must not lose it.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();

        auto save = runtime.createWorld("unload-write", 0xCU, gameplay::GameMode::Creative);
        runtime.loadWorld(std::move(save), /*viewDistanceChunks=*/4);

        // An edit inside the chunk being unloaded, and one in a neighbour chunk,
        // so the unload path writes only its own chunk's record.
        world::WorldMutationService mutations;
        gameplay::GameplayMutationSink sink{runtime.world(), runtime.gameSession()};
        static_cast<void>(mutations.setBlock(
            runtime.world(), {24, 80, 24}, world::BlockState{world::Block::Stone},
            world::MutationFlags::All, world::MutationCause::PlayerPlace, sink));
        static_cast<void>(mutations.setBlock(
            runtime.world(), {40, 80, 40}, world::BlockState{world::Block::Dirt},
            world::MutationFlags::All, world::MutationCause::PlayerPlace, sink));
        // A creature inside chunk (1,1), with fields a fresh spawn would not
        // reproduce, and one in chunk (2,2) that must survive the unload.
        const auto* pigType = gameplay::entities::entityTypeRegistry().byId("pig");
        assert(pigType != nullptr);
        runtime.gameSession().worldEntities().restore({24.0F, 83.0F, 24.0F}, *pigType, 0.5F,
                                                      {0.1F, 0.0F, 0.0F}, 7.5F, 3, 42, 0xABABU);
        const auto* zombieType = gameplay::entities::entityTypeRegistry().byId("zombie");
        assert(zombieType != nullptr);
        runtime.gameSession().worldEntities().restore({40.0F, 64.0F, 40.0F}, *zombieType, 0.0F,
                                                      {0.0F, 0.0F, 0.0F}, 20.0F, 0, 0, 0U);
        {
            const auto drainWrite = runtime.lock().write();
            static_cast<void>(runtime.gameSession().drainEvents());
        }

        // Chunk (1,1) leaves the radius: its edit and pig persist, the pig leaves
        // the simulation, the neighbour chunk is untouched.
        runtime.persistUnloadedChunk({1, 1});
        // The unload's disk write is asynchronous now; force it to land before
        // reading the region file back directly.
        runtime.flushChunkPersistence();
        bool pigPresent = false;
        bool zombiePresent = false;
        for (const auto& entity : runtime.gameSession().worldEntities().entities()) {
            if (entity.type != nullptr && std::string{entity.type->id().path} == "pig") {
                pigPresent = true;
            }
            if (entity.type != nullptr && std::string{entity.type->id().path} == "zombie") {
                zombiePresent = true;
            }
        }
        assert(!pigPresent);
        assert(zombiePresent);

        // The region file for chunk (1,1) holds its pig with every saved field.
        const auto persisted =
            runtime.saveRepository().loadChunkEntities(runtime.currentSave().summary.identifier, 1, 1);
        assert(persisted.size() == 1U);
        assert(persisted[0].species == "pig");
        assert(persisted[0].x == 24.0F && persisted[0].y == 83.0F && persisted[0].z == 24.0F);
        assert(persisted[0].yaw == 0.5F);
        assert(persisted[0].health == 7.5F);
        assert(persisted[0].angerTicks == 3);
        assert(persisted[0].ageTicks == 42U);
        assert(persisted[0].rngState == 0xABABU);
        // The neighbour chunk has no record: its zombie stayed in the simulation.
        assert(runtime.saveRepository()
                   .loadChunkEntities(runtime.currentSave().summary.identifier, 2, 2)
                   .empty());

        // Chunk (1,1) streams back in: the pig returns.
        runtime.restoreLoadedChunk({1, 1});
        bool pigBack = false;
        for (const auto& entity : runtime.gameSession().worldEntities().entities()) {
            if (entity.type != nullptr && std::string{entity.type->id().path} == "pig" &&
                glm::length(entity.position - glm::vec3{24.0F, 83.0F, 24.0F}) < 0.01F) {
                pigBack = true;
            }
        }
        assert(pigBack);

        // Unload again and save: the re-persisted pig (already on disk) survives
        // the save-time region merge, and the reload carries both creatures.
        runtime.persistUnloadedChunk({1, 1});
        runtime.save();
        runtime.stopSimulation();
        {
            world::ChunkStreamer streamer2{0U, 4, 4};
            RecordingHost host2;
            runtime::GameRuntime runtime2{host2, streamer2, saveRoot};
            host2.save = &runtime2.currentSaveSlot();
            auto save2 = runtime.saveRepository().load(runtime.currentSave().summary.identifier);
            runtime2.loadWorld(std::move(save2), 4);
            bool pigReloaded = false;
            bool zombieReloaded = false;
            for (const auto& record : runtime2.currentSave().entities) {
                if (record.species == "pig") {
                    pigReloaded = true;
                }
                if (record.species == "zombie") {
                    zombieReloaded = true;
                }
            }
            assert(pigReloaded);
            assert(zombieReloaded);
            runtime2.stopSimulation();
        }
    }

    // CS-4: world-generation-time population. restoreLoadedChunk's chunk-
    // loaded hook is the same one M-3 C5 exercises above; this covers its new
    // branch — the first time a chunk streams in this session with no restore
    // pending, it runs NaturalSpawner::spawnForChunkGeneration once instead of
    // doing nothing (the pre-CS-4 behaviour: only unload-then-reload chunks
    // got anything, a brand new chunk got no generation-time herd at all).
    {
        world::ChunkStreamer streamer{17U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();

        auto save = runtime.createWorld("cs4-generation", 17U, gameplay::GameMode::Creative);
        runtime.loadWorld(std::move(save), /*viewDistanceChunks=*/4);

        // Every biome is forced to carry pig, the same way natural_spawn_test
        // sidesteps the real biome map for a deterministic assertion — CS-4's
        // contract is "a fresh chunk gets its generation-time herd", which
        // this exercises independent of which biome the terrain happens to be.
        for (int index = 0; index < static_cast<int>(world::gen::Biome::Count); ++index) {
            runtime.gameSession().naturalSpawner().spawnTables().set(
                static_cast<world::gen::Biome>(index), gameplay::entities::MobCategory::Creature,
                {{gameplay::entities::entityTypeRegistry().byId("pig"), 10, 4, 4}});
        }

        // Chunk (1,1) generates for the first time this session — no prior
        // unload, so unloadedChunks_ has nothing to restore for it.
        const auto batch = streamer.requestSync({1, 1}, std::chrono::seconds(10));
        assert(batch.has_value());
        applyBatch(runtime, *batch);
        assert(runtime.world().hasChunk({1, 1}));
        assert(runtime.gameSession().worldEntities().entities().empty());

        runtime.restoreLoadedChunk({1, 1});
        const auto firstCount = runtime.gameSession().worldEntities().entities().size();
        // Seed 17 at chunk (1,1) is a fixed point confirmed offline to clear
        // the pass's creature-spawn-probability draw *and* land its group on
        // real generated terrain (not open water, which the OnGround
        // placement rule correctly refuses), producing 4 pigs — so this is a
        // real, non-empty herd, and the "no duplicate on reload" assertions
        // below are a genuine proof, not a vacuously-true comparison of zeros.
        assert(firstCount == 4U);
        for (const auto& entity : runtime.gameSession().worldEntities().entities()) {
            assert(entity.type != nullptr);
            assert(std::string{entity.type->id().path} == "pig");
        }

        // Calling restoreLoadedChunk again for the same chunk without an
        // intervening unload (a spurious duplicate onChunkLoaded, or the
        // streamer redelivering the same generation batch) must not run the
        // pass a second time — populatedChunks_ dedups within the session.
        runtime.restoreLoadedChunk({1, 1});
        assert(runtime.gameSession().worldEntities().entities().size() == firstCount);

        // Unload the chunk (its herd, if any, and no edits, go to the region
        // file) and stream it back in: this is the M-3 C5 restore path, which
        // must take over instead of running spawnForChunkGeneration again —
        // restoring the exact surviving herd, not adding a fresh one on top.
        runtime.persistUnloadedChunk({1, 1});
        runtime.flushChunkPersistence();
        assert(runtime.gameSession().worldEntities().entities().empty());
        runtime.restoreLoadedChunk({1, 1});
        assert(runtime.gameSession().worldEntities().entities().size() == firstCount);
        runtime.restoreLoadedChunk({1, 1});  // still idempotent post-restore
        assert(runtime.gameSession().worldEntities().entities().size() == firstCount);

        // Cross-session: save (the surviving herd rides the flat entity list
        // the way M-3 C5's phase 1 already does), close, and reopen in a new
        // GameRuntime. The reload restores every entity immediately
        // (loadWorld's flat-list loop) — before any chunk streams — so the
        // first restoreLoadedChunk for (1,1) in the new process must detect
        // the chunk was already visited and must not add a second herd on
        // top of the one loadWorld just restored.
        runtime.save();
        const auto identifier = runtime.currentSave().summary.identifier;
        runtime.stopSimulation();
        {
            world::ChunkStreamer streamer2{17U, 4, 4};
            RecordingHost host2;
            runtime::GameRuntime runtime2{host2, streamer2, saveRoot};
            host2.save = &runtime2.currentSaveSlot();
            for (int index = 0; index < static_cast<int>(world::gen::Biome::Count); ++index) {
                runtime2.gameSession().naturalSpawner().spawnTables().set(
                    static_cast<world::gen::Biome>(index),
                    gameplay::entities::MobCategory::Creature,
                    {{gameplay::entities::entityTypeRegistry().byId("pig"), 10, 4, 4}});
            }
            auto reloaded = runtime2.saveRepository().load(identifier);
            runtime2.loadWorld(std::move(reloaded), 4);
            assert(runtime2.gameSession().worldEntities().entities().size() == firstCount);

            const auto batch2 = streamer2.requestSync({1, 1}, std::chrono::seconds(10));
            assert(batch2.has_value());
            applyBatch(runtime2, *batch2);
            runtime2.restoreLoadedChunk({1, 1});
            // No duplicate herd: still exactly what loadWorld restored.
            assert(runtime2.gameSession().worldEntities().entities().size() == firstCount);
            runtime2.stopSimulation();
        }
    }

    // CS-5: the cross-session dedup gap CS-4 left open (see the CS README's
    // CS-4 entry) — a chunk whose generation-time herd fully wanders off
    // before the world is ever saved leaves neither an edit nor a surviving
    // region entity record. Without the `populated` marker, a fresh process
    // reopening the save has no way to tell "visited, herd left" apart from
    // "never generated", and would run spawnForChunkGeneration a second time,
    // stacking a fresh herd onto empty space that was actually already
    // visited. This proves the marker closes that gap: after the herd leaves
    // and the chunk unloads-and-saves with zero edits/entities, a brand new
    // process's first restoreLoadedChunk for that chunk must NOT repopulate it.
    {
        world::ChunkStreamer streamer{17U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();

        auto save = runtime.createWorld("cs5-dedup-gap", 17U, gameplay::GameMode::Creative);
        for (int index = 0; index < static_cast<int>(world::gen::Biome::Count); ++index) {
            runtime.gameSession().naturalSpawner().spawnTables().set(
                static_cast<world::gen::Biome>(index), gameplay::entities::MobCategory::Creature,
                {{gameplay::entities::entityTypeRegistry().byId("pig"), 10, 4, 4}});
        }
        runtime.loadWorld(std::move(save), /*viewDistanceChunks=*/4);

        // Same fixed point as the CS-4 test above: seed 17, chunk (1,1) — a
        // real, non-empty generation-time herd (4 pigs), not a vacuous zero.
        const auto batch = streamer.requestSync({1, 1}, std::chrono::seconds(10));
        assert(batch.has_value());
        applyBatch(runtime, *batch);
        runtime.restoreLoadedChunk({1, 1});
        const auto firstCount = runtime.gameSession().worldEntities().entities().size();
        assert(firstCount == 4U);

        // The herd fully wanders off before anything is ever unloaded or
        // saved — removeInChunk is the exact extraction persistUnloadedChunk
        // itself uses, so this reproduces "no creature left in the chunk" the
        // same way natural movement out of the chunk's bounds would, without
        // fabricating a different code path. No edit was ever made here
        // either (no WorldMutationService call in this test), so the chunk
        // has neither kind of "there is content" evidence.
        static_cast<void>(runtime.gameSession().worldEntities().removeInChunk(1, 1));
        assert(runtime.gameSession().worldEntities().entities().empty());

        // Unload: persistUnloadedChunk finds zero edits and zero entities for
        // (1,1) — without CS-5 this would prune the record entirely (the
        // pre-CS-5 `saveChunk` rule). With CS-5's `populated=true` on every
        // unload, a bare marker record survives instead.
        runtime.persistUnloadedChunk({1, 1});
        runtime.flushChunkPersistence();
        assert(runtime.saveRepository().isChunkPopulated(
            runtime.currentSave().summary.identifier, 1, 1));
        assert(runtime.saveRepository()
                   .loadChunkEntities(runtime.currentSave().summary.identifier, 1, 1)
                   .empty());

        // A full save (not just the per-chunk unload write) must not drop the
        // marker either — writeRegionFiles' whole-world rewrite is a distinct
        // code path from saveChunk/saveChunks.
        runtime.save();
        const auto identifier = runtime.currentSave().summary.identifier;
        assert(runtime.saveRepository().isChunkPopulated(identifier, 1, 1));
        runtime.stopSimulation();

        // New process, same save: the first restoreLoadedChunk for (1,1) must
        // see the marker and skip spawnForChunkGeneration — without the CS-5
        // fix this is exactly the reproduction of the recorded gap (a second
        // herd of 4 pigs would appear on top of the empty chunk).
        {
            world::ChunkStreamer streamer2{17U, 4, 4};
            RecordingHost host2;
            runtime::GameRuntime runtime2{host2, streamer2, saveRoot};
            host2.save = &runtime2.currentSaveSlot();
            for (int index = 0; index < static_cast<int>(world::gen::Biome::Count); ++index) {
                runtime2.gameSession().naturalSpawner().spawnTables().set(
                    static_cast<world::gen::Biome>(index),
                    gameplay::entities::MobCategory::Creature,
                    {{gameplay::entities::entityTypeRegistry().byId("pig"), 10, 4, 4}});
            }
            auto reloaded = runtime2.saveRepository().load(identifier);
            runtime2.loadWorld(std::move(reloaded), 4);
            // loadWorld's flat-entity restore has nothing to bring back either
            // (the herd's departure was never captured in any flat list).
            assert(runtime2.gameSession().worldEntities().entities().empty());

            const auto batch2 = streamer2.requestSync({1, 1}, std::chrono::seconds(10));
            assert(batch2.has_value());
            applyBatch(runtime2, *batch2);
            runtime2.restoreLoadedChunk({1, 1});
            // The core CS-5 assertion: still zero, not a fresh 4-pig herd.
            assert(runtime2.gameSession().worldEntities().entities().empty());
            runtime2.stopSimulation();
        }
    }

    // D7 channel injection: the server drains client intents from, and publishes
    // snapshots to, an *attached* external channel instead of its internal
    // loopback — the seam the dedicated server plugs a TCP connection into. Prove
    // it with a second loopback pair standing in for the connection: a command
    // sent on the attached channel is applied, and the tick's snapshots come back
    // on it (not on the internal loopback), so a cross-process client would drive
    // and observe the world over exactly this path.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();
        auto save = runtime.createWorld("attach", 3U, gameplay::GameMode::Creative);
        runtime.loadWorld(std::move(save), 4);

        auto connection = net::makeLoopbackPair();  // stands in for a TCP connection
        runtime.attachConnection(*connection.server);

        // A client intent sent on the attached channel reaches the session.
        assert(runtime.gameSession().inventory().selectedHotbarSlot() != 6U);
        net::sendMessage(*connection.client,
                         net::NetMessage{gameplay::GameCommand{gameplay::SwapSlot{6U}}});
        runtime.tick();
        assert(runtime.gameSession().inventory().selectedHotbarSlot() == 6U);

        // The tick's snapshots came back on the attached channel: a mirror pumped
        // from its client end matches what the session published.
        RecordingHost mirrorHost;
        client::ClientMirror mirror;
        assert(mirror.pump(*connection.client, mirrorHost) > 0U);
        assert(mirror.player() == runtime.gameSession().playerTickSnapshot());

        // Nothing leaked onto the internal loopback while a connection is attached.
        std::vector<std::uint8_t> stray;
        assert(!runtime.clientChannel().receiveFrame(stray));
    }

    // CMD3: `/kill <selector>` runs a real target selector over the live pools.
    // Self-contained (its own world) so removing creatures disturbs nothing above.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();
        auto save = runtime.createWorld("selectors", 5U, gameplay::GameMode::Creative);
        runtime.loadWorld(std::move(save), 4);

        const auto* pigType = gameplay::entities::entityTypeRegistry().byId("pig");
        const auto* zombieType = gameplay::entities::entityTypeRegistry().byId("zombie");
        assert(pigType != nullptr && zombieType != nullptr);
        const auto liveOf = [&](std::string_view species) {
            std::size_t count = 0U;
            for (const auto& e : runtime.gameSession().worldEntities().entities()) {
                if (!e.dead() && e.type != nullptr && std::string{e.type->id().path} == species) {
                    ++count;
                }
            }
            return count;
        };
        runtime.gameSession().worldEntities().restore({10.0F, 64.0F, 0.0F}, *pigType, 0.0F,
                                                      {0.0F, 0.0F, 0.0F}, 10.0F, 0, 0, 1U);
        runtime.gameSession().worldEntities().restore({12.0F, 64.0F, 0.0F}, *pigType, 0.0F,
                                                      {0.0F, 0.0F, 0.0F}, 10.0F, 0, 0, 2U);
        runtime.gameSession().worldEntities().restore({14.0F, 64.0F, 0.0F}, *zombieType, 0.0F,
                                                      {0.0F, 0.0F, 0.0F}, 20.0F, 0, 0, 3U);
        assert(liveOf("pig") == 2U && liveOf("zombie") == 1U);

        // type= restricts the kill to the species — the zombie dies, pigs survive.
        runtime.enqueueChat("/kill @e[type=zombie]");
        runtime.tick();
        const auto killedZombie = runtime.takeChatResult();
        assert(killedZombie.has_value() && killedZombie->success);
        assert(liveOf("zombie") == 0U && liveOf("pig") == 2U);

        // limit caps the match count: one pig falls, one remains.
        runtime.enqueueChat("/kill @e[type=pig,limit=1]");
        runtime.tick();
        assert(runtime.takeChatResult().value().success);
        assert(liveOf("pig") == 1U);

        // A selector that matches nothing fails (and reports so).
        runtime.enqueueChat("/kill @e[type=zombie]");
        runtime.tick();
        const auto noZombie = runtime.takeChatResult();
        assert(noZombie.has_value() && !noZombie->success);

        // No target defaults to @s (the executor) — kills the player.
        runtime.enqueueChat("/kill");
        runtime.tick();
        assert(runtime.takeChatResult().value().success);

        runtime.stopSimulation();
    }

    // CMD4: content commands wired to existing systems (setblock/fill/summon/
    // difficulty/seed/clear). Self-contained world.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();
        auto save = runtime.createWorld("content", 12345U, gameplay::GameMode::Creative);
        runtime.loadWorld(std::move(save), 4);
        // Pump the spawn chunk into the world so setblock/fill have loaded cells to
        // write (every edit below stays inside chunk (1,1): x,z in [16,31]).
        const auto batch = streamer.requestSync({1, 1}, std::chrono::seconds(10));
        assert(batch.has_value());
        applyBatch(runtime, *batch);

        const auto runCmd = [&](const std::string& line) {
            runtime.enqueueChat(line);
            runtime.tick();
            return runtime.takeChatResult();
        };
        const auto blockAt = [&](int x, int y, int z) {
            return runtime.world().block(x, y, z);
        };

        // setblock writes one cell (default mode replace).
        {
            const auto result = runCmd("/setblock 24 90 24 stone");
            assert(result.has_value() && result->success);
            assert(blockAt(24, 90, 24) == world::Block::Stone);
        }
        // keep only writes into air, so the stone stays and the command reports failure.
        {
            const auto result = runCmd("/setblock 24 90 24 dirt keep");
            assert(result.has_value() && !result->success);
            assert(blockAt(24, 90, 24) == world::Block::Stone);
        }
        // A chest routes through the mutation path, so its block entity is created —
        // proof the write is not a raw block-array poke.
        {
            const auto result = runCmd("/setblock 24 91 24 chest");
            assert(result.has_value() && result->success);
            assert(blockAt(24, 91, 24) == world::Block::Chest);
            assert(runtime.gameSession().chestSystem().find({24, 91, 24}) != nullptr);
        }
        // fill replace covers the whole 3x3 area.
        {
            const auto result = runCmd("/fill 26 90 26 28 90 28 stone");
            assert(result.has_value() && result->success);
            std::size_t stone = 0U;
            for (int x = 26; x <= 28; ++x) {
                for (int z = 26; z <= 28; ++z) {
                    if (blockAt(x, 90, z) == world::Block::Stone) ++stone;
                }
            }
            assert(stone == 9U);
        }
        // fill outline writes only the shell of a 3x3x3 box; the interior is untouched.
        {
            const auto result = runCmd("/fill 20 92 20 22 94 22 stone outline");
            assert(result.has_value() && result->success);
            assert(blockAt(20, 92, 20) == world::Block::Stone);   // a corner
            assert(blockAt(21, 93, 21) != world::Block::Stone);   // the interior
        }
        // fill enforces the volume cap rather than stalling the tick.
        {
            const auto result = runCmd("/fill 0 0 0 100 100 100 stone");
            assert(result.has_value() && !result->success);
        }
        // summon spawns the species at the given position.
        {
            const auto before = runtime.gameSession().worldEntities().entities().size();
            const auto result = runCmd("/summon pig 18 92 18");
            assert(result.has_value() && result->success);
            assert(runtime.gameSession().worldEntities().entities().size() > before);
            bool found = false;
            for (const auto& e : runtime.gameSession().worldEntities().entities()) {
                if (!e.dead() && e.type != nullptr && std::string{e.type->id().path} == "pig" &&
                    glm::length(e.position - glm::vec3{18.0F, 92.0F, 18.0F}) < 1.5F) {
                    found = true;
                }
            }
            assert(found);
        }
        // difficulty set + query.
        {
            const auto set = runCmd("/difficulty hard");
            assert(set.has_value() && set->success);
            assert(runtime.gameSession().difficulty() == gameplay::Difficulty::Hard);
            const auto query = runCmd("/difficulty");
            assert(query.has_value() && query->success &&
                   query->message.find("hard") != std::string::npos);
        }
        // seed reports the world seed.
        {
            const auto result = runCmd("/seed");
            assert(result.has_value() && result->success &&
                   result->message.find("12345") != std::string::npos);
        }
        // clear empties the inventory.
        {
            static_cast<void>(runCmd("/give stone 10"));
            std::size_t before = 0U;
            for (const auto& s : runtime.gameSession().inventory().slots()) before += s.count;
            assert(before >= 10U);
            const auto result = runCmd("/clear");
            assert(result.has_value() && result->success);
            std::size_t after = 0U;
            for (const auto& s : runtime.gameSession().inventory().slots()) after += s.count;
            assert(after == 0U);
        }
        runtime.stopSimulation();
    }

    // XP-3: /experience (/xp) add|set|query <targets> <amount> [points|levels] —
    // thin command-tree wiring onto XP-0's PlayerExperience add/set API, plus a
    // genuine `/xp` alias (same subtree via redirectTo, not a second copy).
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();
        auto save = runtime.createWorld("experience", 99U, gameplay::GameMode::Survival);
        runtime.loadWorld(std::move(save), 4);

        const auto runCmd = [&](const std::string& line) {
            runtime.enqueueChat(line);
            runtime.tick();
            return runtime.takeChatResult();
        };
        auto& experience = runtime.gameSession().experience();

        // add ... levels: whole levels, not points (sabotage target ①: swapping
        // the points/levels branch would make this land on points instead).
        {
            const auto result = runCmd("/xp add @s 10 levels");
            assert(result.has_value() && result->success);
            assert(experience.level() == 10);
            assert(experience.totalExperience() == 0); // giveExperienceLevels leaves points/total at 0
        }
        // add ... points: raw points, walking levels up the way addExperience does.
        {
            const auto before = experience.totalExperience();
            const auto result = runCmd("/xp add @s 100 points");
            assert(result.has_value() && result->success);
            assert(experience.totalExperience() == before + 100);
        }
        // The bare (unit-omitted) add form defaults to points, matching 26.1's
        // two-argument ExperienceCommand overload.
        {
            const auto before = experience.totalExperience();
            const auto result = runCmd("/xp add @s 5");
            assert(result.has_value() && result->success);
            assert(experience.totalExperience() == before + 5);
        }
        // set ... levels pins the level and zeroes in-level progress/total.
        {
            const auto result = runCmd("/xp set @s 3 levels");
            assert(result.has_value() && result->success);
            assert(experience.level() == 3);
        }
        // set ... points re-derives level/progress from a raw lifetime total.
        {
            const auto result = runCmd("/xp set @s 400 points");
            assert(result.has_value() && result->success);
            assert(experience.totalExperience() == 400);
            assert(experience.level() > 0); // 400 points is well past level 1
        }
        // query levels / points report the live values (via /experience, the
        // canonical name — /xp is checked for equivalence separately below).
        {
            const auto levels = runCmd("/experience query @s levels");
            assert(levels.has_value() && levels->success);
            assert(levels->message.find(std::to_string(experience.level())) != std::string::npos);
            const auto points = runCmd("/experience query @s points");
            assert(points.has_value() && points->success);
            assert(points->message.find(std::to_string(experience.totalExperience())) !=
                   std::string::npos);
        }
        // Negative add levels decrements and does not go below zero (giveExperienceLevels
        // saturates at 0 rather than going negative) — sabotage target ③ variant.
        {
            static_cast<void>(runCmd("/xp set @s 1 levels"));
            const auto result = runCmd("/xp add @s -5 levels");
            assert(result.has_value() && result->success);
            assert(experience.level() == 0);
        }
        // set ... levels with a negative value clamps to 0, not a crash or an
        // underflowed level (sabotage target ③: the primary case the card names).
        {
            static_cast<void>(runCmd("/xp set @s 8 levels"));
            const auto result = runCmd("/xp set @s -3 levels");
            assert(result.has_value() && result->success);
            assert(experience.level() == 0);
        }
        // /xp and /experience are a genuine alias: same completion, same effect.
        {
            static_cast<void>(runCmd("/xp set @s 0 levels"));
            const auto viaAlias = runCmd("/xp add @s 7 levels");
            assert(viaAlias.has_value() && viaAlias->success);
            assert(experience.level() == 7);
            static_cast<void>(runCmd("/xp set @s 0 levels"));
            const auto viaFull = runCmd("/experience add @s 7 levels");
            assert(viaFull.has_value() && viaFull->success);
            assert(experience.level() == 7); // identical outcome through either name
            // The alias shares the real subtree (redirectTo), so completion after
            // `/xp add @s 7 ` offers the same points/levels literals `/experience`
            // does — proof it is not a second, driftable copy of the tree.
            const auto afterXp = runtime.commandDispatcher().suggestions("/xp add @s 7 ", 13);
            const auto hasUnit = [](const auto& list, std::string_view text) {
                return std::ranges::any_of(
                    list, [&](const auto& suggestion) { return suggestion.text == text; });
            };
            assert(hasUnit(afterXp, "points") && hasUnit(afterXp, "levels"));
        }
        // A selector matching no players affects nothing and fails cleanly.
        {
            const auto result = runCmd("/xp add @e[type=pig] 5 levels");
            assert(result.has_value() && !result->success);
        }
        runtime.stopSimulation();
    }

    // CMD5: execute rebinds the command source (as/at/positioned) and gates it
    // (if/unless), then `run` re-enters the dispatcher on each transformed source.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();
        auto save = runtime.createWorld("execute", 7U, gameplay::GameMode::Creative);
        runtime.loadWorld(std::move(save), 4);
        const auto batch = streamer.requestSync({1, 1}, std::chrono::seconds(10));
        assert(batch.has_value());
        applyBatch(runtime, *batch);

        const auto runCmd = [&](const std::string& line) {
            runtime.enqueueChat(line);
            runtime.tick();
            return runtime.takeChatResult();
        };
        const auto liveOf = [&](std::string_view species) {
            std::size_t count = 0U;
            for (const auto& e : runtime.gameSession().worldEntities().entities()) {
                if (!e.dead() && e.type != nullptr && std::string{e.type->id().path} == species) {
                    ++count;
                }
            }
            return count;
        };

        const auto* pigType = gameplay::entities::entityTypeRegistry().byId("pig");
        const auto* zombieType = gameplay::entities::entityTypeRegistry().byId("zombie");
        assert(pigType != nullptr && zombieType != nullptr);
        runtime.gameSession().worldEntities().restore({20.0F, 90.0F, 20.0F}, *pigType, 0.0F,
                                                      {0.0F, 0.0F, 0.0F}, 10.0F, 0, 0, 1U);
        runtime.gameSession().worldEntities().restore({21.0F, 90.0F, 21.0F}, *pigType, 0.0F,
                                                      {0.0F, 0.0F, 0.0F}, 10.0F, 0, 0, 2U);
        runtime.gameSession().worldEntities().restore({22.0F, 90.0F, 22.0F}, *pigType, 0.0F,
                                                      {0.0F, 0.0F, 0.0F}, 10.0F, 0, 0, 3U);
        runtime.gameSession().worldEntities().restore({24.0F, 90.0F, 24.0F}, *zombieType, 0.0F,
                                                      {0.0F, 0.0F, 0.0F}, 20.0F, 0, 0, 4U);
        assert(liveOf("pig") == 3U && liveOf("zombie") == 1U);

        // as: each pig becomes @s in turn, so `kill @s` kills every pig (not the
        // player, and not one pig three times) — the zombie is untouched.
        {
            const auto result = runCmd("/execute as @e[type=pig] run kill @s");
            assert(result.has_value() && result->success);
            assert(liveOf("pig") == 0U && liveOf("zombie") == 1U);
        }
        // positioned penetrates to run's `~`: the block lands at the given
        // position, not at the executor's feet.
        {
            const auto result = runCmd("/execute positioned 30 90 30 run setblock ~ ~ ~ stone");
            assert(result.has_value() && result->success);
            assert(runtime.world().block(30, 90, 30) == world::Block::Stone);
        }
        // at moves the source onto the target, so `~` follows it there: `at @p`
        // then `setblock ~ ~ ~` lands at the player's own cell. (Read the feet
        // after the command: setblock ran at the end of that tick, so the same
        // post-physics position it used is what we floor here.)
        {
            const auto result = runCmd("/execute at @p run setblock ~ ~ ~ stone");
            assert(result.has_value() && result->success);
            const glm::vec3 feet = runtime.gameSession().player().position();
            const int px = static_cast<int>(std::floor(feet.x));
            const int py = static_cast<int>(std::floor(feet.y));
            const int pz = static_cast<int>(std::floor(feet.z));
            assert(runtime.world().block(px, py, pz) == world::Block::Stone);
        }
        // if entity gates on a match: the zombie exists, so the block is placed.
        {
            const auto result = runCmd("/execute if entity @e[type=zombie] run setblock 28 90 28 stone");
            assert(result.has_value() && result->success);
            assert(runtime.world().block(28, 90, 28) == world::Block::Stone);
        }
        // unless entity: no pig is left, so `unless entity @e[type=pig]` runs.
        {
            const auto result = runCmd("/execute unless entity @e[type=pig] run setblock 29 90 29 stone");
            assert(result.has_value() && result->success);
            assert(runtime.world().block(29, 90, 29) == world::Block::Stone);
        }
        // A failed gate runs nothing: no pig means `if entity @e[type=pig]` skips.
        {
            const auto result = runCmd("/execute if entity @e[type=pig] run setblock 31 90 31 stone");
            assert(result.has_value() && !result->success);
            assert(runtime.world().block(31, 90, 31) != world::Block::Stone);
        }
        runtime.stopSimulation();
    }

    // CMD6: command introspection + /help, and the R1 refactor (usage generated
    // from the node tree, not hand-written). usage() needs no world.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();
        auto save = runtime.createWorld("help", 1U, gameplay::GameMode::Creative);
        runtime.loadWorld(std::move(save), 4);
        auto& dispatcher = runtime.commandDispatcher();

        gameplay::command::CommandSource owner; // op4
        gameplay::command::CommandSource op1;
        op1.permissionLevel = gameplay::command::PermissionLevel::Moderators;

        // R1 regression: the generated usage of every command is locked to its
        // canonical shape, so a signature change that would silently reword it is
        // caught here (the old hand-written strings had drifted from this shape).
        assert(dispatcher.usage("gamemode", owner) == "gamemode <mode>");
        assert(dispatcher.usage("gamerule", owner) == "gamerule <rule> [<value>]");
        assert(dispatcher.usage("weather", owner) == "weather (clear|rain|thunder) [<duration>]");
        // Literal alternatives are sorted (determinism), so the modes read
        // alphabetically rather than in registration order.
        assert(dispatcher.usage("setblock", owner) ==
               "setblock <x> <y> <z> <block> [destroy|keep|replace]");
        assert(dispatcher.usage("fill", owner) ==
               "fill <x> <y> <z> <x> <y> <z> <block> [destroy|hollow|keep|outline|replace]");
        assert(dispatcher.usage("summon", owner) == "summon <entity> [<x> <y> <z>]");
        assert(dispatcher.usage("spawnpoint", owner) == "spawnpoint [<x> <y> <z>]");
        assert(dispatcher.usage("kill", owner) == "kill [<targets>]");
        assert(dispatcher.usage("difficulty", owner) == "difficulty [<level>]");
        assert(dispatcher.usage("time", owner) == "time set <time>");
        assert(dispatcher.usage("give", owner) == "give <item> <count>");
        // CMD-7: execute is a real redirect subtree now, so its usage is
        // generated from the clause nodes (sorted, grouped) instead of the old
        // hand-written `execute <chain>`. Locks the whole clause tree's shape.
        assert(dispatcher.usage("execute", owner) ==
               "execute (as <targets>|at <targets>|facing (entity <targets>|<x> <y> <z>)|"
               "if (block <x> <y> <z> <block>|entity <targets>)|in <dimension>|"
               "positioned (as <targets>|<x> <y> <z>)|rotated (as <targets>|<rot>)|run|"
               "unless (block <x> <y> <z> <block>|entity <targets>))");
        assert(dispatcher.usage("seed", owner) == "seed");
        assert(dispatcher.usage("clear", owner) == "clear");

        // op filtering: a level-2 command is hidden from op1; /help (level 0) is not.
        assert(dispatcher.usage("gamemode", op1).empty());
        assert(dispatcher.usage("help", op1) == "help [<command>]");

        // CMD-7: the real /execute tree completes across its redirects — the
        // clause list after `execute`, and every command after `run` (the win
        // the greedy-string form could not give). Uses the live runtime tree.
        const auto textOf = [](const std::vector<gameplay::command::Suggestion>& list,
                               std::string_view text) {
            return std::ranges::any_of(
                list, [&](const auto& suggestion) { return suggestion.text == text; });
        };
        {
            const auto clauses = dispatcher.suggestions("/execute ", 9);
            assert(textOf(clauses, "as") && textOf(clauses, "if") && textOf(clauses, "run"));
            const auto afterRun = dispatcher.suggestions("/execute run ", 13);
            assert(textOf(afterRun, "setblock") && textOf(afterRun, "gamemode"));
            const auto partial = dispatcher.suggestions("/execute positioned ~ ~ ~ ru", 28);
            assert(textOf(partial, "run"));
        }

        // CMD-7 item 3: the gamerule value completes by the selected rule's type —
        // a boolean rule offers true/false, an int rule its range instead. The
        // value type reads the `rule` bound earlier on the line.
        {
            const std::string boolLine = "/gamerule keepInventory ";
            const auto boolValue = dispatcher.suggestions(boolLine, boolLine.size());
            assert(textOf(boolValue, "true") && textOf(boolValue, "false"));
            const std::string intLine = "/gamerule randomTickSpeed ";
            const auto intValue = dispatcher.suggestions(intLine, intLine.size());
            assert(textOf(intValue, "0"));
            assert(!textOf(intValue, "true")); // an int rule is not boolean-completed
        }

        // R2: an incomplete command reports its usage (via the same generator).
        {
            runtime.enqueueChat("/gamerule");
            runtime.tick();
            const auto result = runtime.takeChatResult();
            assert(result.has_value() && !result->success);
            assert(result->message == "Usage: /gamerule <rule> [<value>]");
        }
        // /help lists commands with usage; /help <command> shows one.
        {
            runtime.enqueueChat("/help");
            runtime.tick();
            const auto all = runtime.takeChatResult();
            assert(all.has_value() && all->success);
            assert(all->message.find("/gamemode <mode>") != std::string::npos);
            assert(all->message.find("/setblock <x> <y> <z> <block>") != std::string::npos);

            runtime.enqueueChat("/help gamerule");
            runtime.tick();
            const auto one = runtime.takeChatResult();
            assert(one.has_value() && one->success);
            assert(one->message == "/gamerule <rule> [<value>]");
        }
        runtime.stopSimulation();
    }

    // CMD-8: allowCommands (Allow Cheats) drives the host's op level. Cheats off →
    // gameplay commands (GameMasters) are refused by the existing permission layer,
    // while client-side level-0 commands (/help) still run; cheats on → all pass.
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();
        const auto runCmd = [&](const std::string& line) {
            runtime.enqueueChat(line);
            runtime.tick();
            return runtime.takeChatResult();
        };

        auto noCheats =
            runtime.createWorld("no-cheats", 1U, gameplay::GameMode::Creative, /*allowCommands=*/false);
        runtime.loadWorld(std::move(noCheats), 4);
        const auto denied = runCmd("/gamemode survival");
        assert(denied.has_value() && !denied->success);
        assert(denied->message.find("permission") != std::string::npos);
        const auto help = runCmd("/help"); // level All — unaffected by cheats
        assert(help.has_value() && help->success);
        runtime.stopSimulation();
    }
    {
        world::ChunkStreamer streamer{0U, 4, 4};
        RecordingHost host;
        runtime::GameRuntime runtime{host, streamer, saveRoot};
        host.save = &runtime.currentSaveSlot();
        auto cheats =
            runtime.createWorld("cheats", 2U, gameplay::GameMode::Creative, /*allowCommands=*/true);
        runtime.loadWorld(std::move(cheats), 4);
        runtime.enqueueChat("/gamemode survival");
        runtime.tick();
        const auto allowed = runtime.takeChatResult();
        assert(allowed.has_value() && allowed->success);
        runtime.stopSimulation();
    }

    std::filesystem::remove_all(saveRoot);
    std::cout << "PASS: game_runtime_test\n";
    return 0;
}
