#pragma once

#include "gameplay/CommandResult.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/SimulationDriver.hpp"
#include "gameplay/command/CommandDispatcher.hpp"
#include "persistence/SaveRepository.hpp"
#include "world/World.hpp"
#include "world/WorldLock.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mc::world {
class ChunkStreamer;
}

namespace mc::runtime {

// The authoritative world runtime: everything a dedicated server needs, with no
// GLFW or Vulkan anywhere in the dependency chain. It owns the world, the save
// repository, the game session, the simulation driver and the world lock, and
// it is where world loading, saving and switching live.
//
// The renderer is one SimulationHost implementation (GameSession.hpp); a headless
// test supplies a recording stub. The renderer keeps its presentation state —
// audio, menus, the camera, GPU resources, commands, block interaction — and
// reaches the runtime's objects through its accessors.
//
// The member declaration order below is a destruction contract, inherited from
// the renderer layout this class was extracted from: gameSession_ is declared
// before simulationDriver_ so the driver's thread join lands ahead of the
// session's destruction, and the destructor calls stopSimulation() before any
// member the tick touches is torn down. chunkStreamer_ is a reference — the
// application constructs it and it outlives this runtime.
class GameRuntime final {
  public:
    GameRuntime(gameplay::SimulationHost& host, world::ChunkStreamer& chunkStreamer,
                std::filesystem::path saveRoot);
    ~GameRuntime();

    GameRuntime(const GameRuntime&) = delete;
    GameRuntime& operator=(const GameRuntime&) = delete;
    GameRuntime(GameRuntime&&) = delete;
    GameRuntime& operator=(GameRuntime&&) = delete;

    // Simulation control. startSimulation() ticks at 20 TPS on a background
    // thread while simulationActive() is true; MC_REBEDROCK_SYNC_TICK keeps the
    // synchronous fallback (tick() called from the render thread). Headless
    // tests use the synchronous form.
    void startSimulation();
    void stopSimulation();
    void tick();

    // Command dispatch: the chat line the render thread submits is enqueued
    // here and executed on the runtime's dispatcher during the tick, so chat
    // commands are server-authoritative too. The result is read back with
    // takeChatResult() and displayed in the renderer's chat log.
    void enqueueChat(std::string line);
    [[nodiscard]] gameplay::command::CommandDispatcher& commandDispatcher() {
        return commandDispatcher_;
    }
    // The most recently executed chat command's result, consumed (cleared) by
    // the renderer to append to the chat history.
    [[nodiscard]] std::optional<gameplay::CommandResult> takeChatResult();

    // World lifecycle (the authoritative half; the renderer adds presentation
    // around it). loadWorld restores the session, resets the chunk streamer to
    // the world's seed and edits and requests the spawn area. createWorld makes
    // and persists a fresh save. unloadWorld drops the save and unloads the
    // chunks.
    void loadWorld(persistence::SaveGame save, int viewDistanceChunks);
    [[nodiscard]] persistence::SaveGame createWorld(std::string name, std::uint64_t seed,
                                                    gameplay::GameMode mode);
    void unloadWorld();

    // Persists the open world. saveLocked() assumes the caller already holds the
    // world's write section (a command handler saves from inside one); save()
    // takes a read section itself. Returns false when there is no save open.
    [[nodiscard]] bool saveLocked();
    void save();

    // Accessors.
    [[nodiscard]] gameplay::GameSession& gameSession() { return gameSession_; }
    [[nodiscard]] const gameplay::GameSession& gameSession() const { return gameSession_; }
    [[nodiscard]] world::World& world() { return serverWorld_; }
    [[nodiscard]] const world::World& world() const { return serverWorld_; }
    [[nodiscard]] world::WorldLock& lock() { return worldLock_; }
    [[nodiscard]] world::ChunkStreamer& chunkStreamer() { return chunkStreamer_; }
    [[nodiscard]] persistence::SaveRepository& saveRepository() { return saveRepository_; }
    [[nodiscard]] persistence::SaveGame& currentSave() { return *currentSave_; }
    [[nodiscard]] std::optional<persistence::SaveGame>& currentSaveSlot() {
        return currentSave_;
    }
    [[nodiscard]] std::uint64_t& worldEpoch() { return worldEpoch_; }
    [[nodiscard]] std::atomic_bool& simulationActive() { return simulationActive_; }
    [[nodiscard]] gameplay::SimulationDriver& simulationDriver() { return simulationDriver_; }

    // N-Mem, first piece: the server-side resident bytes, measurable headless
    // because no render allocation exists here. Sums each chunk's section state
    // storage plus the light nibble arrays.
    [[nodiscard]] std::size_t serverResidentBytes() const;

  private:
    void registerAuthoritativeCommands();
    void processChatQueue();
    [[nodiscard]] gameplay::CommandResult applySpawnPoint(
        const std::optional<glm::vec3>& position);

    gameplay::SimulationHost& host_;
    persistence::SaveRepository saveRepository_;
    world::ChunkStreamer& chunkStreamer_;
    world::World serverWorld_;
    gameplay::GameSession gameSession_;
    gameplay::SimulationDriver simulationDriver_;
    world::WorldLock worldLock_;
    std::atomic_bool simulationActive_{false};
    std::optional<persistence::SaveGame> currentSave_;
    std::uint64_t worldEpoch_ = 0U;
    // The server-authoritative command tree. The renderer registers its
    // client-only commands on it through commandDispatcher().
    gameplay::command::CommandDispatcher commandDispatcher_;
    std::vector<std::string> chatQueue_;
    std::optional<gameplay::CommandResult> chatResult_;
};

} // namespace mc::runtime
