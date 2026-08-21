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
#include <span>
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
    // A redstone component's scheduled tick (torch toggle, repeater/comparator
    // flip). One task for every component so they drain in the single
    // (dueTick, priority, subTickOrder) order Java's LevelTicks gives them —
    // diodes schedule at HIGH so they run before the ordinary ticks around them.
    RedstoneComponent,
    Count,
};

inline constexpr std::size_t kTickTaskCount = static_cast<std::size_t>(TickTask::Count);

// The tie-breaker between ticks due on the same game tick, bit-for-bit Java's
// TickPriority. Redstone leans on it: a repeater or comparator schedules at HIGH
// so it updates *before* the ordinary ticks around it ("face the diode first"),
// which is what makes a clock or a piston door behave the same tick after tick.
// The stored value is the JE ordinal (EXTREMELY_HIGH = -3 … EXTREMELY_LOW = 3);
// draining sorts by it ascending, so a more negative value fires first.
enum class TickPriority : std::int8_t {
    ExtremelyHigh = -3,
    VeryHigh = -2,
    High = -1,
    Normal = 0,
    Low = 1,
    VeryLow = 2,
    ExtremelyLow = 3,
};

struct ScheduledTick final {
    SimulationPosition position;
    std::uint64_t dueTick = 0U;
    // The diode-first tie-break within a game tick (JE TickPriority).
    TickPriority priority = TickPriority::Normal;
    // Insertion order across every chunk, so draining can restore the global
    // FIFO the flat queues had. This is JE's `subTickOrder`: a monotonic counter
    // that orders ticks sharing a dueTick *and* a priority.
    std::uint64_t sequence = 0U;
};

// The serialisable form of a scheduled tick, a one-for-one map of JE's
// `SavedTick(type, pos, delay, priority)`. The key difference from ScheduledTick
// is `delay`, which is **relative** — `dueTick - gameTime` — so a saved tick
// keeps its remaining wait whatever game time it is reloaded at, exactly as JE
// stores `block_ticks`/`fluid_ticks`. `subTickOrder` is deliberately not stored:
// it is re-derived from the order ticks are imported in, which is why the
// exporter must emit them already in drain order.
struct SavedTick final {
    TickTask type = TickTask::FallingBlock;
    SimulationPosition position;
    std::int32_t delay = 0;
    TickPriority priority = TickPriority::Normal;

    [[nodiscard]] bool operator==(const SavedTick&) const = default;
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
    //
    // `priority` orders ticks sharing a dueTick: HIGH for diodes so they update
    // before their neighbours. It defaults to NORMAL, so every existing caller —
    // sand, fluid, support, decay, growth — keeps the exact FIFO it had.
    bool schedule(TickTask task, SimulationPosition position, std::uint64_t dueTick,
                  bool allowDuplicates = false, TickPriority priority = TickPriority::Normal);

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
            // The deterministic drain order, restored: chunk buckets are an
            // unordered map, so the total order — JE's (triggerTick, priority,
            // subTickOrder) — has to be rebuilt from the entries themselves.
            std::ranges::sort(due, drainOrder);
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

    // JE's (triggerTick, priority, subTickOrder) total order over ticks. Public
    // so the persistence codec can emit ticks in the exact order they will be
    // drained in — which is what lets a save drop subTickOrder and rebuild it
    // from list position on load.
    [[nodiscard]] static bool drainOrder(const ScheduledTick& left, const ScheduledTick& right) {
        if (left.dueTick != right.dueTick) {
            return left.dueTick < right.dueTick;
        }
        if (left.priority != right.priority) {
            return static_cast<std::int8_t>(left.priority) < static_cast<std::int8_t>(right.priority);
        }
        return left.sequence < right.sequence;
    }

    // Every pending tick as a JE-shaped SavedTick, in drain order, with `delay`
    // relative to `gameTime`. This is the lossless map to `block_ticks`: a
    // converter or a per-chunk save writes exactly these, and importSavedTicks
    // reconstructs the identical drain order. Passing a chunk restricts the
    // export to that chunk (JE saves ticks per chunk); the default exports all.
    [[nodiscard]] std::vector<SavedTick> exportSavedTicks(std::uint64_t gameTime) const;
    [[nodiscard]] std::vector<SavedTick> exportSavedTicks(int chunkX, int chunkZ,
                                                          std::uint64_t gameTime) const;

    // Re-schedules a run of SavedTicks, resolving each delay back to an absolute
    // dueTick (`gameTime + delay`) and assigning subTickOrder from list position
    // — so importing what exportSavedTicks produced restores the same order.
    void importSavedTicks(std::uint64_t gameTime, std::span<const SavedTick> ticks);

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
