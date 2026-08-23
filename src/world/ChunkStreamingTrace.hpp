#pragma once

// CS-1: chunk-streaming diagnostic instrumentation (evidence-only, no behaviour
// change). This header is the CS-Load lane's counterpart to
// core/FrameTrace.hpp: env-gated, off by default, and cheap enough when
// disabled that it can sit in ChunkStreamer/WorldRenderer's hot paths.
//
//   MC_REBEDROCK_CHUNK_TRACE  set to any value to enable recording.
//
// Scope (see docs/content-dev/CS-chunk-streaming/README.md, task CS-1):
//   1. Three metrics aligned to PENDING_WORK:terrain-multidraw-indirect.md
//      §1.1: firstMeshLatency / radiusFillTime / streamingFrameCost.
//   2. A missing-chunk detector: chunks the request radius expects to have a
//      resident mesh that never got one (or lost one without a re-request),
//      surfacing the three suspects in the README's problem ③ analysis
//      (border-remesh gap / epoch-cancel gap / delivery race).
//   3. A delivery-order trace: records the ring distance of each section as
//      it becomes GPU-visible, so a caller can check whether delivery order
//      is a monotonic ring expansion (README problem ②).
//
// Discipline: this file only measures. It must never change what
// ChunkStreamer generates, meshes, delivers, or what WorldRenderer uploads —
// see CS-2/CS-3 for behaviour changes, which must cite evidence gathered
// here.
//
// DOD shape: every recorder is a fixed-capacity ring buffer sized at
// construction (no per-event heap allocation); when disabled, call sites pay
// one relaxed atomic/bool load and return, so the class is safe to leave
// wired into ChunkStreamer's worker loop and WorldRenderer's frame loop.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace mc::diag {

[[nodiscard]] inline bool chunkTraceEnabled() {
    static const bool enabled = std::getenv("MC_REBEDROCK_CHUNK_TRACE") != nullptr;
    return enabled;
}

// ---------------------------------------------------------------------------
// 1. Three metrics (MDI §1.1 alignment)
// ---------------------------------------------------------------------------

// A section position expressed as plain ints so this header does not need to
// depend on world/ChunkStreamer.hpp's SectionPosition (kept diagnostics-only
// and dependency-light; callers pass the fields they already have).
struct TraceSectionKey final {
    int chunkX = 0;
    int sectionY = 0;
    int chunkZ = 0;

    [[nodiscard]] bool operator==(const TraceSectionKey&) const = default;
};

