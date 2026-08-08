#include "world/ChunkStreamer.hpp"

#include "world/ChunkMesher.hpp"
#include "world/SurfaceGenerator.hpp"
#include "world/WorldConstants.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mc::world {
namespace {

// Vanilla sizes its world-generation worker pool as clamp(cores - 1, 1, 7)
// (Util#getServerWorkerExecutor); the chunk pipeline and meshing reuse the same
// bound so generation and meshing get the same throughput headroom the server
// would.
[[nodiscard]] std::size_t parallelWorkerCount(std::size_t requestCount) {
    const std::size_t hardwareThreads = std::max(1U, std::thread::hardware_concurrency());
    const std::size_t maxWorkers =
        std::clamp(hardwareThreads - 1U, std::size_t{1U}, std::size_t{7U});
    return std::min(requestCount, maxWorkers);
}

constexpr std::array<ChunkPosition, 8> kNeighborChunks{{
    {1, 0},
    {-1, 0},
    {0, 1},
    {0, -1},
    {1, 1},
    {1, -1},
    {-1, 1},
    {-1, -1},
}};

struct EditPosition final {
    int x;
    int y;
    int z;

    [[nodiscard]] bool operator==(const EditPosition&) const = default;
};

struct EditPositionHash final {
    [[nodiscard]] std::size_t operator()(const EditPosition& position) const noexcept {
        std::size_t seed = std::hash<int>{}(position.x);
        seed ^= std::hash<int>{}(position.y) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<int>{}(position.z) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

[[nodiscard]] bool outsideRadius(ChunkPosition position, ChunkPosition center, int radius) {
    return std::abs(position.x - center.x) > radius || std::abs(position.z - center.z) > radius;
}

struct ChunkMeshRequest final {
    ChunkPosition position;
    bool skipEmptySections = false;
    std::vector<int> sectionYs;
};

// One chunk to generate, plus the persistent edits that land inside it.
struct GenerationRequest final {
    ChunkPosition position;
    std::span<const PersistentBlockEdit*> edits;
};

struct GenerationResult final {
    ChunkPosition position;
    Chunk chunk;
    // Tree crown blocks that crossed this chunk's border, to be applied to the
    // neighbouring chunks once they are published (see applyBorderBlocks).
    std::vector<gen::TreeBorderBlock> borderBlocks;
};

// Generates the requested chunks on a small pool of worker threads. Chunk
// generation is the expensive part of a world load (noise, carvers, surface,
// features), and unlike meshing it is embarrassingly parallel — each chunk is
// independent. Every worker owns its own SurfaceGenerator (the noise samplers
// are read-only, but a per-thread copy keeps concurrent generation trivially
// safe, the way vanilla gives each ChunkGenerator thread its own generator).
// The generators are constructed once per world update by the caller and
// reused across batches: constructing them is itself nontrivial (each draws a
// dozen octave samplers from the seed stream), so building one per batch would
// tax small batches heavily. SurfaceGenerator is not relocatable, so the pool
// lives behind unique_ptrs and this helper takes a span of raw pointers. Work
// is stolen with an atomic counter, mirroring buildChunkMeshesParallel.
[[nodiscard]] std::vector<GenerationResult> generateChunksParallel(
    std::span<SurfaceGenerator*> generators,
    std::span<const GenerationRequest> requests,
    const std::atomic<bool>& stopping) {
    if (requests.empty() || stopping.load(std::memory_order_relaxed)) {
        return {};
    }
    std::vector<GenerationResult> results(requests.size());
    std::vector<std::exception_ptr> errors(requests.size());
    std::atomic<std::size_t> nextRequest{0U};
    const std::size_t workerCount = std::min(requests.size(), generators.size());
    const auto generateNext = [&](std::size_t workerIndex) {
        SurfaceGenerator& generator = *generators[workerIndex];
        while (!stopping.load(std::memory_order_relaxed)) {
            const std::size_t requestIndex = nextRequest.fetch_add(1U, std::memory_order_relaxed);
            if (requestIndex >= requests.size()) {
                return;
            }
            try {
                const auto& request = requests[requestIndex];
                std::vector<gen::TreeBorderBlock> borderBlocks;
                Chunk chunk = generator.generate(
                    request.position.x, request.position.z, borderBlocks);
                for (const auto* edit : request.edits) {
                    const int localX = edit->x - request.position.x * kChunkWidth;
                    const int localZ = edit->z - request.position.z * kChunkDepth;
                    chunk.setBlock(localX, edit->y, localZ, edit->block);
                    chunk.setOrientation(localX, edit->y, localZ, edit->orientation);
                    if (isFluid(edit->block)) {
                        chunk.setFluidLevel(localX, edit->y, localZ, edit->fluidLevel);
                    }
                }
                results[requestIndex] = {
                    request.position, std::move(chunk), std::move(borderBlocks)};
            } catch (...) {
                errors[requestIndex] = std::current_exception();
            }
        }
    };

    if (workerCount == 1U) {
        generateNext(0U);
    } else {
        std::vector<std::jthread> workers;
        workers.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index) {
            workers.emplace_back(generateNext, index);
        }
    }
    if (!stopping.load(std::memory_order_relaxed)) {
        for (const auto& error : errors) {
            if (error) {
                std::rethrow_exception(error);
            }
        }
    }
    return results;
}

[[nodiscard]] int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    if (value % divisor < 0)
        --quotient;
    return quotient;
}

[[nodiscard]] std::vector<SectionMeshUpdate>
buildChunkMeshesParallel(const World& world, std::span<const ChunkMeshRequest> requests,
                         const std::atomic<bool>& stopping, const ChunkStreamer& self) {
    if (requests.empty() || stopping.load(std::memory_order_relaxed))
        return {};
    std::vector<std::vector<SectionMeshUpdate>> perChunk(requests.size());
    std::vector<std::exception_ptr> errors(requests.size());
    std::atomic<std::size_t> nextRequest{0U};
    const auto buildNext = [&] {
        while (!stopping.load(std::memory_order_relaxed)) {
            const std::size_t requestIndex = nextRequest.fetch_add(1U, std::memory_order_relaxed);
            if (requestIndex >= requests.size())
                return;
            try {
                const auto& request = requests[requestIndex];
                auto& updates = perChunk[requestIndex];
                // One O(1) snapshot per request chunk (shared by all its
                // sections) replaces the ~13 chunk-map lookups every corner
                // used to cost.
                int minimumSectionY = 0;
                int maximumSectionY = kSectionCount - 1;
                if (!request.sectionYs.empty()) {
                    minimumSectionY = *std::ranges::min_element(request.sectionYs);
                    maximumSectionY = *std::ranges::max_element(request.sectionYs);
                }
                const MeshLightingSnapshot lighting{
                    world, request.position, minimumSectionY, maximumSectionY,
                    self.smoothLightingQuality()};
                const std::size_t sectionCount = request.sectionYs.empty()
                                                     ? static_cast<std::size_t>(kSectionCount)
                                                     : request.sectionYs.size();
                updates.reserve(sectionCount);
                for (std::size_t sectionIndex = 0U; sectionIndex < sectionCount; ++sectionIndex) {
                    const int sectionY = request.sectionYs.empty()
                                             ? static_cast<int>(sectionIndex)
                                             : request.sectionYs[sectionIndex];
                    if (stopping.load(std::memory_order_relaxed)) return;
                    // Build into a pooled RenderMeshData so the six vectors'
                    // capacity circulates between the worker and render thread
                    // instead of being reallocated per section.
                    render::RenderMeshData mesh = self.acquireMeshData();
                    static_cast<void>(ChunkMesher::buildSection(
                        world, request.position, sectionY, lighting, mesh));
                    const bool empty = mesh.empty();
                    if (empty && request.skipEmptySections) {
                        self.releaseMeshData(std::move(mesh));
                        continue;
                    }
                    updates.push_back({
                        {request.position.x, sectionY, request.position.z},
                        std::move(mesh), empty, 0U,
                    });
                }
            } catch (...) {
                errors[requestIndex] = std::current_exception();
            }
        }
    };

    const std::size_t workerCount = parallelWorkerCount(requests.size());
    if (workerCount == 1U) {
        buildNext();
    } else {
        std::vector<std::jthread> workers;
        workers.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index) {
            workers.emplace_back(buildNext);
        }
    }
    if (!stopping.load(std::memory_order_relaxed)) {
        for (const auto& error : errors) {
            if (error)
                std::rethrow_exception(error);
        }
    }
    std::vector<SectionMeshUpdate> result;
    for (auto& updates : perChunk) {
        result.insert(result.end(), std::make_move_iterator(updates.begin()),
                      std::make_move_iterator(updates.end()));
    }
    return result;
}

} // namespace

