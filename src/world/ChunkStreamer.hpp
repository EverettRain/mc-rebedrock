#pragma once

#include "render/MeshData.hpp"
#include "world/PersistentBlockEdit.hpp"
#include "world/World.hpp"
#include "world/WorldLightEngine.hpp"
#include "world/gen/TreeGrower.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mc::world {

struct SectionPosition final {
    int chunkX = 0;
    int sectionY = 0;
    int chunkZ = 0;

    [[nodiscard]] bool operator==(const SectionPosition&) const = default;
};

struct SectionPositionHash final {
    [[nodiscard]] std::size_t operator()(const SectionPosition& position) const noexcept;
};

struct BlockEditPosition final {
    int x = 0;
    int y = 0;
    int z = 0;

    [[nodiscard]] bool operator==(const BlockEditPosition&) const = default;
};

struct BlockEditPositionHash final {
    [[nodiscard]] std::size_t operator()(const BlockEditPosition& position) const noexcept;
};

struct SectionMeshUpdate final {
    SectionPosition position;
    render::RenderMeshData mesh;
    bool remove = false;
    std::uint64_t revision = 0U;
    bool highPriority = false;
};

struct ChunkDataUpdate final {
    ChunkPosition position;
    Chunk chunk;
    bool remove = false;
};

// A generation-side state change to an already loaded chunk. Cross-chunk tree
// crowns are the first producer. `expected` makes delivery a compare-and-swap:
// a player/simulation edit made after generation started remains authoritative
// instead of being rewound by a late generation batch.
struct BlockStateDelta final {
    int worldX = 0;
    int y = 0;
    int worldZ = 0;
    BlockState expected{};
    BlockState state{};
};

struct ChunkStreamBatch final {
    std::uint64_t worldEpoch = 0U;
    ChunkPosition center;
    std::size_t loadedChunkCount = 0;
    std::size_t appliedBlockEditCount = 0;
    std::vector<ChunkDataUpdate> chunkUpdates;
    std::vector<BlockStateDelta> stateUpdates;
    std::vector<SectionMeshUpdate> sectionUpdates;
    bool highPriority = false;
};

[[nodiscard]] ChunkPosition chunkPositionFromWorld(float worldX, float worldZ);
[[nodiscard]] std::vector<ChunkPosition> chunkPositionsInRadius(
    ChunkPosition center,
    int radius);
class ChunkStreamer final {
  public:
    ChunkStreamer(std::uint64_t seed, int loadRadius, int unloadRadius);
    ~ChunkStreamer();

    ChunkStreamer(const ChunkStreamer&) = delete;
    ChunkStreamer& operator=(const ChunkStreamer&) = delete;

    void stop();
    void request(ChunkPosition center);
    void setRadii(int loadRadius, int unloadRadius);
    // Marks a region that must never stream out, the way vanilla 1.16.1 keeps
    // its spawn chunks loaded: every chunk within `radius` of `center` is
    // skipped by the unload pass. The world spawn registers itself here once it
    // is known, so the player's home base never despawns under them.
    void protectChunks(ChunkPosition center, int radius);
    [[nodiscard]] std::uint64_t resetWorld(
        std::uint64_t seed,
        std::vector<PersistentBlockEdit> edits = {});
    void setBlock(
        int worldX,
        int y,
        int worldZ,
        Block value,
        std::uint8_t fluidLevel = 0U,
        std::optional<BlockOrientation> orientation = std::nullopt);
    // The whole state in one edit. The overload above builds one of these from
    // a block and its two loose fields; anything with a state that neither can
    // carry — a furnace's LIT — has to come through here, or the edit arrives
    // as a different state than the one the world already holds and the light
    // update is skipped as a no-op.
    void setState(int worldX, int y, int worldZ, BlockState value);
    // Mesh-data reuse pool access for the worker and render threads.
    [[nodiscard]] render::RenderMeshData acquireMeshData() const;
    void releaseMeshData(render::RenderMeshData&& mesh) const;
    [[nodiscard]] std::optional<ChunkStreamBatch> poll();

    // Synchronously request one specific chunk: blocks the caller until a
    // batch delivering that chunk has been published (or `timeout` elapses),
    // then returns that batch. This is the vanilla-style fallback
    // (ServerChunkManager#getChunk with create=true) that lets the render
    // thread force-load the chunk the player is about to enter instead of
    // stopping them at an invisible wall. The worker generates the requested
    // chunks ahead of its normal nearest-first batches.
    [[nodiscard]] std::optional<ChunkStreamBatch> requestSync(
        ChunkPosition position, std::chrono::milliseconds timeout);

