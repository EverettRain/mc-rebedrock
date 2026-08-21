#pragma once

// The redstone-wire power model (W-4), signal-derived from RedStoneWireBlock /
// RedstoneWireEvaluator. Only the *semantics* are taken from Java — the wire's
// POWER attenuates one per cell, a wire reads the strongest of its non-wire
// source signals and its neighbours' POWER minus one. The *algorithm* here is a
// deliberately naive serial relaxation (correctness first); W-5 replaces it with
// a self-authored AC-graph evaluator whose output must equal this, bit for bit.
//
// A wire has no per-cell delay: a source change re-solves the whole connected
// network at once. This computes that steady-state distribution for the network
// a cell belongs to; WorldSimulation applies it.

#include "gameplay/RedstoneSignal.hpp"

#include "world/Block.hpp"
#include "world/BlockPos.hpp"
#include "world/World.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mc::gameplay::redstone {

[[nodiscard]] inline bool isWire(const world::World& world, world::BlockPos pos) {
    return world.block(pos.x, pos.y, pos.z) == world::Block::RedstoneWire;
}

// The signal a wire cell picks up from its non-wire neighbours (source dust does
// not feed itself; wire-to-wire travels through the attenuation instead). This
// is RedstoneWireEvaluator.getBlockSignal in spirit: the best neighbour signal
// with wire neighbours excluded.
[[nodiscard]] inline int wireBlockSignal(const world::World& world, world::BlockPos pos) {
    int best = 0;
    for (const Direction dir : kAllDirections) {
        const world::BlockPos neighbor = relative(pos, dir);
        if (world.block(neighbor.x, neighbor.y, neighbor.z) == world::Block::RedstoneWire) {
            continue;
        }
        best = std::max(best, getSignal(world, neighbor, dir));
        if (best >= 15) {
            return 15;
        }
    }
    return best;
}

struct WirePower final {
    world::BlockPos pos;
    int power = 0;
};

// The steady-state POWER of every wire in the network `start` belongs to. Naive:
// flood the connected wires, seed each with its block signal, then relax
// POWER[cell] = max(blockSignal, neighbourPOWER - 1) to a fixed point. Every
// steady state is uniquely determined by the attenuation law, so the order of
// relaxation does not matter.
[[nodiscard]] inline std::vector<WirePower> computeWireNetwork(const world::World& world,
                                                              world::BlockPos start) {
    if (!isWire(world, start)) {
        return {};
    }
    // Wire-to-wire adjacency: the six orthogonal cells (the up/down-step
    // conductor rules are a later refinement; direct vertical neighbours already
    // connect here).
    const auto& adjacency = kDirectionOffsets;

    std::vector<world::BlockPos> cells;
    std::unordered_map<std::int64_t, std::size_t> indexOf;
    std::vector<std::size_t> frontier;
    indexOf.emplace(packBlockPos(start), 0);
    cells.push_back(start);
    frontier.push_back(0);
    while (!frontier.empty()) {
        const world::BlockPos current = cells[frontier.back()];
        frontier.pop_back();
        for (const auto& offset : adjacency) {
            const world::BlockPos neighbor{current.x + offset.x, current.y + offset.y,
                                           current.z + offset.z};
            if (!isWire(world, neighbor)) {
                continue;
            }
            const std::int64_t key = packBlockPos(neighbor);
            if (indexOf.contains(key)) {
                continue;
            }
            indexOf.emplace(key, cells.size());
            frontier.push_back(cells.size());
            cells.push_back(neighbor);
        }
    }

    std::vector<int> blockSignal(cells.size());
    std::vector<int> power(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i) {
        blockSignal[i] = wireBlockSignal(world, cells[i]);
        power[i] = blockSignal[i];
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            int value = blockSignal[i];
            for (const auto& offset : adjacency) {
                const world::BlockPos neighbor{cells[i].x + offset.x, cells[i].y + offset.y,
                                               cells[i].z + offset.z};
                const auto found = indexOf.find(packBlockPos(neighbor));
                if (found != indexOf.end()) {
                    value = std::max(value, power[found->second] - 1);
                }
            }
            value = std::clamp(value, 0, 15);
            if (value != power[i]) {
                power[i] = value;
                changed = true;
            }
        }
    }

    std::vector<WirePower> result;
    result.reserve(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i) {
        result.push_back({cells[i], power[i]});
    }
    return result;
}

} // namespace mc::gameplay::redstone
