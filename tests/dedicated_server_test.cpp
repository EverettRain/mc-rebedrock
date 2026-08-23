#include "server/DedicatedServer.hpp"

#include "client/ClientMirror.hpp"
#include "gameplay/BlockTags.hpp"
#include "gameplay/GameCommand.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/SessionCommand.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "net/Handshake.hpp"
#include "net/NetMessage.hpp"
#include "net/TcpTransport.hpp"
#include "net/Transport.hpp"
#include "world/gen/Biome.hpp"

#include <glm/vec3.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

// Stage C, §8.2 / D7: the dedicated server runs the full authoritative gameplay
// chain headless — world generation, chunk streaming, simulation, persistence —
// with only the runtime linked (this test's build lists no render/vulkan/glfw
// source, so a link would fail if the runtime reached into them). It is
// game_runtime_test promoted into the server object: create a world, load its
// spawn column, tick it, and prove it advanced and persisted across a reload.
//
// A small view distance keeps the streamer worker's background generation cheap
// in the unoptimised test build; the point here is that the server ticks and
// persists, not how large an area it streams.

using namespace mc;

namespace {

// A do-nothing host for the client's ClientMirror to apply events into — the
// client thread has no world of its own, it only needs the snapshots.
struct NullHost final : gameplay::SimulationHost {
    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
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

}  // namespace

int main() {
    const auto saveRoot = std::filesystem::temp_directory_path() / "dedicated-server-test";
    std::filesystem::remove_all(saveRoot);
    std::filesystem::create_directories(saveRoot);

    std::string worldId;
    std::uint64_t tickAfterRun = 0U;

    // Create, load the spawn column, and run the simulation headless.
    {
        server::DedicatedServer server{saveRoot, /*viewDistanceChunks=*/1};
        server.createAndLoadWorld("dedicated-test", 0x1234'5678'9ABC'DEF0ULL,
                                  gameplay::GameMode::Survival);
        worldId = server.worldIdentifier();
        assert(!worldId.empty());

        // The player's own column is force-loaded before the first tick.
        server.ensureSpawnAreaLoaded(/*radius=*/0);
        assert(server.loadedChunkCount() >= 1U);

        // The world advances: run ticks and the persisted server tick reflects it.
        server.runForTicks(30);
        server.save();
        tickAfterRun = server.runtime().currentSave().serverTick;
        assert(tickAfterRun >= 30U);

        // Resident memory is real but bounded — catches a gross leak without being
        // terrain-sensitive.
        assert(server.serverResidentBytes() > 0U);
        assert(server.serverResidentBytes() < 64U * 1024U * 1024U);
    }

    // A fresh server reopens the same save by its identifier and the world's
    // progress survived the round trip, then keeps ticking from there.
    {
        server::DedicatedServer server{saveRoot, /*viewDistanceChunks=*/1};
        assert(server.loadWorld(worldId));
        assert(server.runtime().currentSave().serverTick == tickAfterRun);

        server.ensureSpawnAreaLoaded(/*radius=*/0);
        server.runForTicks(20);
        server.save();
        assert(server.runtime().currentSave().serverTick >= tickAfterRun + 20U);
    }

    // Loading a name that does not exist fails rather than crashing or throwing.
    {
        server::DedicatedServer server{saveRoot, /*viewDistanceChunks=*/1};
        assert(!server.loadWorldByName("no-such-world"));
    }

    // CS-5: the headless dedicated server now drives the M-3 C5 / CS-4
    // chunk-loaded hook too (applyBatch calls restoreLoadedChunk/
    // persistUnloadedChunk the same way VulkanRenderer.cpp's
    // onChunkLoaded/onChunkUnloaded callbacks do) — this was a recorded gap
    // (CS README's CS-4 entry: "DedicatedServer... currently does not wire
    // restoreLoadedChunk"). Proves world-generation-time creature population
    // actually fires headless: every biome forced to carry pig (same
    // determinism fixture natural_spawn_test/game_runtime_test use), spawn
    // point's own chunk force-loaded, and the simulation must come up with a
    // real, non-empty herd rather than the pre-fix silence.
    {
        server::DedicatedServer server{saveRoot, /*viewDistanceChunks=*/1};
        server.createAndLoadWorld("cs5-dedicated-spawn", 17U, gameplay::GameMode::Survival);
        // Registered once already by gameplay::entities::registerBuiltinEntities()
        // inside DedicatedServer's constructor-time BuiltinEntityRegistration;
        // safe to look the type up directly. Must run *after*
        // createAndLoadWorld: loadWorld's NaturalSpawner::setSeed replaces
        // tables_ with a fresh copy of the process-wide default tables (see
        // NaturalSpawner.cpp), so setting the override first would just be
        // overwritten — the same ordering game_runtime_test's CS-4 case uses.
        for (int index = 0; index < static_cast<int>(world::gen::Biome::Count); ++index) {
            server.runtime().gameSession().naturalSpawner().spawnTables().set(
                static_cast<world::gen::Biome>(index), gameplay::entities::MobCategory::Creature,
                {{gameplay::entities::entityTypeRegistry().byId("pig"), 10, 4, 4}});
        }
        // Chunk (1,1) is the deterministic fixed point game_runtime_test's CS-4
        // case uses (seed 17 clears the probability draw and lands on real
        // terrain, not open water) — force it in directly via requestSync +
        // the same applyBatch the server's own force-load path uses, instead
        // of depending on where the default spawn column happens to fall.
        auto& runtime = server.runtime();
        const auto batch =
            runtime.chunkStreamer().requestSync({1, 1}, std::chrono::seconds(10));
        assert(batch.has_value());
        assert(batch->worldEpoch == runtime.worldEpoch());
        for (const auto& update : batch->chunkUpdates) {
            if (!update.remove) {
                runtime.world().setChunk(update.position, update.chunk);
                runtime.restoreLoadedChunk(update.position);
            }
        }
        assert(runtime.world().hasChunk({1, 1}));
        assert(runtime.gameSession().worldEntities().entities().size() == 4U);
        for (const auto& entity : runtime.gameSession().worldEntities().entities()) {
            assert(entity.type != nullptr);
            assert(std::string{entity.type->id().path} == "pig");
        }
    }

    // PACK-1: the dedicated server (no render, no resource stack — this test
    // build lists no render/vulkan source, so a link would fail if the
    // runtime reached into them) loads a save's <save>/datapacks/, and
    // `/datapack enable` triggers the rebuild — the authoritative-on-both-
    // ends acceptance point proven with zero render dependency. Also proves
    // per-save isolation and the "no residue after unload" property across
    // two different saves opened in the same process.
    {
        const auto resourceDir = saveRoot / "pack1-resources";
        std::filesystem::create_directories(resourceDir);  // an empty built-in base is enough

        server::DedicatedServer server{saveRoot, /*viewDistanceChunks=*/1, resourceDir};
        server.createAndLoadWorld("pack1-a", 1U, gameplay::GameMode::Survival);
        const std::string worldA = server.worldIdentifier();

        // Write a datapack directly into save A's own datapacks/ folder,
        // after the world exists (mirrors a player dropping a folder in
        // while the world is closed, then reopening it).
        const auto packRoot = saveRoot / worldA / "datapacks" / "pickaxe-override";
        std::filesystem::create_directories(packRoot);
        {
            std::ofstream mcmeta{packRoot / "pack.mcmeta", std::ios::binary};
            mcmeta << R"({"pack": {"pack_format": 84, "description": "server test"}})";
        }
        const auto tagPath =
            packRoot / "data" / "minecraft" / "tags" / "block" / "mineable" / "pickaxe.json";
        std::filesystem::create_directories(tagPath.parent_path());
        {
            std::ofstream tag{tagPath, std::ios::binary};
            tag << R"({"replace": true, "values": []})";
        }

        // Reopen save A: loadWorld's scan discovers the new folder. Not yet
        // enabled, so the built-in floor still holds.
        const bool reopened = server.loadWorld(worldA);
        assert(reopened);
        assert(!gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));
        auto& runtimeA = server.runtime();
        {
            const auto listing = runtimeA.dataPackStack().list();
            assert(listing.size() == 1U);
            assert(listing[0].id == "pickaxe-override");
            assert(!listing[0].enabled);
        }

        // `/datapack enable` mutates the stack and rebuilds — the tag becomes
        // data-driven without reloading the world.
        runtimeA.enqueueChat("/datapack enable pickaxe-override");
        server.tickOnce();
        const auto enableResult = runtimeA.takeChatResult();
        assert(enableResult.has_value());
        assert(enableResult->success);
        assert(gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));