    [[nodiscard]] int loadRadius() const {
        return loadRadius_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int unloadRadius() const {
        return unloadRadius_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int protectedRadius() const {
        return protectedRadius_.load(std::memory_order_relaxed);
    }
    // The smooth-lighting quality new meshes are baked with. Changing it on the
    // worker only affects meshes built after the call; the render thread drives
    // a full remesh (requestFullRemesh) so existing sections catch up.
    void setSmoothLightingQuality(SmoothLightingQuality quality) {
        smoothLightingQuality_.store(quality, std::memory_order_relaxed);
    }
    [[nodiscard]] SmoothLightingQuality smoothLightingQuality() const {
        return smoothLightingQuality_.load(std::memory_order_relaxed);
    }
    // Re-meshes every loaded section (used when the baked smooth-lighting
    // quality changes). The worker picks the flag up on its next wake and
    // publishes one high-priority batch so the render thread applies it fast.
    void requestFullRemesh();
    // Re-meshes one section (used by the render thread when the pending-mesh
    // backlog evicts a section that never reached the GPU, so it is not left as
    // a permanent hole). The worker re-meshes just that section and republishes.
    void requestSectionRemesh(SectionPosition position);

  private:
    struct BlockEdit final {
        int worldX = 0;
        int y = 0;
        int worldZ = 0;
        BlockState state{};
    };

    struct WorldReset final {
        std::uint64_t seed = 0U;
        std::uint64_t epoch = 0U;
        std::vector<PersistentBlockEdit> edits;
    };

    void workerLoop();
    // One world update can now yield several batches: chunks are generated,
    // lit and meshed nearest-first in small groups, and each group is published
    // to completed_ as soon as it is ready, so the render thread applies the
    // area around the player long before the far edge of the load radius is
    // done. Published batches are consumed by poll().
    void updateWorld(
        World& world,
        WorldLightEngine& lightEngine,
        ChunkPosition center,
        std::span<const PersistentBlockEdit> persistentEdits,
        std::uint64_t epoch);
    // Finishes the tree crowns a chunk could not reach: applies its pending
    // border blocks (from earlier neighbours) and, after every chunk in a batch
    // is published, applies the batch's own border blocks to the neighbours now
    // present. Blocks whose target chunk is still missing wait in
    // pendingBorderBlocks_ until that chunk is published. Targets inside
    // `batchChunks` are about to be light-initialized by the caller, so they are
    // not individually relit here.
    void applyBorderBlocks(
        World& world,
        WorldLightEngine& lightEngine,
        std::vector<gen::TreeBorderBlock>& blocks,
        const std::unordered_set<ChunkPosition, ChunkPositionHash>& batchChunks,
        const std::unordered_set<BlockEditPosition, BlockEditPositionHash>& persistentPositions,
        std::vector<BlockStateDelta>* stateUpdates = nullptr);
    void rememberBorderBlocks(std::span<const gen::TreeBorderBlock> blocks);
    void remeshAll(World& world, ChunkPosition center, std::uint64_t epoch);
    // Generates any chunk positions the render thread is synchronously waiting
    // on (requestSync), ahead of the normal batch loop.
    void processSyncRequests(
        World& world,
        WorldLightEngine& lightEngine,
        ChunkPosition center,
        std::uint64_t epoch,
        std::span<const PersistentBlockEdit> persistentEdits);
    void publish(ChunkStreamBatch batch);
    [[nodiscard]] ChunkStreamBatch applyBlockEdits(
        World& world,
        WorldLightEngine& lightEngine,
        ChunkPosition center,
        std::uint64_t epoch,
        std::vector<BlockEdit> edits) const;

    std::uint64_t seed_;
    std::atomic<int> loadRadius_;
    std::atomic<int> unloadRadius_;
    // The never-unload spawn region (vanilla spawn chunks). `protectedRadius_`
    // of 0 disables it; the position is only meaningful while enabled.
    std::atomic<int> protectedChunkX_{0};
    std::atomic<int> protectedChunkZ_{0};
    std::atomic<int> protectedRadius_{0};
    std::mutex mutex_;
    std::condition_variable wakeWorker_;
    // Signalled by publish() so requestSync waiters can observe a delivered
    // batch without busy-polling completed_.
    std::condition_variable completedCv_;
    std::optional<ChunkPosition> pendingCenter_;
    std::optional<WorldReset> pendingReset_;
    std::vector<BlockEdit> pendingBlockEdits_;
    std::vector<SectionPosition> pendingSectionRemesh_;
    std::optional<ChunkPosition> lastRequestedCenter_;
    std::deque<ChunkStreamBatch> completed_;
    // Positions the render thread is blocking on via requestSync; the worker
    // generates these ahead of its normal batches. Guarded by mutex_.
    std::unordered_set<ChunkPosition, ChunkPositionHash> syncPending_;
    // Tree crown blocks indexed by their target chunk. Entries remain after an
    // application: if the target unloads and regenerates while the source tree
    // stays loaded, the same crown must be reconstructed instead of becoming
    // clipped. Only touched by the worker thread, so it needs no lock.
    std::unordered_map<ChunkPosition, std::vector<gen::TreeBorderBlock>, ChunkPositionHash>
        pendingBorderBlocks_;
    // Bounded pool of RenderMeshData the worker and render thread hand back and
    // forth so section meshes reuse their vector capacity instead of allocating
    // six vectors per section through the streaming burst.
    struct MeshDataPool final {
        std::mutex mutex;
        std::vector<render::RenderMeshData> free;
    };
    // Mutable so the const world-update paths can still circulate pooled meshes.
    mutable MeshDataPool meshPool_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> fullRemeshRequested_{false};
    std::atomic<SmoothLightingQuality> smoothLightingQuality_{SmoothLightingQuality::Standard};
    std::uint64_t requestedEpoch_ = 0U;
    mutable std::uint64_t nextMeshRevision_ = 0U;
    std::thread worker_;
};

} // namespace mc::world