std::size_t SectionPositionHash::operator()(const SectionPosition& position) const noexcept {
    std::size_t seed = ChunkPositionHash{}({position.chunkX, position.chunkZ});
    seed ^= std::hash<int>{}(position.sectionY) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    return seed;
}

render::RenderMeshData ChunkStreamer::acquireMeshData() const {
    std::lock_guard<std::mutex> lock(meshPool_.mutex);
    if (meshPool_.free.empty()) {
        return {};
    }
    render::RenderMeshData result = std::move(meshPool_.free.back());
    meshPool_.free.pop_back();
    return result;
}

void ChunkStreamer::releaseMeshData(render::RenderMeshData&& mesh) const {
    std::lock_guard<std::mutex> lock(meshPool_.mutex);
    // Bound the free list so a teleport burst does not hoard every section's
    // capacity; surplus is simply destroyed.
    if (meshPool_.free.size() < 96U) {
        meshPool_.free.push_back(std::move(mesh));
    }
}

ChunkPosition chunkPositionFromWorld(float worldX, float worldZ) {
    return {
        static_cast<int>(std::floor(worldX / static_cast<float>(kChunkWidth))),
        static_cast<int>(std::floor(worldZ / static_cast<float>(kChunkDepth))),
    };
}

std::vector<ChunkPosition> chunkPositionsInRadius(ChunkPosition center, int radius) {
    if (radius < 0) {
        throw std::invalid_argument("Chunk loading radius cannot be negative");
    }
    const int diameter = radius * 2 + 1;
    std::vector<ChunkPosition> positions;
    positions.reserve(static_cast<std::size_t>(diameter * diameter));
    for (int z = center.z - radius; z <= center.z + radius; ++z) {
        for (int x = center.x - radius; x <= center.x + radius; ++x) {
            positions.push_back({x, z});
        }
    }
    return positions;
}

