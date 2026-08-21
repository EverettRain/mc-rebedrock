#pragma once

// W-6 (island-analysis scope): the deterministic island partitioner. Given the
// redstone component ticks due on a gametick, each described only by its position
// and the footprint cells its tick can touch, it groups them into islands —
// connected components that cannot influence each other this tick — and emits a
// single total order over the ticks: island-major, islands sorted by their
// minimum (chunkPos, packed pos) member, ticks within an island in JE drain
// order.
//
// This is the machinery a threaded evaluator would use to hand one island per
// worker and merge the results deterministically. Here it stays single-threaded:
// its only job is to *reorder* the serial drain into island groups, so the
// lockstep gate can prove that reordering is bit-for-bit identical to the flat
// serial order — which holds exactly when the partition is sound. A wrong
// partition (a coupled pair split into two islands) reorders work that was
// order-dependent, and the lockstep diff catches it.
//
// The type is deliberately pure: it knows nothing about the World. The caller
// (RedstoneIslandPlanner) turns component footprints — a Chebyshev ball, or a
// flooded wire network — into the cells registered here, which keeps this unit
// testable in isolation from synthetic footprints.

#include "gameplay/ChunkTickScheduler.hpp"
#include "gameplay/SimulationPosition.hpp"
#include "world/BlockPos.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mc::gameplay::redstone {

class IslandPartitioner final {
  public:
    // Clears the previous partition, keeping the arenas allocated so a warm
    // partitioner does not allocate per tick.
    void reset() {
        ticks_.clear();
        parent_.clear();
        rank_.clear();
        cellOwner_.clear();
        hasGlobal_ = false;
    }

    // Registers a due tick and returns its handle. Footprint cells and the global
    // flag are attached to the handle afterwards.
    [[nodiscard]] std::size_t addTick(const ScheduledTick& tick) {
        const std::size_t handle = ticks_.size();
        ticks_.push_back(tick);
        parent_.push_back(handle);
        rank_.push_back(0U);
        return handle;
    }

    // A cell the tick's footprint touches. The first tick to claim a cell owns it;
    // a later tick claiming the same cell is coupled to the owner, so the two are
    // unioned into one island.
    void addFootprintCell(std::size_t handle, std::int64_t packedCell) {
        const auto [it, inserted] = cellOwner_.try_emplace(packedCell, handle);
        if (!inserted) {
            unite(handle, it->second);
        }
    }

    // The tick couples through a channel no footprint can bound (a block event,
    // a piston move): the whole due set collapses to one island in finalize.
    void markGlobal(std::size_t handle) {
        hasGlobal_ = true;
        globalAnchor_ = handle;
    }

    [[nodiscard]] std::size_t tickCount() const { return ticks_.size(); }

    // Produces the island-major total order. `planOut` receives every due tick in
    // the order they should be drained; `islandOffsetsOut` receives the start
    // index of each island in `planOut` (so `planOut[offsets[i] .. offsets[i+1])`
    // is island i) — the boundaries a threaded evaluator would parallelise over
    // and the merge would reassemble. Both are cleared first.
    void finalize(std::vector<ScheduledTick>& planOut, std::vector<std::size_t>& islandOffsetsOut) {
        planOut.clear();
        islandOffsetsOut.clear();
        if (ticks_.empty()) {
            return;
        }
        if (hasGlobal_) {
            for (std::size_t handle = 0U; handle < ticks_.size(); ++handle) {
                unite(handle, globalAnchor_);
            }
        }

        // Group handles by their union-find root.
        std::unordered_map<std::size_t, std::size_t> rootToIsland;
        islands_.clear();
        for (std::size_t handle = 0U; handle < ticks_.size(); ++handle) {
            const std::size_t root = find(handle);
            const auto [it, inserted] = rootToIsland.try_emplace(root, islands_.size());
            if (inserted) {
                islands_.emplace_back();
            }
            islands_[it->second].push_back(ticks_[handle]);
        }

        // Within each island: JE drain order. Between islands: the minimum
        // (chunkPos, packed pos) member, so the island order is a deterministic
        // function of the circuit, not of hash iteration.
        order_.clear();
        for (std::size_t island = 0U; island < islands_.size(); ++island) {
            auto& members = islands_[island];
            std::sort(members.begin(), members.end(), ChunkTickScheduler::drainOrder);
            order_.push_back({islandKey(members.front().position), island});
        }
        std::sort(order_.begin(), order_.end(),
                  [](const OrderedIsland& left, const OrderedIsland& right) {
                      return left.key < right.key;
                  });

        for (const OrderedIsland& entry : order_) {
            islandOffsetsOut.push_back(planOut.size());
            const auto& members = islands_[entry.island];
            planOut.insert(planOut.end(), members.begin(), members.end());
        }
    }

    // The number of islands the last finalize produced — the parallelism the
    // partition exposes, which the benchmark and the multi-island test read.
    [[nodiscard]] std::size_t islandCount() const { return islands_.size(); }

  private:
    // The between-island sort key: (chunkX, chunkZ, packed pos). Chunk-major so
    // the order matches the (chunkPos, packed pos) total order the design pins,
    // and stable regardless of how the union-find roots fell out.
    using IslandKey = std::tuple<int, int, std::int64_t>;

    struct OrderedIsland final {
        IslandKey key;
        std::size_t island;
    };

    [[nodiscard]] static IslandKey islandKey(SimulationPosition pos) {
        return {pos.x >> 4, pos.z >> 4, world::packBlockPos(pos.x, pos.y, pos.z)};
    }

    [[nodiscard]] std::size_t find(std::size_t handle) {
        while (parent_[handle] != handle) {
            parent_[handle] = parent_[parent_[handle]];
            handle = parent_[handle];
        }
        return handle;
    }

    void unite(std::size_t a, std::size_t b) {
        std::size_t rootA = find(a);
        std::size_t rootB = find(b);
        if (rootA == rootB) {
            return;
        }
        if (rank_[rootA] < rank_[rootB]) {
            std::swap(rootA, rootB);
        }
        parent_[rootB] = rootA;
        if (rank_[rootA] == rank_[rootB]) {
            ++rank_[rootA];
        }
    }

    std::vector<ScheduledTick> ticks_;
    std::vector<std::size_t> parent_;
    std::vector<std::size_t> rank_;
    std::unordered_map<std::int64_t, std::size_t> cellOwner_;
    bool hasGlobal_ = false;
    std::size_t globalAnchor_ = 0U;

    // finalize arenas, reused across ticks.
    std::vector<std::vector<ScheduledTick>> islands_;
    std::vector<OrderedIsland> order_;
};

} // namespace mc::gameplay::redstone
