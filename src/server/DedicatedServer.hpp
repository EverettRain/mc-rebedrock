#pragma once

// The dedicated server (stage C, §8.2 / D7): the authoritative runtime running
// headless, with no render or Vulkan anywhere. It is the payoff of the whole
// client/server split — once D0 removed the renderer's direct reach into the
// session, the runtime is self-sufficient, and this binary proves it by running
// the full gameplay chain (world generation, streaming, simulation, persistence)
// with nothing but the runtime linked. game_runtime_test is its functional
// ancestor; this promotes that headless chain into a real, long-running server.
//
// It can run the world locally (create/load, stream the spawn area, tick at the
// caller's cadence, save), or accept one TCP client, gate it through the protocol
// handshake, and attach that MessageChannel to the runtime in place of loopback.
// Multi-client player ownership and reconnect/accept loops remain later work.
//
// PACK-1: an optional `resourceRoot` is this build's built-in resources
// directory — the *data* half only (recipes/loot/tags/entity_attributes/
// biome spawn tables' `data/` floor), read through the same headless
// DirectoryResourceProvider the runtime already links (assets/ResourceProvider
// is in mc_rebedrock_runtime; it is the render/Vulkan/atlas machinery, not
// resource IO itself, that this binary must never link — see PACK REGULAR
// #1's "dedicated server has no resource stack" guardrail, which is about the
// client Resources PackStackKind, never built here). When given, loadWorld
// scans and rebuilds the open save's `<save>/datapacks/` the same way the
// integrated single-player runtime does — proving the authoritative per-save
// data-pack path with zero render dependency. Omitted (the default), the
// server behaves exactly as before this card: no data-pack scan, tables stay
// at whatever their own lazy built-in defaults are.

#include "assets/ResourceProvider.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "net/Handshake.hpp"
#include "net/TcpTransport.hpp"
#include "runtime/GameRuntime.hpp"
#include "server/ServerHost.hpp"
#include "world/ChunkStreamer.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace mc::server {

class DedicatedServer final {
  public:
    // saveRoot is the directory worlds live in; viewDistanceChunks is the
    // simulation/stream radius the server keeps loaded around the player.
    // resourceRoot (PACK-1) is this build's resources/ directory; empty keeps
    // the pre-PACK-1 behaviour (see the class comment above).
    explicit DedicatedServer(std::filesystem::path saveRoot, int viewDistanceChunks = 8,
                             std::filesystem::path resourceRoot = {})
        : builtinEntities_{}, viewDistanceChunks_{viewDistanceChunks},
          streamer_{0U, viewDistanceChunks, viewDistanceChunks + 2},
          dataBase_{resourceRoot.empty() ? std::nullopt
                                         : std::optional<assets::DirectoryResourceProvider>{
                                               std::in_place, std::move(resourceRoot)}},
          runtime_{host_, streamer_, std::move(saveRoot),
                   dataBase_.has_value() ? &*dataBase_ : nullptr} {
        host_.save = &runtime_.currentSaveSlot();
    }

    DedicatedServer(const DedicatedServer&) = delete;
    DedicatedServer& operator=(const DedicatedServer&) = delete;

    // Creates a fresh world and opens it. The world's seed reseeds the streamer
    // (loadWorld does that), so the constructor's placeholder seed does not
    // matter.
    void createAndLoadWorld(std::string name, std::uint64_t seed, gameplay::GameMode mode) {
        auto save = runtime_.createWorld(std::move(name), seed, mode);
        host_.save = &runtime_.currentSaveSlot();
        runtime_.loadWorld(std::move(save), viewDistanceChunks_);
    }

    // Opens an existing world by its identifier. Returns false when no such save
    // exists.
    [[nodiscard]] bool loadWorld(const std::string& identifier) {
        // load() throws when the save is absent, so check the listing first — a
        // missing world is a caller-handled "not found", not an exception.
        bool exists = false;
        for (const auto& summary : runtime_.saveRepository().list()) {
            if (summary.identifier == identifier) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            return false;
        }
        runtime_.loadWorld(runtime_.saveRepository().load(identifier), viewDistanceChunks_);
        host_.save = &runtime_.currentSaveSlot();
        return true;
    }

    // Opens the first existing world whose display name matches, since a world's
    // identifier is a generated slug, not its name. Returns false when none
    // matches (the caller then creates one).
    [[nodiscard]] bool loadWorldByName(const std::string& name) {
        for (const auto& summary : runtime_.saveRepository().list()) {
            if (summary.displayName == name) {
                return loadWorld(summary.identifier);
            }
        }
        return false;
    }

    // The open world's identifier (the generated slug), for a later reload.
    [[nodiscard]] const std::string& worldIdentifier() {
        return runtime_.currentSave().summary.identifier;
    }