ChunkStreamer::ChunkStreamer(std::uint64_t seed, int loadRadius, int unloadRadius)
    : seed_(seed), loadRadius_(loadRadius), unloadRadius_(unloadRadius) {
    if (loadRadius < 0 || unloadRadius < loadRadius) {
        throw std::invalid_argument(
            "Chunk unload radius must be greater than or equal to load radius");
    }
    worker_ = std::thread(&ChunkStreamer::workerLoop, this);
}

ChunkStreamer::~ChunkStreamer() { stop(); }

void ChunkStreamer::stop() {
    if (stopping_.exchange(true, std::memory_order_relaxed))
        return;
    wakeWorker_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ChunkStreamer::request(ChunkPosition center) {
    {
        std::scoped_lock lock{mutex_};
        if (lastRequestedCenter_ == center) {
            return;
        }
        lastRequestedCenter_ = center;
        pendingCenter_ = center;
    }
    wakeWorker_.notify_one();
}

void ChunkStreamer::setRadii(int loadRadius, int unloadRadius) {
    if (loadRadius < 0 || unloadRadius < loadRadius) {
        throw std::invalid_argument(
            "Chunk unload radius must be greater than or equal to load radius");
    }
    loadRadius_.store(loadRadius, std::memory_order_relaxed);
    unloadRadius_.store(unloadRadius, std::memory_order_relaxed);
    {
        std::scoped_lock lock{mutex_};
        if (lastRequestedCenter_.has_value()) {
            pendingCenter_ = lastRequestedCenter_;
        }
    }
    wakeWorker_.notify_one();
}

std::uint64_t ChunkStreamer::resetWorld(std::uint64_t seed,
                                        std::vector<PersistentBlockEdit> edits) {
    std::uint64_t epoch = 0U;
    {
        std::scoped_lock lock{mutex_};
        epoch = ++requestedEpoch_;
        pendingReset_ = WorldReset{seed, epoch, std::move(edits)};
        pendingCenter_.reset();
        pendingBlockEdits_.clear();
        lastRequestedCenter_.reset();
        completed_.clear();
    }
    wakeWorker_.notify_one();
    return epoch;
}

void ChunkStreamer::setBlock(int worldX, int y, int worldZ, Block value,
                             std::uint8_t fluidLevel,
                             std::optional<BlockOrientation> orientation) {
    {
        std::scoped_lock lock{mutex_};
        pendingBlockEdits_.push_back(
            {worldX, y, worldZ, value, fluidLevel,
             orientation.value_or(defaultOrientation(value))});
    }
    wakeWorker_.notify_one();
}

std::optional<ChunkStreamBatch> ChunkStreamer::poll() {
    std::scoped_lock lock{mutex_};
    if (completed_.empty()) {
        return std::nullopt;
    }
    const auto highPriority = std::ranges::find_if(
        completed_, [](const ChunkStreamBatch& batch) { return batch.highPriority; });
    const auto selected = highPriority != completed_.end() ? highPriority : completed_.begin();
    ChunkStreamBatch result = std::move(*selected);
    completed_.erase(selected);
    return result;
}

void ChunkStreamer::workerLoop() {
    World world;
    WorldLightEngine lightEngine{&stopping_};
    ChunkPosition currentCenter{};
    std::vector<PersistentBlockEdit> persistentEdits;
    std::unordered_map<EditPosition, std::size_t, EditPositionHash> persistentEditIndices;
    std::uint64_t currentEpoch = 0U;
    while (true) {
        std::optional<WorldReset> reset;
        std::optional<ChunkPosition> requestedCenter;
        std::vector<BlockEdit> edits;
        std::vector<SectionPosition> sectionRemeshes;
        {
            std::unique_lock lock{mutex_};
            wakeWorker_.wait(lock, [this] {
                return stopping_.load(std::memory_order_relaxed) || pendingReset_.has_value() ||
                       pendingCenter_.has_value() || !pendingBlockEdits_.empty() ||
                       !syncPending_.empty() ||
                       !pendingSectionRemesh_.empty() ||
                       fullRemeshRequested_.load(std::memory_order_relaxed);
            });
            if (stopping_.load(std::memory_order_relaxed)) {
                return;
            }
            reset = std::move(pendingReset_);
            pendingReset_.reset();
            requestedCenter = pendingCenter_;
            pendingCenter_.reset();
            edits = std::move(pendingBlockEdits_);
            pendingBlockEdits_.clear();
            sectionRemeshes = std::move(pendingSectionRemesh_);
            pendingSectionRemesh_.clear();
        }
        const bool fullRemesh = fullRemeshRequested_.exchange(false, std::memory_order_relaxed);

        if (reset.has_value()) {
            world = World{};
            static_cast<void>(lightEngine.takeDirtySections());
            currentCenter = {};
            seed_ = reset->seed;
            currentEpoch = reset->epoch;
            persistentEdits = std::move(reset->edits);
            persistentEditIndices.clear();
            persistentEditIndices.reserve(persistentEdits.size());
            for (std::size_t index = 0; index < persistentEdits.size(); ++index) {
                const auto& edit = persistentEdits[index];
                persistentEditIndices.insert_or_assign(EditPosition{edit.x, edit.y, edit.z}, index);
            }
        }
        // Chunks the render thread is blocked on (requestSync) are generated
        // ahead of the normal nearest-first batches so the player's immediate
        // surroundings never lag behind their movement.
        processSyncRequests(
            world, lightEngine, currentCenter, currentEpoch, persistentEdits);
        if (stopping_.load(std::memory_order_relaxed)) return;
        bool editsApplied = false;
        if (!edits.empty() && world.chunkCount() != 0U) {
            for (const auto& edit : edits) {
                const PersistentBlockEdit saved{edit.worldX, edit.y, edit.worldZ, edit.value,
                                                edit.fluidLevel, edit.orientation};
                const EditPosition position{edit.worldX, edit.y, edit.worldZ};
                const auto found = persistentEditIndices.find(position);
                if (found == persistentEditIndices.end()) {
                    persistentEditIndices.emplace(position, persistentEdits.size());
                    persistentEdits.push_back(saved);
                } else {
                    persistentEdits[found->second] = saved;
                }
            }
            publish(applyBlockEdits(
                world, lightEngine, currentCenter, currentEpoch, edits));
            editsApplied = true;
            if (stopping_.load(std::memory_order_relaxed)) return;
        }
        if (requestedCenter.has_value()) {
            currentCenter = *requestedCenter;
            // updateWorld publishes each nearest-first batch as it completes,
            // so poll() sees the player's surroundings while the far edge is
            // still generating.
            updateWorld(world, lightEngine, currentCenter, persistentEdits, currentEpoch);
            if (stopping_.load(std::memory_order_relaxed))
                return;
        }
        if (fullRemesh && !stopping_.load(std::memory_order_relaxed)) {
            // Re-bake every loaded section at the current quality. Runs after
            // the reset/edit/updateWorld steps so the remesh sees the latest
            // world.
            remeshAll(world, currentCenter, currentEpoch);
            if (stopping_.load(std::memory_order_relaxed))
                return;
        }
        if (!sectionRemeshes.empty() && !stopping_.load(std::memory_order_relaxed)) {
            // Re-mesh sections the render thread's backlog evicted before they
            // reached the GPU. This is cheap (no regeneration, no relight).
            std::vector<ChunkMeshRequest> requests;
            requests.reserve(sectionRemeshes.size());
            for (const auto& position : sectionRemeshes) {
                requests.push_back({
                    {position.chunkX, position.chunkZ}, true, {position.sectionY}});
            }
            auto meshUpdates = buildChunkMeshesParallel(world, requests, stopping_, *this);
            if (stopping_.load(std::memory_order_relaxed))
                return;
            for (auto& update : meshUpdates) {
                update.revision = ++nextMeshRevision_;
            }
            ChunkStreamBatch batch;
            batch.worldEpoch = currentEpoch;
            batch.center = currentCenter;
            batch.highPriority = true;
            batch.sectionUpdates = std::move(meshUpdates);
            batch.loadedChunkCount = world.chunkCount();
            publish(std::move(batch));
        }
        if (!edits.empty() && !editsApplied) {
            for (const auto& edit : edits) {
                const PersistentBlockEdit saved{edit.worldX, edit.y, edit.worldZ, edit.value,
                                                edit.fluidLevel, edit.orientation};
                const EditPosition position{edit.worldX, edit.y, edit.worldZ};
                const auto found = persistentEditIndices.find(position);
                if (found == persistentEditIndices.end()) {
                    persistentEditIndices.emplace(position, persistentEdits.size());
                    persistentEdits.push_back(saved);
                } else {
                    persistentEdits[found->second] = saved;
                }
            }
            publish(applyBlockEdits(world, lightEngine, currentCenter, currentEpoch,
                                    std::move(edits)));
            if (stopping_.load(std::memory_order_relaxed))
                return;
        }
    }
}

void ChunkStreamer::requestFullRemesh() {
    fullRemeshRequested_.store(true, std::memory_order_relaxed);
    wakeWorker_.notify_one();
}

void ChunkStreamer::requestSectionRemesh(SectionPosition position) {
    {
        std::scoped_lock lock{mutex_};
        if (std::ranges::find(pendingSectionRemesh_, position) == pendingSectionRemesh_.end()) {
            pendingSectionRemesh_.push_back(position);
        }
    }
    wakeWorker_.notify_one();
}

void ChunkStreamer::remeshAll(World& world, ChunkPosition center, std::uint64_t epoch) {
    const auto positions = world.positions();
    std::vector<ChunkMeshRequest> requests;
    requests.reserve(positions.size());
    for (const auto position : positions) {
        requests.push_back({position, false, {}});
    }
    auto meshUpdates = buildChunkMeshesParallel(world, requests, stopping_, *this);
    if (stopping_.load(std::memory_order_relaxed)) {
        return;
    }
    for (auto& update : meshUpdates) {
        update.revision = ++nextMeshRevision_;
    }
    ChunkStreamBatch batch;
    batch.worldEpoch = epoch;
    batch.center = center;
    batch.highPriority = true;
    batch.sectionUpdates = std::move(meshUpdates);
    batch.loadedChunkCount = world.chunkCount();
    publish(std::move(batch));
}

void ChunkStreamer::publish(ChunkStreamBatch batch) {
    {
        std::scoped_lock lock{mutex_};
        completed_.push_back(std::move(batch));
    }
    // Wake any requestSync caller waiting on this delivery.
    completedCv_.notify_all();
}

std::optional<ChunkStreamBatch> ChunkStreamer::requestSync(
    ChunkPosition position, std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    syncPending_.insert(position);
    wakeWorker_.notify_one();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        for (auto it = completed_.begin(); it != completed_.end(); ++it) {
            const bool delivers = std::ranges::any_of(
                it->chunkUpdates, [position](const ChunkDataUpdate& update) {
                    return !update.remove && update.position == position;
                });
            if (delivers) {
                ChunkStreamBatch result = std::move(*it);
                completed_.erase(it);
                syncPending_.erase(position);
                return result;
            }
        }
        if (completedCv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            syncPending_.erase(position);
            return std::nullopt;
        }
    }
}

