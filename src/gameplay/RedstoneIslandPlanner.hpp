#pragma once

// W-6 (island-analysis scope): the World-facing adapter that turns a gametick's
// due redstone ticks into footprints the pure IslandPartitioner can group. It is
// the one place that reads the World: it looks up each due component's coupling
// descriptor and expands it into cells — a Chebyshev ball for a fixed-reach
// component, or the flooded connected network (plus a one-cell ring) for a wire —
// then hands the partitioner the total order to drain.
//
// Keeping the flooding here, out of the partitioner, is what lets the partitioner
// stay a pure, world-free unit. This adapter is the piece a threaded W-6 would
// grow a snapshot around; today it just feeds the single-threaded island drain
// and the lockstep gate.

#include "gameplay/ChunkTickScheduler.hpp"
#include "gameplay/RedstoneCoupling.hpp"
#include "gameplay/RedstoneIslandPartition.hpp"
#include "gameplay/RedstoneSignal.hpp"

#include "world/Block.hpp"
#include "world/BlockPos.hpp"
#include "world/World.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace mc::gameplay::redstone {

class RedstoneIslandPlanner final {
  public:
    // Builds the island-major drain plan for `due` (a snapshot of every
    // RedstoneComponent tick due this gametick). `planOut` receives the ticks in
    // drain order; `islandOffsetsOut` the island boundaries. Both are cleared.
    void plan(const world::World& world, std::span<const ScheduledTick> due,
              std::vector<ScheduledTick>& planOut, std::vector<std::size_t>& islandOffsetsOut) {
        partitioner_.reset();
        planWireCells_.clear();

        for (const ScheduledTick& tick : due) {
            const auto pos = tick.position;
            const auto block = world.block(pos.x, pos.y, pos.z);
            const RedstoneCoupling coupling = redstoneCoupling(block);
            const std::size_t handle = partitioner_.addTick(tick);

            if (coupling.globalCoupling) {
                partitioner_.markGlobal(handle);
            }

            if (coupling.wireNetwork && block == world::Block::RedstoneWire) {
                addWireNetworkFootprint(world, handle, pos);
            } else {
                addBallFootprint(handle, pos, coupling.reach);
            }
        }

        partitioner_.finalize(planOut, islandOffsetsOut);
    }

    [[nodiscard]] std::size_t islandCount() const { return partitioner_.islandCount(); }

  private:
    // A solid Chebyshev ball of radius `reach` around `pos` — the fixed footprint
    // of a component whose tick reads and writes within `reach` cells. A reach of
    // 0 registers only the cell itself.
    void addBallFootprint(std::size_t handle, SimulationPosition pos, std::uint8_t reach) {
        const int r = static_cast<int>(reach);
        for (int dx = -r; dx <= r; ++dx) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dz = -r; dz <= r; ++dz) {
                    partitioner_.addFootprintCell(
                        handle, world::packBlockPos(pos.x + dx, pos.y + dy, pos.z + dz));
                }
            }
        }
    }

    // Floods the connected redstone_wire network from `pos` (6-orthogonal, the
    // adjacency the wire evaluator uses) and registers every network cell plus a
    // one-cell ring as the wire tick's footprint. The ring captures the conductors
    // and components a wire reads and powers just outside the dust. Networks are
    // flooded once per plan: a later wire tick in the same network claims a cell
    // already owned and unions through it without re-flooding.
    void addWireNetworkFootprint(const world::World& world, std::size_t handle,
                                 SimulationPosition pos) {
        const std::int64_t start = world::packBlockPos(pos.x, pos.y, pos.z);
        // Always claim the start cell so a wire tick whose network another tick
        // already flooded still unions into it.
        addRing(handle, pos);
        if (planWireCells_.contains(start)) {
            return;
        }
        floodStack_.clear();
        floodStack_.push_back({pos.x, pos.y, pos.z});
        planWireCells_.insert(start);
        while (!floodStack_.empty()) {
            const world::BlockPos cell = floodStack_.back();
            floodStack_.pop_back();
            addRing(handle, {cell.x, cell.y, cell.z});
            for (const Direction dir : kAllDirections) {
                const world::BlockPos next = relative(cell, dir);
                if (world.block(next.x, next.y, next.z) != world::Block::RedstoneWire) {
                    continue;
                }
                const std::int64_t packed = world::packBlockPos(next.x, next.y, next.z);
                if (planWireCells_.insert(packed).second) {
                    floodStack_.push_back(next);
                }
            }
        }
    }

    // Registers a cell and its six orthogonal neighbours as footprint of `handle`
    // — a wire cell reads the signal at, and powers, its immediate neighbours.
    void addRing(std::size_t handle, SimulationPosition cell) {
        partitioner_.addFootprintCell(handle, world::packBlockPos(cell.x, cell.y, cell.z));
        for (const Direction dir : kAllDirections) {
            const world::BlockPos neighbor =
                relative({cell.x, cell.y, cell.z}, dir);
            partitioner_.addFootprintCell(handle,
                                          world::packBlockPos(neighbor.x, neighbor.y, neighbor.z));
        }
    }

    IslandPartitioner partitioner_;
    std::vector<world::BlockPos> floodStack_;
    std::unordered_set<std::int64_t> planWireCells_;
};

} // namespace mc::gameplay::redstone
