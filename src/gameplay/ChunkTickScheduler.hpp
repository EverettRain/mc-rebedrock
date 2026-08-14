#pragma once

// Scheduled block ticks, stored per chunk.
//
// WorldSimulation used to keep five parallel containers — falling sand, fluid
// updates, support checks, leaf decay, tree growth — each a deque or vector of
// `{position, dueTick}` beside its own `unordered_set` for de-duplication. They
// were already the same shape five times over, which is why the plan calls this
// a convergence rather than a rewrite.
//
// Keying by chunk is what 26.1's LevelTicks does, and it buys two things the
// flat containers could not:
//
//   * **Unloading.** A position in a chunk the streamer has dropped used to sit
//     in the queue forever, and firing it later resurrected work in a chunk
//     that no longer existed. `forgetChunk` drops the whole bucket at once.
//   * **Persistence.** Scheduled ticks are per-chunk data, so they can be saved
//     and restored with the chunk (C5). A flat global queue cannot be.
//
// **Processing order is deliberately unchanged.** Bucketing by chunk would
// otherwise reorder the work — sand in one chunk falling before sand queued
// earlier in another — and falling blocks and fluid spread are order-sensitive.
// Every entry therefore carries a monotonic sequence number, and draining sorts
// the due entries by it, reproducing the exact global FIFO the flat deques had.

#include "gameplay/SimulationPosition.hpp"
#include "world/WorldConstants.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mc::gameplay {

// The kinds of work a cell can have scheduled. One enum rather than one
// container per kind: the budget, the de-duplication and the unload sweep are
// identical for all of them, and only the handler differs.
enum class TickTask : std::uint8_t {
    FallingBlock,
    Fluid,
    SupportCheck,
    LeafDecay,
    TreeGrowth,
    Count,
};

inline constexpr std::size_t kTickTaskCount = static_cast<std::size_t>(TickTask::Count);

struct ScheduledTick final {
    SimulationPosition position;
    std::uint64_t dueTick = 0U;
    // Insertion order across every chunk, so draining can restore the global
    // FIFO the flat queues had.
    std::uint64_t sequence = 0U;
};

class ChunkTickScheduler final {
  public:
    // Schedules `position` for `task` at `dueTick`. Returns false when the cell
    // already has that task pending, which is the de-duplication the separate
    // `unordered_set`s used to provide.
    //
    // `allowDuplicates` exists for falling blocks, whose flat deque never
    // de-duplicated: the same cell could legitimately be queued twice and each
    // entry re-checked against the world when it fired. Keeping that means this
    // conversion changes nothing about when sand falls.
    bool schedule(TickTask task, SimulationPosition position, std::uint64_t dueTick,
                  bool allowDuplicates = false);

    // Hands every entry of `task` that is due at `now` to `handler`, oldest
    // first, up to `budget` of them. Each entry is removed just before its
    // handler runs, so a handler that re-schedules the same cell works.
    //
    // Two subtleties, both of them behaviour the flat queues had:
    //
    //   * Entries are taken **one at a time**, not lifted out as a batch. A
    //     handler often wakes its neighbours, and a neighbour still sitting in
    //     the queue must stay a single pending entry — batch-removing first
    //     would let the wake re-schedule a cell whose handler had not run yet,
    //     doubling the work and eating the next tick's budget.
    //   * Work a handler schedules as **immediately due** is picked up in the
    //     same drain. Support checks rely on exactly that: popping an
    //     unsupported block strands the next one along, and that cascade has to
    //     resolve within the tick.
    template <typename Handler>
    std::size_t drainDue(TickTask task, std::uint64_t now, std::size_t budget, Handler&& handler) {
        const auto index = static_cast<std::size_t>(task);
        // Most ticks have nothing scheduled for most tasks, and the collect
        // below walks every chunk bucket. One counter turns that into an O(1)
        // early out, which is what keeps per-chunk storage from costing more
        // than the flat deques it replaced.
        std::size_t processed = 0U;
        while (processed < budget && totals_[index] != 0U) {
            auto& due = scratch_;
            due.clear();
            for (auto& [chunk, bucket] : chunks_) {
                for (const auto& entry : bucket.tasks[index]) {
                    if (entry.dueTick <= now) {
                        due.push_back(entry);
                    }
                }
            }
            if (due.empty()) {
                break;
            }
            // The global FIFO the flat deques had, restored: chunk buckets are
            // an unordered map, so insertion order has to come from the entry.
            std::ranges::sort(due, {}, &ScheduledTick::sequence);
            const std::size_t batch = std::min(due.size(), budget - processed);
            // Copied out before handling: a handler mutates the buckets, and
            // scratch_ is refilled by the next outer pass anyway.
            positions_.assign(due.begin(), due.begin() + static_cast<std::ptrdiff_t>(batch));
            for (const auto& entry : positions_) {
                remove(task, entry.position);
                handler(entry.position);
                ++processed;
            }
        }
        return processed;
    }

    [[nodiscard]] std::size_t pending(TickTask task) const {
        return totals_[static_cast<std::size_t>(task)];
    }
    [[nodiscard]] bool contains(TickTask task, SimulationPosition position) const;

    // Drops every scheduled tick in a chunk the world no longer holds. Without
    // this, a queue entry outlives its chunk and fires against cells that are
    // not loaded.
    void forgetChunk(int chunkX, int chunkZ);

    // Every chunk that currently holds scheduled work, for persistence and for
    // the unload sweep.
    [[nodiscard]] std::vector<std::pair<int, int>> scheduledChunks() const;

    void clear();

  private:
    struct ChunkKey final {
        int x = 0;
        int z = 0;
        [[nodiscard]] bool operator==(const ChunkKey&) const = default;
    };
    struct ChunkKeyHash final {
        [[nodiscard]] std::size_t operator()(const ChunkKey& key) const noexcept {
            return static_cast<std::size_t>(static_cast<std::uint32_t>(key.x)) * 0x9E3779B1U ^
                   static_cast<std::size_t>(static_cast<std::uint32_t>(key.z)) * 0x85EBCA77U;
        }
    };
    struct Bucket final {
        std::array<std::vector<ScheduledTick>, kTickTaskCount> tasks;
        std::array<std::unordered_set<SimulationPosition, SimulationPositionHash>, kTickTaskCount>
            queued;
        [[nodiscard]] bool empty() const;
    };

    [[nodiscard]] static ChunkKey keyOf(SimulationPosition position);
    void remove(TickTask task, SimulationPosition position);

    std::unordered_map<ChunkKey, Bucket, ChunkKeyHash> chunks_;
    // Entries per task across every chunk, so an empty task costs one compare.
    std::array<std::size_t, kTickTaskCount> totals_{};
    std::uint64_t nextSequence_ = 0U;
    // Reused across drains so a tick does not allocate.
    std::vector<ScheduledTick> scratch_;
    std::vector<ScheduledTick> positions_;
};

} // namespace mc::gameplay