void ChunkStreamer::processSyncRequests(
    World& world,
    WorldLightEngine& lightEngine,
    ChunkPosition center,
    std::uint64_t epoch,
    std::span<const PersistentBlockEdit> persistentEdits) {
    std::vector<ChunkPosition> pending;
    {
        std::scoped_lock lock{mutex_};
        if (syncPending_.empty()) {
            return;
        }
        pending.assign(syncPending_.begin(), syncPending_.end());
    }
    if (pending.empty()) {
        return;
    }
    // Index the persistent edits once so a generated chunk can re-apply them.
    std::unordered_map<ChunkPosition, std::vector<const PersistentBlockEdit*>, ChunkPositionHash>
        editsByChunk;
    for (const auto& edit : persistentEdits) {
        if (edit.y < 0 || edit.y >= kWorldHeight) {
            continue;
        }
        editsByChunk[{
                         floorDiv(edit.x, kChunkWidth),
                         floorDiv(edit.z, kChunkDepth),
                     }]
            .push_back(&edit);
    }
    const SurfaceGenerator generator{seed_};
    for (const auto position : pending) {
        if (stopping_.load(std::memory_order_relaxed)) {
            return;
        }
        // Already generated by an in-flight update; its own batch will deliver
        // the chunk, so leave it to the normal publish path.
        if (world.hasChunk(position)) {
            continue;
        }
        std::vector<gen::TreeBorderBlock> borderBlocks;
        Chunk chunk = generator.generate(position.x, position.z, borderBlocks);
        const auto chunkEdits = editsByChunk.find(position);
        if (chunkEdits != editsByChunk.end()) {
            for (const auto* edit : chunkEdits->second) {
                const int localX = edit->x - position.x * kChunkWidth;
                const int localZ = edit->z - position.z * kChunkDepth;
                chunk.setBlock(localX, edit->y, localZ, edit->block);
                chunk.setOrientation(localX, edit->y, localZ, edit->orientation);
                if (isFluid(edit->block)) {
                    chunk.setFluidLevel(localX, edit->y, localZ, edit->fluidLevel);
                }
            }
        }
        world.setChunk(position, std::move(chunk));
        // A tree an already-generated neighbour planted across this border lands
        // here, and this chunk's own border crowns spill into whatever is loaded.
        const std::unordered_set<ChunkPosition, ChunkPositionHash> syncChunk{position};
        const auto pending = pendingBorderBlocks_.find(position);
        if (pending != pendingBorderBlocks_.end()) {
            applyBorderBlocks(world, lightEngine, pending->second, syncChunk);
            pendingBorderBlocks_.erase(pending);
        }
        applyBorderBlocks(world, lightEngine, borderBlocks, syncChunk);
        const ChunkPosition positions[]{position};
        lightEngine.initializeChunks(world, positions);

        // Remesh this chunk and every already-present neighbour whose border
        // now faces real terrain, mirroring one batch of updateWorld.
        std::vector<ChunkPosition> dirty{position};
        for (const auto offset : kNeighborChunks) {
            const ChunkPosition neighbor{position.x + offset.x, position.z + offset.z};
            if (world.hasChunk(neighbor)) {
                dirty.push_back(neighbor);
            }
        }
        const std::unordered_set<ChunkPosition, ChunkPositionHash> newlySet{position};
        std::vector<ChunkMeshRequest> meshRequests;
        meshRequests.reserve(dirty.size());
        for (const auto dirtyPosition : dirty) {
            meshRequests.push_back({dirtyPosition, newlySet.contains(dirtyPosition), {}});
        }
        auto meshUpdates = buildChunkMeshesParallel(world, meshRequests, stopping_, *this);
        for (auto& update : meshUpdates) {
            update.revision = ++nextMeshRevision_;
        }

        ChunkStreamBatch batch;
        batch.worldEpoch = epoch;
        batch.center = center;
        // Sync deliveries skip the streaming queue: the caller applies them
        // immediately and the mesh uploads jump ahead of distant chunks.
        batch.highPriority = true;
        batch.chunkUpdates.push_back({position, *world.chunk(position), false});
        batch.sectionUpdates = std::move(meshUpdates);
        for (auto& update : batch.sectionUpdates) {
            if (update.revision == 0U) {
                update.revision = ++nextMeshRevision_;
            }
        }
        batch.loadedChunkCount = world.chunkCount();
        publish(std::move(batch));
    }
}

