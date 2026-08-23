#pragma once

#include "core/ParallelWorkerPool.hpp"
#include "render/MeshData.hpp"
#include "world/Dimension.hpp"
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
#include <memory>
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
    // Read-only shared payload: all receiving worlds initially point at the
    // worker's generated chunk, then World performs whole-chunk copy-on-write.
    std::shared_ptr<const Chunk> chunk;
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

// Chunks stay loaded this many rings past the load radius before the unload
// pass takes them. Without the gap, a player lingering on a chunk boundary
// thrashes the same ring load→unload every time the centre flips by one chunk,
// and each unload is a region-file write. The load radius sizes the resident
// window; the unload radius is kept this much larger so the boundary has
// hysteresis. Steady-state callers pass (r, r + kUnloadHysteresisChunks).
inline constexpr int kUnloadHysteresisChunks = 2;
// M-Chunk B-5: this is the server-side chunk source. It generates, lights and
// persists chunks (through the GameRuntime that owns it) and publishes batches;
// the renderer's ClientChunkCache (WorldRenderer::clientCache) is the client
// side that receives those batches and meshes from them. The two worlds stay in
// sync because the same batches and simulation edits feed both.
class ChunkStreamer final {
  public:
    // `dimension` selects which terrain generator the worker uses
    // (DimensionChunkGenerator): the Overworld's SurfaceGenerator by default, or
    // the Nether/End generators once WG-4 stands a per-dimension streamer up. The
    // seed is the *world* seed for every dimension; the nether/end generators
    // derive their own stream from it, so the overworld streamer is unchanged.
    ChunkStreamer(std::uint64_t seed, int loadRadius, int unloadRadius,
                  DimensionId dimension = DimensionId::Overworld);
    ~ChunkStreamer();

    [[nodiscard]] DimensionId dimension() const { return dimension_; }

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
    // N-Mem: the worker's own World is the third resident chunk copy (besides the
    // server world and the client cache). It lives on the worker thread, so the
    // worker publishes its resident bytes to this atomic after each work cycle
    // and outside readers sample it lock-free. Approximate by design (updated
    // only when the worker did work), which is all the memory report needs.
    [[nodiscard]] std::size_t workerWorldResidentBytes() const {
        return workerResidentBytes_.load(std::memory_order_relaxed);
    }
    // Bytes of chunks the worker world solely owns (not shared with server/client
    // via COW). See World::uniqueResidentBytes.
    [[nodiscard]] std::size_t workerWorldUniqueResidentBytes() const {
        return workerUniqueResidentBytes_.load(std::memory_order_relaxed);
    }
    // Reserved bytes of the CPU RenderMeshData reuse pool (kept at peak capacity
    // for reuse — a real resident cost, not a leak). See §7.4#3.
    [[nodiscard]] std::size_t cpuMeshPoolBytes() const;
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
    // Which dimension this streamer generates. Fixed for the streamer's life (a
    // streamer serves one world/dimension), so the generator dispatch is chosen
    // once, not per chunk.
    DimensionId dimension_ = DimensionId::Overworld;
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
    // Generation, initial lighting and meshing are sequential pipeline stages.
    // Reuse one bounded pool across all three instead of creating 1-7 threads
    // for every stage of every 24-chunk streaming batch.
    mutable core::ParallelWorkerPool parallelWorkers_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> fullRemeshRequested_{false};
    std::atomic<SmoothLightingQuality> smoothLightingQuality_{SmoothLightingQuality::Standard};
    std::atomic<std::size_t> workerResidentBytes_{0};
    std::atomic<std::size_t> workerUniqueResidentBytes_{0};
    std::uint64_t requestedEpoch_ = 0U;
    mutable std::uint64_t nextMeshRevision_ = 0U;
    std::thread worker_;
};

} // namespace mc::world