struct TraceSectionKeyHash final {
    [[nodiscard]] std::size_t operator()(const TraceSectionKey& key) const noexcept {
        std::size_t seed = std::hash<int>{}(key.chunkX);
        seed ^= std::hash<int>{}(key.sectionY) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<int>{}(key.chunkZ) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

// One completed sample per metric. Ring buffers so a long session cannot grow
// unbounded; the tail (most recent kCapacity samples) is what matters for a
// live diagnosis session.
template <typename Sample, std::size_t Capacity>
class SampleRing final {
  public:
    void push(const Sample& sample) {
        if (samples_.size() < Capacity) {
            samples_.push_back(sample);
        } else {
            samples_[nextIndex_] = sample;
            nextIndex_ = (nextIndex_ + 1U) % Capacity;
        }
        ++totalCount_;
    }

    [[nodiscard]] std::size_t totalCount() const { return totalCount_; }
    [[nodiscard]] const std::vector<Sample>& samples() const { return samples_; }
    void clear() {
        samples_.clear();
        nextIndex_ = 0U;
        totalCount_ = 0U;
    }

  private:
    std::vector<Sample> samples_;
    std::size_t nextIndex_ = 0U;
    std::size_t totalCount_ = 0U;
};

// firstMeshLatency: wall-clock from a chunk position entering the request
// radius to its first section mesh becoming GPU-visible.
struct FirstMeshLatencySample final {
    int chunkX = 0;
    int chunkZ = 0;
    double latencyMs = 0.0;
};

// radiusFillTime: wall-clock from a new request centre to every chunk inside
// the load radius having at least one resident (or confirmed-empty) mesh.
struct RadiusFillSample final {
    int centerX = 0;
    int centerZ = 0;
    int radius = 0;
    double fillMs = 0.0;
    std::size_t chunkCount = 0;
};

// streamingFrameCost: per-frame CPU cost attributable to streaming (batch
// application + GPU upload prep), independent of frame-trace's broader
// unload/persist accounting.
struct StreamingFrameCostSample final {
    double queueBatchMs = 0.0;
    double uploadPrepMs = 0.0;
    std::size_t sectionsUploaded = 0;
};

class ChunkStreamingMetrics final {
  public:
    using Clock = std::chrono::steady_clock;

    // --- firstMeshLatency -------------------------------------------------
    // Called when a chunk position first enters the request radius (a
    // generation batch names it as newly loaded). No-op if already armed.
    void armFirstMesh(TraceSectionKey chunkKey, Clock::time_point requestedAt) {
        std::scoped_lock lock{mutex_};
        firstMeshArmed_.try_emplace(chunkKey, requestedAt);
    }
    // Called the first time a section belonging to that chunk becomes
    // GPU-visible (prepareStreamingUpdates uploads it). Disarms so later
    // sections of the same chunk do not re-trigger.
    void recordFirstMesh(TraceSectionKey chunkKey, Clock::time_point uploadedAt) {
        std::scoped_lock lock{mutex_};
        const auto found = firstMeshArmed_.find(chunkKey);
        if (found == firstMeshArmed_.end()) {
            return;
        }
        const double latencyMs =
            std::chrono::duration<double, std::milli>(uploadedAt - found->second).count();
        firstMesh_.push({chunkKey.chunkX, chunkKey.chunkZ, latencyMs});
        firstMeshArmed_.erase(found);
    }

    // --- radiusFillTime -----------------------------------------------------
    void beginRadiusFill(int centerX, int centerZ, int radius, std::size_t expectedChunkCount,
                         Clock::time_point startedAt) {
        std::scoped_lock lock{mutex_};
        radiusFillArmed_ = RadiusFillArm{centerX, centerZ, radius, expectedChunkCount, startedAt};
    }
    // Called after each batch's chunks become resident; completes the sample
    // once `residentCount` reaches the armed expectation for the same centre.
    void noteRadiusProgress(int centerX, int centerZ, std::size_t residentCount,
                            Clock::time_point now) {
        std::scoped_lock lock{mutex_};
        if (!radiusFillArmed_.has_value()) {
            return;
        }
        auto& arm = *radiusFillArmed_;
        if (arm.centerX != centerX || arm.centerZ != centerZ) {
            return;
        }
        if (residentCount < arm.expectedChunkCount) {
            return;
        }
        const double fillMs =
            std::chrono::duration<double, std::milli>(now - arm.startedAt).count();
        radiusFill_.push({arm.centerX, arm.centerZ, arm.radius, fillMs, arm.expectedChunkCount});
        radiusFillArmed_.reset();
    }

    // --- streamingFrameCost -------------------------------------------------
    void recordFrameCost(double queueBatchMs, double uploadPrepMs, std::size_t sectionsUploaded) {
        std::scoped_lock lock{mutex_};
        streamingFrameCost_.push({queueBatchMs, uploadPrepMs, sectionsUploaded});
    }

    [[nodiscard]] std::vector<FirstMeshLatencySample> firstMeshSamples() const {
        std::scoped_lock lock{mutex_};
        return firstMesh_.samples();
    }
    [[nodiscard]] std::vector<RadiusFillSample> radiusFillSamples() const {
        std::scoped_lock lock{mutex_};
        return radiusFill_.samples();
    }
    [[nodiscard]] std::vector<StreamingFrameCostSample> streamingFrameCostSamples() const {
        std::scoped_lock lock{mutex_};
        return streamingFrameCost_.samples();
    }

    void clear() {
        std::scoped_lock lock{mutex_};
        firstMesh_.clear();
        radiusFill_.clear();
        streamingFrameCost_.clear();
        firstMeshArmed_.clear();
        radiusFillArmed_.reset();
    }

  private:
    struct RadiusFillArm final {
        int centerX = 0;
        int centerZ = 0;
        int radius = 0;
        std::size_t expectedChunkCount = 0;
        Clock::time_point startedAt{};
    };

    mutable std::mutex mutex_;
    std::unordered_map<TraceSectionKey, Clock::time_point, TraceSectionKeyHash> firstMeshArmed_;
    std::optional<RadiusFillArm> radiusFillArmed_;
    SampleRing<FirstMeshLatencySample, 512> firstMesh_;
    SampleRing<RadiusFillSample, 64> radiusFill_;
    SampleRing<StreamingFrameCostSample, 1024> streamingFrameCost_;
};

[[nodiscard]] inline ChunkStreamingMetrics& chunkStreamingMetrics() {
    static ChunkStreamingMetrics instance;
    return instance;
}

// ---------------------------------------------------------------------------
// 2. Missing-chunk / missing-mesh detector
// ---------------------------------------------------------------------------

// A chunk the detector expects to hold at least one resident section mesh
// (or a confirmed-empty verdict) but does not, past a grace period. This is
// evidence for README problem ③'s three suspects; it does not diagnose which
// suspect fired, only that a gap occurred and how long it has been open.
struct MissingChunkReport final {
    int chunkX = 0;
    int chunkZ = 0;
    double openMs = 0.0;
};

// Tracks, per chunk position: "has this chunk delivered CPU data?" and "has
// at least one of its non-empty sections reached the GPU (or been confirmed
// entirely empty)?". A chunk that has CPU data but never reaches either
// resolution within the grace window is reported as missing.
class MissingChunkDetector final {
  public:
    using Clock = std::chrono::steady_clock;

    // Called when a chunk's CPU data is delivered (ChunkDataUpdate, not a
    // removal). Chunks not already tracked start the clock here.
    void noteChunkDelivered(int chunkX, int chunkZ, Clock::time_point deliveredAt) {
        std::scoped_lock lock{mutex_};
        const Key key{chunkX, chunkZ};
        auto& entry = tracked_[key];
        entry.deliveredAt = deliveredAt;
        entry.resolved = false;
    }
    // Called when a chunk is unloaded: stop tracking it entirely so an
    // intentional unload never counts as "missing".
    void noteChunkRemoved(int chunkX, int chunkZ) {
        std::scoped_lock lock{mutex_};
        tracked_.erase(Key{chunkX, chunkZ});
    }
    // Called when any section belonging to the chunk becomes GPU-visible
    // (non-empty upload), or when every section of the chunk is confirmed
    // empty (no renderable geometry — e.g. an all-air ocean-floor chunk).
    void noteChunkResolved(int chunkX, int chunkZ) {
        std::scoped_lock lock{mutex_};
        const auto found = tracked_.find(Key{chunkX, chunkZ});
        if (found != tracked_.end()) {
            found->second.resolved = true;
        }
    }

    // Scans tracked chunks and returns those still unresolved past
    // `graceMs`. Read-only; does not mutate tracking state, so it can be
    // polled repeatedly (e.g. once per frame) without affecting the result.
    [[nodiscard]] std::vector<MissingChunkReport> scanMissing(Clock::time_point now,
                                                               double graceMs) const {
        std::scoped_lock lock{mutex_};
        std::vector<MissingChunkReport> reports;
        for (const auto& [key, entry] : tracked_) {
            if (entry.resolved) {
                continue;
            }
            const double openMs =
                std::chrono::duration<double, std::milli>(now - entry.deliveredAt).count();
            if (openMs >= graceMs) {
                reports.push_back({key.chunkX, key.chunkZ, openMs});
            }
        }
        std::ranges::sort(reports, {}, [](const MissingChunkReport& report) {
            return -report.openMs;
        });
        return reports;
    }

    [[nodiscard]] std::size_t trackedCount() const {
        std::scoped_lock lock{mutex_};
        return tracked_.size();
    }

    void clear() {
        std::scoped_lock lock{mutex_};
        tracked_.clear();
    }

  private:
    struct Key final {
        int chunkX = 0;
        int chunkZ = 0;
        [[nodiscard]] bool operator==(const Key&) const = default;
    };
    struct KeyHash final {
        [[nodiscard]] std::size_t operator()(const Key& key) const noexcept {
            std::size_t seed = std::hash<int>{}(key.chunkX);
            seed ^= std::hash<int>{}(key.chunkZ) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
            return seed;
        }
    };
    struct Entry final {
        Clock::time_point deliveredAt{};
        bool resolved = false;
    };

    mutable std::mutex mutex_;
    std::unordered_map<Key, Entry, KeyHash> tracked_;
};

[[nodiscard]] inline MissingChunkDetector& missingChunkDetector() {
    static MissingChunkDetector instance;
    return instance;
}

// ---------------------------------------------------------------------------
// 3. Delivery-order trace
// ---------------------------------------------------------------------------

// One section's GPU-visible delivery event, in the order the render thread
// actually uploaded it (README problem ②: "is delivery a strict ring
// expansion, or a jumbled band?").
struct DeliveryEvent final {
    int chunkX = 0;
    int chunkZ = 0;
    // Chebyshev ring distance from the request centre in effect when this
    // section was uploaded (matches ChunkStreamer's orderByDistance metric).
    int ring = 0;
    std::uint64_t sequence = 0;
};

class DeliveryOrderTrace final {
  public:
    void record(int chunkX, int chunkZ, int ring) {
        std::scoped_lock lock{mutex_};
        events_.push({chunkX, chunkZ, ring, nextSequence_++});
    }

    [[nodiscard]] std::vector<DeliveryEvent> events() const {
        std::scoped_lock lock{mutex_};
        return events_.samples();
    }

    // Verifies delivery is a monotonic ring expansion: once ring N has been
    // seen, no later event may report a ring smaller than N minus the given
    // slack. Slack exists because sections inside one already-fetched batch
    // can complete meshing out of order (README problem ②'s "batch-internal"
    // caveat); it does not excuse a whole ring regressing.
    [[nodiscard]] bool isMonotonicRingExpansion(int slack = 0) const {
        std::scoped_lock lock{mutex_};
        int maxRingSeen = -1;
        for (const auto& event : events_.samples()) {
            if (event.ring < maxRingSeen - slack) {
                return false;
            }
            maxRingSeen = std::max(maxRingSeen, event.ring);
        }
        return true;
    }

    void clear() {
        std::scoped_lock lock{mutex_};
        events_.clear();
        nextSequence_ = 0U;
    }

  private:
    mutable std::mutex mutex_;
    SampleRing<DeliveryEvent, 4096> events_;
    std::uint64_t nextSequence_ = 0U;
};

[[nodiscard]] inline DeliveryOrderTrace& deliveryOrderTrace() {
    static DeliveryOrderTrace instance;
    return instance;
}

// Resets every CS-1 recorder. Intended for test isolation between cases in
// the same process, and for a fresh session when the render thread rebuilds
// its streaming state (world reset).
inline void resetChunkStreamingTrace() {
    chunkStreamingMetrics().clear();
    missingChunkDetector().clear();
    deliveryOrderTrace().clear();
}

} // namespace mc::diag