void ChunkStreamer::applyBorderBlocks(
    World& world,
    WorldLightEngine& lightEngine,
    std::vector<gen::TreeBorderBlock>& blocks,
    const std::unordered_set<ChunkPosition, ChunkPositionHash>& batchChunks) {
    for (auto& block : blocks) {
        const ChunkPosition target{
            floorDiv(block.worldX, kChunkWidth),
            floorDiv(block.worldZ, kChunkDepth),
        };
        Chunk* chunk = world.chunk(target);
        if (chunk == nullptr) {
            // The crown's other half is still ungenerated; hold it until then.
            pendingBorderBlocks_[target].push_back(block);
            continue;
        }
        const int localX = block.worldX - target.x * kChunkWidth;
        const int localZ = block.worldZ - target.z * kChunkDepth;
        // The placer saw an empty cell out of bounds; against the real terrain
        // only air, leaves and plants yield — never a log, stone or water.
        if (!gen::treeReplaceable(chunk->block(localX, block.y, localZ))) {
            continue;
        }
        chunk->setBlock(localX, block.y, localZ, block.block);
        chunk->setOrientation(localX, block.y, localZ, block.orientation);
        // Chunks in the batch are light-initialized by the caller right after
        // this pass; already-present neighbours need the targeted relight here.
        if (!batchChunks.contains(target)) {
            lightEngine.updateBlock(world, block.worldX, block.y, block.worldZ);
        }
    }
}