    // Force-loads the spawn area synchronously before the first tick — vanilla's
    // "Preparing spawn area". Without it the player would tick over an unloaded
    // column (the controller early-outs there) while the async streamer slowly
    // catches up; a bounded run would finish before any chunk arrived. Pulls the
    // (2*radius+1)^2 columns around the player and applies each batch at once.
    void ensureSpawnAreaLoaded(int radius = 1) {
        const auto position = runtime_.gameSession().player().position();
        const auto center = world::chunkPositionFromWorld(position.x, position.z);
        streamer_.request(center);  // Centre the worker before force-loading.
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const world::ChunkPosition column{center.x + dx, center.z + dz};
                if (auto batch = streamer_.requestSync(column, std::chrono::seconds(10))) {
                    applyBatch(*batch);
                }
            }
        }
    }

    // Advances one authoritative tick: point the streamer at the player's column
    // (so its worker keeps the play area loaded and then idles, exactly as the
    // render loop's per-frame request does — without it the worker never settles
    // on a centre), apply whatever chunks it has ready, then tick.
    void tickOnce() {
        const auto position = runtime_.gameSession().player().position();
        streamer_.request(world::chunkPositionFromWorld(position.x, position.z));
        drainStreamedChunks();
        runtime_.tick();
    }

    // Runs a fixed number of ticks synchronously (tests, or a bounded run).
    void runForTicks(int ticks) {
        for (int tick = 0; tick < ticks; ++tick) {
            tickOnce();
        }
    }

    // Persists the open world.
    void save() { runtime_.save(); }

    // Opens the TCP listener (port 0 lets the OS choose, reported by listenPort).
    // Until a connection is accepted the server runs the world locally.
    void openListener(std::uint16_t port = 0) {
        listener_ = std::make_unique<net::TcpListener>(port);
    }
    [[nodiscard]] std::uint16_t listenPort() const {
        return listener_ != nullptr ? listener_->port() : 0U;
    }

    // Blocks for one client, runs the login handshake, and — only if it is
    // accepted — attaches the connection to the runtime so the tick loop drives
    // client intents and snapshots over the socket instead of the loopback. This
    // is where D0's client intents (movement, session commands) first cross a real
    // process boundary. Returns the handshake outcome; a rejected client leaves no
    // connection attached.
    [[nodiscard]] net::HandshakeResult acceptConnection(
        std::chrono::milliseconds timeout = net::kDefaultHandshakeTimeout) {
        connection_ = listener_->accept();
        auto result = net::performServerHandshake(*connection_, net::kProtocolVersion, timeout);
        if (result.ok()) {
            runtime_.attachConnection(*connection_);
        } else {
            connection_.reset();
        }
        return result;
    }

    // True while a client is connected. Once the peer drops (closed()), the tick
    // loop should detachConnection() and go back to (or wait for) a connection.
    [[nodiscard]] bool hasConnection() const {
        return connection_ != nullptr && !connection_->closed();
    }
    void dropConnection() {
        runtime_.detachConnection();
        connection_.reset();
    }

    // Applies every chunk batch the streamer has ready right now, non-blocking.
    void drainStreamedChunks() {
        while (auto batch = streamer_.poll()) {
            applyBatch(*batch);
        }
    }

    [[nodiscard]] runtime::GameRuntime& runtime() { return runtime_; }
    [[nodiscard]] std::size_t loadedChunkCount() const { return runtime_.world().chunkCount(); }
    [[nodiscard]] std::size_t serverResidentBytes() const {
        return runtime_.serverResidentBytes();
    }

  private:
    // GameRuntime constructs GameSession, whose NaturalSpawner snapshots the
    // entity registry immediately. This sentinel is declared before runtime_ so
    // built-in types exist before that member construction begins; registering in
    // the constructor body would be too late and permanently cache empty spawn
    // tables for the dedicated server.
    struct BuiltinEntityRegistration final {
        BuiltinEntityRegistration() { gameplay::entities::registerBuiltinEntities(); }
    };

    // Applies one streamer batch to the authoritative world: chunk data lands, and
    // state updates land only when the CAS expectation still holds — the same
    // epoch/CAS discipline the render side uses, so a batch generated against a
    // world edit the tick has since changed is dropped rather than clobbering it.
    //
    // CS-5: also drives the M-3 C5 / CS-4 chunk-loaded/unloaded hooks
    // (persistUnloadedChunk / restoreLoadedChunk) the same way
    // VulkanRenderer.cpp's onChunkUnloaded/onChunkLoaded callbacks do — same
    // call, same order relative to the world edit (unload persists then
    // removes the chunk data; load restores after the chunk data lands). This
    // was a known gap (see the CS README's CS-4 entry): the headless dedicated
    // server ran world generation but never triggered the generation-time
    // creature pass or the unload/reload herd round-trip, because nothing
    // called either hook. Low-cost fix — the exact same two calls the renderer
    // already makes, just made from this loop instead.
    void applyBatch(const world::ChunkStreamBatch& batch) {
        if (batch.worldEpoch != runtime_.worldEpoch()) {
            return;
        }
        for (const auto& update : batch.chunkUpdates) {
            if (update.remove) {
                runtime_.persistUnloadedChunk(update.position);
                runtime_.world().removeChunk(update.position);
            } else {
                runtime_.world().setChunk(update.position, update.chunk);
                runtime_.restoreLoadedChunk(update.position);
            }
        }
        for (const auto& update : batch.stateUpdates) {
            if (runtime_.world().state(update.worldX, update.y, update.worldZ) == update.expected) {
                static_cast<void>(runtime_.world().setState(update.worldX, update.y,
                                                            update.worldZ, update.state));
            }
        }
    }

    BuiltinEntityRegistration builtinEntities_;
    int viewDistanceChunks_;
    ServerHost host_;
    world::ChunkStreamer streamer_;
    // PACK-1: declared before runtime_ so it is fully constructed before
    // runtime_'s member-initialiser takes its address; std::nullopt when no
    // resourceRoot was given (runtime_ then gets a null dataBase pointer, the
    // pre-PACK-1 no-op path).
    std::optional<assets::DirectoryResourceProvider> dataBase_;
    // The listener and the accepted connection are declared before runtime_ so
    // they are destroyed *after* it: runtime_'s destructor stops the tick/sim
    // threads first, so nothing publishes to the connection after it is gone.
    std::unique_ptr<net::TcpListener> listener_;
    std::unique_ptr<net::TcpChannel> connection_;
    runtime::GameRuntime runtime_;
};

}  // namespace mc::server