        // Persists: save, reopen the same world, the pack is still enabled
        // with no /datapack call needed.
        server.save();
        assert(server.loadWorld(worldA));
        assert(gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));
        assert(server.runtime().dataPackStack().list().at(0).enabled);

        // A second, unrelated save with no datapack folder: opening it must
        // show the built-in floor, not save A's override — per-save
        // isolation, and no residue left by unloadWorld's reset.
        server.createAndLoadWorld("pack1-b", 2U, gameplay::GameMode::Survival);
        assert(!gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));
        assert(server.runtime().dataPackStack().list().empty());

        // Back to save A: the tables follow A's stack again (still enabled,
        // persisted above), proving a world switch is not one-directional.
        assert(server.loadWorld(worldA));
        assert(gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));

        // Explicit unloadWorld(), with no new loadWorld yet: the tables must
        // already be back at the built-in floor — a caller reading between
        // unload and the next load must never see the outgoing save's
        // residue. This is the direct proof of the "world unload leaves
        // tables un-reset" sabotage target, distinct from loadWorld's own
        // reset-then-rescan (which the save-B switch above already exercises).
        runtimeA.unloadWorld();
        assert(!gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));
        assert(runtimeA.dataPackStack().list().empty());

        // Re-open A: the stack and tables come back from disk, unaffected by
        // the unload/reload round trip.
        assert(server.loadWorld(worldA));
        assert(gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));

        // `/datapack disable` + rebuild falls back to the floor without a
        // reload, and `/datapack list` reports it.
        runtimeA.enqueueChat("/datapack disable pickaxe-override");
        server.tickOnce();
        assert(runtimeA.takeChatResult().has_value());
        assert(!gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));

        runtimeA.enqueueChat("/datapack list");
        server.tickOnce();
        const auto listResult = runtimeA.takeChatResult();
        assert(listResult.has_value());
        assert(listResult->success);
        assert(listResult->message.find("disabled") != std::string::npos);
    }

    // D7 cross-process play over a real socket: a client connects on 127.0.0.1,
    // completes the login handshake, and drives the authoritative world over TCP.
    // This is the first time D0's client intents (a session command and a game
    // command) cross a real process boundary and come back as authoritative
    // snapshots — proven headless with the client on its own thread and the server
    // ticking on the main one.
    {
        server::DedicatedServer server{saveRoot, /*viewDistanceChunks=*/1};
        server.createAndLoadWorld("networked", 5U, gameplay::GameMode::Survival);
        server.ensureSpawnAreaLoaded(/*radius=*/0);
        server.openListener(/*port=*/0);
        const std::uint16_t port = server.listenPort();
        assert(port != 0);

        std::atomic<bool> clientDone{false};
        std::atomic<bool> clientOk{false};
        std::thread clientThread{[&] {
            auto channel = net::TcpChannel::connect(port);
            const auto handshake = net::performClientHandshake(*channel);
            if (!handshake.ok()) {
                clientDone.store(true);
                return;
            }
            // Two client intents that must reach the authoritative server over the
            // socket: switch to creative (a SessionCommand) and pick hotbar slot 5
            // (a GameCommand).
            net::sendMessage(*channel, net::NetMessage{gameplay::SessionCommand{
                                           gameplay::SetGameMode{gameplay::GameMode::Creative}}});
            net::sendMessage(*channel,
                             net::NetMessage{gameplay::GameCommand{gameplay::SwapSlot{5U}}});

            // Pump the mirror until the server's snapshots reflect both intents.
            NullHost host;
            client::ClientMirror mirror;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
                mirror.pump(*channel, host);
                if (mirror.player().gameMode == gameplay::GameMode::Creative &&
                    mirror.player().selectedHotbarSlot == 5U) {
                    clientOk.store(true);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            clientDone.store(true);
        }};

        // Server: accept the connection (runs the server handshake and attaches
        // it), then tick until the client has observed its intents applied.
        const auto handshake = server.acceptConnection();
        assert(handshake.ok());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (!clientDone.load() && std::chrono::steady_clock::now() < deadline) {
            server.tickOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        clientThread.join();

        assert(clientOk.load());
        // The server applied the client's game-mode intent authoritatively.
        assert(server.runtime().gameSession().gameMode() == gameplay::GameMode::Creative);
    }

    std::cout << "PASS: dedicated_server_test\n";
    return 0;
}