void ChunkStreamer::updateWorld(
    World& world,
    WorldLightEngine& lightEngine,
    ChunkPosition center,
    std::span<const PersistentBlockEdit> persistentEdits,
    std::uint64_t epoch) {
    // Saved worlds can contain many edits. Index them once per stream batch
    // instead of rescanning the entire save history for every generated chunk.
    std::unordered_map<ChunkPosition, std::vector<const PersistentBlockEdit*>, ChunkPositionHash>
        editsByChunk;
    editsByChunk.reserve(persistentEdits.size());
    for (const auto& edit : persistentEdits) {
        if (edit.y < 0 || edit.y >= kWorldHeight)
            continue;
        editsByChunk[{
                         floorDiv(edit.x, kChunkWidth),
                         floorDiv(edit.z, kChunkDepth),
                     }]
            .push_back(&edit);
    }

    const int loadRadius = loadRadius_.load(std::memory_order_relaxed);
    const int unloadRadius = unloadRadius_.load(std::memory_order_relaxed);
    // Nearest-first work order: chunks closest to the player are generated, lit
    // and meshed before the far corners, the same distance-to-player priority
    // vanilla's ChunkTaskPrioritySystem applies to its status passes.
    const auto orderByDistance = [center](const ChunkPosition& position) {
        return std::tuple{
            std::max(std::abs(position.x - center.x), std::abs(position.z - center.z)),
            std::abs(position.x - center.x) + std::abs(position.z - center.z),
            position.z,
            position.x,
        };
    };
    std::vector<ChunkPosition> missing;
    for (const auto position : chunkPositionsInRadius(center, loadRadius)) {
        if (stopping_.load(std::memory_order_relaxed))
            return;
        if (!world.hasChunk(position)) {
            missing.push_back(position);
        }
    }
    std::ranges::sort(missing, {}, orderByDistance);

    // The trailing edge falls outside the unload radius once the center moves.
    // Unload it up front so the neighbours it leaves behind are remeshed with
    // correct data, but deliver the removal records with the first generation
    // batch: a lone removal batch would briefly leave the player standing in a
    // void while the leading-edge chunks still stream in.
    std::vector<ChunkPosition> unloading;
    for (const auto position : world.positions()) {
        if (stopping_.load(std::memory_order_relaxed))
            return;
        if (outsideRadius(position, center, unloadRadius)) {
            unloading.push_back(position);
        }
    }
    const std::unordered_set<ChunkPosition, ChunkPositionHash> unloadingSet{unloading.begin(),
                                                                            unloading.end()};
    std::vector<ChunkPosition> unloadNeighbors;
    for (const auto position : unloading) {
        for (const auto offset : kNeighborChunks) {
            const ChunkPosition neighbor{position.x + offset.x, position.z + offset.z};
            if (world.hasChunk(neighbor) && !unloadingSet.contains(neighbor)) {
                unloadNeighbors.push_back(neighbor);
            }
        }
        world.removeChunk(position);
        lightEngine.updateAfterChunkRemoval(world, position);
    }

    // SurfaceGenerator construction is nontrivial (each draws a dozen octave
    // samplers from the seed stream), so build one per worker once and share
    // them across all batches of this update instead of rebuilding per batch.
    std::vector<std::unique_ptr<SurfaceGenerator>> generatorPool;
    std::vector<SurfaceGenerator*> generators;
    if (!missing.empty()) {
        const std::size_t workerCount = parallelWorkerCount(missing.size());
        generatorPool.reserve(workerCount);
        generators.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index) {
            generatorPool.push_back(std::make_unique<SurfaceGenerator>(seed_));
            generators.push_back(generatorPool.back().get());
        }
    }

    // Generation batches, nearest ring first. Each batch runs the full
    // generate -> light -> mesh sequence and is delivered on its own, so the
    // area around the player becomes usable (CPU data for collision, meshes for
    // rendering) long before the far edge of the load radius is done. The world
    // is touched serially here, between parallel passes, as before.
    constexpr std::size_t kBatchSize = 24;
    bool unloadDelivered = false;
    for (std::size_t offset = 0; offset < missing.size(); offset += kBatchSize) {
        if (stopping_.load(std::memory_order_relaxed))
            return;
        // Serve any synchronous requests between batches so a render thread
        // blocked in requestSync is not held up by a full radius reload.
        if (!syncPending_.empty()) {
            processSyncRequests(world, lightEngine, center, epoch, persistentEdits);
        }
        const std::size_t count = std::min(kBatchSize, missing.size() - offset);
        const std::span<const ChunkPosition> batchPositions{missing.data() + offset, count};

        std::vector<GenerationRequest> requests;
        requests.reserve(count);
        for (const auto position : batchPositions) {
            const auto chunkEdits = editsByChunk.find(position);
            requests.push_back({
                position,
                chunkEdits != editsByChunk.end()
                    ? std::span<const PersistentBlockEdit*>{chunkEdits->second}
                    : std::span<const PersistentBlockEdit*>{},
            });
        }
        auto generated = generateChunksParallel(generators, requests, stopping_);

        // Chunks that became dirty for this batch: the newly generated ones and
        // every already-present neighbour whose border now faces real terrain.
        std::vector<ChunkPosition> dirty;
        dirty.reserve(count * 9U);
        const std::unordered_set<ChunkPosition, ChunkPositionHash> batchChunks{
            batchPositions.begin(), batchPositions.end()};
        for (auto& result : generated) {
            if (stopping_.load(std::memory_order_relaxed))
                return;
            const auto position = result.position;
            world.setChunk(position, std::move(result.chunk));
            dirty.push_back(position);
            // Crown blocks an earlier neighbour left for this chunk (its tree
            // crossed the border before the chunk existed) now land in place.
            const auto pending = pendingBorderBlocks_.find(position);
            if (pending != pendingBorderBlocks_.end()) {
                applyBorderBlocks(world, lightEngine, pending->second, batchChunks);
                pendingBorderBlocks_.erase(pending);
            }
            for (const auto neighborOffset : kNeighborChunks) {
                const ChunkPosition neighbor{
                    position.x + neighborOffset.x, position.z + neighborOffset.z};
                if (world.hasChunk(neighbor)) {
                    dirty.push_back(neighbor);
                }
            }
        }
        // Finish this batch's own border crowns in the neighbours now present;
        // blocks whose target chunk is still missing wait in pendingBorderBlocks_.
        for (auto& result : generated) {
            applyBorderBlocks(world, lightEngine, result.borderBlocks, batchChunks);
        }

        lightEngine.initializeChunks(world, batchPositions);

        ChunkStreamBatch batch;
        batch.worldEpoch = epoch;
        batch.center = center;
        if (!unloadDelivered) {
            unloadDelivered = true;
            if (!unloading.empty()) {
                for (const auto position : unloading) {
                    batch.chunkUpdates.push_back({position, {}, true});
                    for (int sectionY = 0; sectionY < kSectionCount; ++sectionY) {
                        batch.sectionUpdates.push_back({
                            {position.x, sectionY, position.z},
                            {},
                            true,
                        });
                    }
                }
                dirty.insert(dirty.end(), unloadNeighbors.begin(), unloadNeighbors.end());
            }
        }
        for (const auto position : batchPositions) {
            batch.chunkUpdates.push_back({position, *world.chunk(position), false});
        }

        // Remesh the dirty set, nearest first; after the sort duplicate border
        // hits are adjacent and collapse before meshing.
        std::ranges::sort(dirty, {}, orderByDistance);
        dirty.erase(std::ranges::unique(dirty).begin(), dirty.end());
        const std::unordered_set<ChunkPosition, ChunkPositionHash> newlySet{
            batchPositions.begin(), batchPositions.end()};
        std::vector<ChunkMeshRequest> meshRequests;
        meshRequests.reserve(dirty.size());
        for (const auto position : dirty) {
            meshRequests.push_back({position, newlySet.contains(position), {}});
        }
        auto meshUpdates = buildChunkMeshesParallel(world, meshRequests, stopping_, *this);
        for (auto& update : meshUpdates) update.revision = ++nextMeshRevision_;
        batch.sectionUpdates.insert(batch.sectionUpdates.end(),
                                    std::make_move_iterator(meshUpdates.begin()),
                                    std::make_move_iterator(meshUpdates.end()));
        for (auto& update : batch.sectionUpdates) {
            if (update.revision == 0U) update.revision = ++nextMeshRevision_;
        }
        batch.loadedChunkCount = world.chunkCount();
        publish(std::move(batch));
    }

    // A pure trailing-edge shift (every chunk inside the load radius already
    // exists) still needs its removal records delivered.
    if (!unloadDelivered && !unloading.empty()) {
        if (stopping_.load(std::memory_order_relaxed))
            return;
        ChunkStreamBatch batch;
        batch.worldEpoch = epoch;
        batch.center = center;
        for (const auto position : unloading) {
            batch.chunkUpdates.push_back({position, {}, true});
            for (int sectionY = 0; sectionY < kSectionCount; ++sectionY) {
                batch.sectionUpdates.push_back({
                    {position.x, sectionY, position.z},
                    {},
                    true,
                });
            }
        }
        std::vector<ChunkMeshRequest> meshRequests;
        meshRequests.reserve(unloadNeighbors.size());
        for (const auto position : unloadNeighbors) {
            meshRequests.push_back({position, false, {}});
        }
        auto meshUpdates = buildChunkMeshesParallel(world, meshRequests, stopping_, *this);
        for (auto& update : meshUpdates) update.revision = ++nextMeshRevision_;
        batch.sectionUpdates.insert(batch.sectionUpdates.end(),
                                    std::make_move_iterator(meshUpdates.begin()),
                                    std::make_move_iterator(meshUpdates.end()));
        for (auto& update : batch.sectionUpdates) {
            if (update.revision == 0U) update.revision = ++nextMeshRevision_;
        }
        batch.loadedChunkCount = world.chunkCount();
        publish(std::move(batch));
    }

    static_cast<void>(lightEngine.takeDirtySections());
}

ChunkStreamBatch ChunkStreamer::applyBlockEdits(World& world,
                                                WorldLightEngine& lightEngine,
                                                ChunkPosition center,
                                                std::uint64_t epoch,
                                                std::vector<BlockEdit> edits) const {
    ChunkStreamBatch batch;
    batch.worldEpoch = epoch;
    batch.center = center;
    batch.highPriority = true;
    std::unordered_set<SectionPosition, SectionPositionHash> dirtySections;

    const auto markSection = [&world, &dirtySections](SectionPosition position) {
        if (position.sectionY >= 0 && position.sectionY < kSectionCount &&
            world.hasChunk({position.chunkX, position.chunkZ})) {
            dirtySections.insert(position);
        }
    };

    for (const auto& edit : edits) {
        if (stopping_.load(std::memory_order_relaxed))
            return batch;
        if (edit.y < 0 || edit.y >= kWorldHeight) {
            continue;
        }
        const auto previous = world.block(edit.worldX, edit.y, edit.worldZ);
        if (!world.setBlock(edit.worldX, edit.y, edit.worldZ, edit.value)) {
            continue;
        }
        world.setOrientation(edit.worldX, edit.y, edit.worldZ, edit.orientation);
        if (isFluid(edit.value)) {
            world.setFluidLevel(edit.worldX, edit.y, edit.worldZ, edit.fluidLevel);
        }
        ++batch.appliedBlockEditCount;
        // Geometry and vertex AO sample one voxel beyond a section boundary.
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int sampleY = edit.y + dy;
                    if (sampleY < 0 || sampleY >= kWorldHeight) continue;
                    const ChunkPosition sampleChunk = chunkPositionFromWorld(
                        static_cast<float>(edit.worldX + dx),
                        static_cast<float>(edit.worldZ + dz));
                    markSection({sampleChunk.x, sampleY / kSectionSize, sampleChunk.z});
                }
            }
        }
        // Most random-tick edits swap blocks with identical light behaviour —
        // grass reverting to dirt, dirt greening over, crop stages — and
        // updateBlock recomputes the whole 256-block sky column plus a settle
        // pass over both channels even when nothing changes. That wasted work
        // on thousands of grass edits saturated the worker and pushed genuine
        // light changes (a grown tree) seconds behind their meshes. The edit's
        // own section is already marked for remesh above, so only the light
        // channels need this treatment, and only when they can actually differ.
        if (skyLightOpacity(previous) != skyLightOpacity(edit.value) ||
            emittedLight(previous) != emittedLight(edit.value)) {
            lightEngine.updateBlock(world, edit.worldX, edit.y, edit.worldZ);
        }
    }

    for (const auto position : lightEngine.takeDirtySections()) {
        const Chunk* chunk = world.chunk({position.chunkX, position.chunkZ});
        // Empty sections have no vertices that can consume changed lighting.
        // An edited section that became empty is covered above and still emits
        // the required removal update.
        if (chunk != nullptr && !chunk->section(position.sectionY).empty()) {
            markSection({position.chunkX, position.sectionY, position.chunkZ});
        }
    }

    // The render thread applies gameplay edits to its authoritative World
    // immediately. Sending a complete 16x256x16 Chunk snapshot back for every
    // edit duplicated several MiB during water/sand cascades and could also
    // overwrite newer gameplay state. Only remeshed sections are required here.
    std::unordered_map<ChunkPosition, std::vector<int>, ChunkPositionHash> sectionsByChunk;
    for (const auto position : dirtySections) {
        sectionsByChunk[{position.chunkX, position.chunkZ}].push_back(position.sectionY);
    }
    std::vector<ChunkMeshRequest> meshRequests;
    meshRequests.reserve(sectionsByChunk.size());
    for (auto& [position, sectionYs] : sectionsByChunk) {
        std::ranges::sort(sectionYs);
        meshRequests.push_back({position, false, std::move(sectionYs)});
    }
    std::ranges::sort(meshRequests, {}, [](const ChunkMeshRequest& request) {
        return std::pair{request.position.z, request.position.x};
    });
    batch.sectionUpdates = buildChunkMeshesParallel(world, meshRequests, stopping_, *this);
    for (auto& update : batch.sectionUpdates) update.revision = ++nextMeshRevision_;
    batch.loadedChunkCount = world.chunkCount();
    return batch;
}

} // namespace mc::world
